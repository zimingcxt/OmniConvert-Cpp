#include "omni/core/dual_pool_scheduler.hpp"
#include "omni/engines/conversion_engine.hpp"
#include "omni/infra/portable_env.hpp"

#include <algorithm>
#include <atomic>
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

    // Determine concurrency:
    // - Image & Novel tasks are in-process CPU/IO tasks -> scale up to 4 threads
    // - Audio & Video spawn multi-threaded FFmpeg subprocesses -> cap at 2 concurrent processes
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

    // Guard output path allocation so concurrent workers converting identically-named files
    // into a shared custom output directory never pick the same filename.
    std::mutex pathAllocMutex;
    std::atomic<std::size_t> nextIndex{0};
    std::atomic<int> successCnt{0};
    std::atomic<int> failCnt{0};
    std::atomic<std::uintmax_t> totalSavedBytes{0};

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
            const std::string suffixTag = (task.mode != WorkMode::Novel)
                                              ? ("_" + compressionToKey(options.compLevel))
                                              : "_converted";

            std::filesystem::path outPath;
            {
                std::lock_guard<std::mutex> lk(pathAllocMutex);
                outPath = omni::infra::PortableEnv::resolveSafeOutputPath(
                    task.inputPath, options.outputDir, options.targetFormat, suffixTag);
                // Touch a zero-byte reservation file if multi-worker so sibling threads see it immediately
                if (workerCount > 1) {
                    FILE* fp = nullptr;
#ifdef _WIN32
                    _wfopen_s(&fp, outPath.wstring().c_str(), L"wb");
#else
                    fp = std::fopen(outPath.c_str(), "wb");
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
                omni::engines::ConversionEngine::executeTask(task, outPath, options, progCb, cancelCb);

                std::error_code outEc;
                const std::uintmax_t outSize = std::filesystem::is_regular_file(outPath, outEc)
                                                   ? std::filesystem::file_size(outPath, outEc)
                                                   : 0;
                if (inSize > outSize && outSize > 0) {
                    totalSavedBytes.fetch_add(inSize - outSize, std::memory_order_relaxed);
                }
                successCnt.fetch_add(1, std::memory_order_relaxed);

                TaskResult res;
                res.rowId = task.rowId;
                res.ok = true;
                res.outputPath = outPath;
                res.message = "✅ 完成 (双击定位)";
                res.inputBytes = inSize;
                res.outputBytes = outSize;
                if (onTaskFinished) onTaskFinished(res);

            } catch (const std::exception& ex) {
                std::error_code rmEc;
                std::filesystem::remove(outPath, rmEc);

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
