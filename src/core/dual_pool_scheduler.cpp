#include "omni/core/dual_pool_scheduler.hpp"
#include "omni/engines/conversion_engine.hpp"
#include "omni/infra/portable_env.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>

namespace omni::core {

void DualPoolScheduler::executeBatch(
    const std::vector<TaskItem>& tasks,
    const ConversionOptions& options,
    const TaskProgressFn& onProgress,
    const TaskFinishedFn& onTaskFinished,
    const BatchFinishedFn& onBatchFinished) {

    cancelled_.store(false, std::memory_order_relaxed);

    if (tasks.empty()) {
        if (onBatchFinished) onBatchFinished(0, 0, 0);
        return;
    }

    // Check if this batch is an N-to-1 aggregate operation (Merge images -> PDF, Merge PDFs, Hash Manifest)
    if (isAggregateBatchAction(tasks.front().mode, options)) {
        std::uintmax_t sumInBytes = 0;
        for (const auto& t : tasks) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(t.inputPath, ec)) {
                sumInBytes += std::filesystem::file_size(t.inputPath, ec);
            }
            if (onProgress) onProgress(t.rowId, 5, "等待汇总...");
        }

        auto itemProg = [&](std::size_t idx, int pct) {
            if (idx < tasks.size() && onProgress) {
                onProgress(tasks[idx].rowId, pct, "汇总处理中 " + std::to_string(pct) + "%");
            }
        };
        auto cancelCb = [&]() -> bool { return isCancelled(); };

        try {
            const std::filesystem::path mergedOut =
                omni::engines::ConversionEngine::executeAggregateTasks(tasks, options, itemProg, cancelCb);

            std::error_code ec;
            const std::uintmax_t outBytes = std::filesystem::is_regular_file(mergedOut, ec)
                                                ? std::filesystem::file_size(mergedOut, ec)
                                                : 0;
            const std::string outName = pathToUtf8(mergedOut.filename());

            for (std::size_t i = 0; i < tasks.size(); ++i) {
                TaskResult res;
                res.rowId = tasks[i].rowId;
                res.ok = true;
                res.outputPath = mergedOut;
                res.outputBytes = (i == 0) ? outBytes : 0;
                res.message = "✅ 已汇总至 " + outName + " (双击定位)";
                if (onTaskFinished) onTaskFinished(res);
            }

            const std::uintmax_t saved = (sumInBytes > outBytes && outBytes > 0) ? (sumInBytes - outBytes) : 0;
            if (onBatchFinished) onBatchFinished(static_cast<int>(tasks.size()), 0, saved);
        } catch (const std::exception& ex) {
            const std::string errStr = ex.what();
            const bool wasCancelled = isCancelled() || errStr == "CANCELLED";
            for (const auto& t : tasks) {
                TaskResult res;
                res.rowId = t.rowId;
                res.ok = false;
                res.cancelled = wasCancelled;
                res.message = wasCancelled ? "⏹ 已中止" : ("❌ 失败: " + errStr);
                if (onTaskFinished) onTaskFinished(res);
            }
            if (onBatchFinished) onBatchFinished(0, wasCancelled ? 0 : static_cast<int>(tasks.size()), 0);
        }
        return;
    }

    // Standard per-file parallel execution
    bool hasMediaProcess = false;
    for (const auto& t : tasks) {
        if (t.mode == WorkMode::Audio || t.mode == WorkMode::Video) {
            hasMediaProcess = true;
            break;
        }
    }

    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    std::size_t workerCount = hasMediaProcess
                                  ? std::min<std::size_t>(tasks.size(), 2)
                                  : std::min<std::size_t>(tasks.size(), std::clamp<std::size_t>(hw / 2, 1, 4));

    std::mutex pathAllocMutex;
    std::atomic<std::size_t> nextIndex{0};
    std::atomic<int> successCnt{0};
    std::atomic<int> failCnt{0};
    std::atomic<std::uintmax_t> totalSavedBytes{0};

    auto buildSuffixTag = [&](WorkMode m) -> std::string {
        if (m == WorkMode::Novel) return "_book";
        if (m == WorkMode::Document) return "_converted";
        if (m == WorkMode::Toolbox) {
            if (options.toolAction == "exact_kb") return "_" + std::to_string(options.toolTargetKb) + "kb";
            if (options.toolAction == "id_watermark") return "_watermarked";
            if (options.toolAction == "subtitle") return "_sub";
            return "_out";
        }
        return "_" + compressionToKey(options.compLevel);
    };

    auto workerLoop = [&]() {
        while (true) {
            const std::size_t idx = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (idx >= tasks.size()) break;

            const TaskItem& task = tasks[idx];
            if (isCancelled()) {
                TaskResult res;
                res.rowId = task.rowId;
                res.ok = false;
                res.cancelled = true;
                res.message = "已取消";
                if (onTaskFinished) onTaskFinished(res);
                continue;
            }

            std::error_code ec;
            if (!std::filesystem::is_regular_file(task.inputPath, ec)) {
                failCnt.fetch_add(1, std::memory_order_relaxed);
                TaskResult res;
                res.rowId = task.rowId;
                res.ok = false;
                res.message = "源文件不存在";
                if (onTaskFinished) onTaskFinished(res);
                continue;
            }

            const std::uintmax_t inSize = std::filesystem::file_size(task.inputPath, ec);
            const bool createsFolderOnly = (task.mode == WorkMode::Document && options.docAction == "pdf_to_img");

            std::filesystem::path suggestedOutPath;
            if (!createsFolderOnly) {
                std::lock_guard<std::mutex> lk(pathAllocMutex);
                suggestedOutPath = omni::infra::PortableEnv::resolveSafeOutputPath(
                    task.inputPath, options.outputDir, options.targetFormat, buildSuffixTag(task.mode));
                if (workerCount > 1) {
                    FILE* fp = nullptr;
#ifdef _WIN32
                    _wfopen_s(&fp, suggestedOutPath.wstring().c_str(), L"wb");
#else
                    fp = std::fopen(suggestedOutPath.c_str(), "wb");
#endif
                    if (fp) std::fclose(fp);
                }
            }

            if (onProgress) {
                onProgress(task.rowId, 2, "准备中...");
            }

            auto progCb = [&](int pct) {
                if (onProgress) {
                    onProgress(task.rowId, pct, "处理中 " + std::to_string(pct) + "%");
                }
            };
            auto cancelCb = [&]() -> bool { return isCancelled(); };

            try {
                const std::filesystem::path actualOut =
                    omni::engines::ConversionEngine::executeTask(task, suggestedOutPath, options, progCb, cancelCb);

                std::error_code outEc;
                std::uintmax_t outSize = 0;
                if (std::filesystem::is_regular_file(actualOut, outEc)) {
                    outSize = std::filesystem::file_size(actualOut, outEc);
                } else if (std::filesystem::is_directory(actualOut, outEc)) {
                    for (const auto& entry : std::filesystem::directory_iterator(actualOut, outEc)) {
                        if (entry.is_regular_file(outEc)) {
                            outSize += entry.file_size(outEc);
                        }
                    }
                }

                if (inSize > outSize && outSize > 0) {
                    totalSavedBytes.fetch_add(inSize - outSize, std::memory_order_relaxed);
                }
                successCnt.fetch_add(1, std::memory_order_relaxed);

                TaskResult res;
                res.rowId = task.rowId;
                res.ok = true;
                res.outputPath = actualOut;
                res.message = "✅ 完成 (双击定位)";
                res.inputBytes = inSize;
                res.outputBytes = outSize;
                if (onTaskFinished) onTaskFinished(res);

            } catch (const std::exception& ex) {
                if (!suggestedOutPath.empty()) {
                    std::error_code rmEc;
                    std::filesystem::remove(suggestedOutPath, rmEc);
                }

                const std::string errStr = ex.what();
                if (isCancelled() || errStr == "CANCELLED") {
                    TaskResult res;
                    res.rowId = task.rowId;
                    res.ok = false;
                    res.cancelled = true;
                    res.message = "⏹ 已中止";
                    if (onTaskFinished) onTaskFinished(res);
                } else {
                    failCnt.fetch_add(1, std::memory_order_relaxed);
                    TaskResult res;
                    res.rowId = task.rowId;
                    res.ok = false;
                    res.message = "❌ 失败: " + errStr;
                    if (onTaskFinished) onTaskFinished(res);
                }
            }
        }
    };

    if (workerCount <= 1) {
        workerLoop();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(workerCount);
        for (std::size_t i = 0; i < workerCount; ++i) {
            pool.emplace_back(workerLoop);
        }
        for (auto& th : pool) {
            if (th.joinable()) th.join();
        }
    }

    if (onBatchFinished) {
        onBatchFinished(successCnt.load(), failCnt.load(), totalSavedBytes.load());
    }
}

} // namespace omni::core
