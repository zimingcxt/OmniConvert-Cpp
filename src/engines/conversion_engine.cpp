#include "omni/engines/conversion_engine.hpp"
#include "omni/engines/document_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/engines/media_engine.hpp"
#include "omni/engines/novel_engine.hpp"
#include "omni/engines/toolbox_engine.hpp"
#include "omni/infra/portable_env.hpp"

#include <stdexcept>

namespace omni::engines {

EngineHealthStatus ConversionEngine::inspectHealth() {
    EngineHealthStatus st;
    st.imageCoreOk = true;
    st.avifSupported = ImageEngine::supportsAvif();
    st.heicSupported = ImageEngine::supportsHeic();

    if (const auto ff = omni::infra::PortableEnv::resolveFfmpeg()) {
        st.ffmpegAvailable = true;
        st.ffmpegPath = omni::core::pathToUtf8(*ff);
    }
    if (const auto cal = omni::infra::PortableEnv::resolveCalibreConvert()) {
        st.calibrePath = omni::core::pathToUtf8(*cal);
    }

    st.pdfReadOk = true;
    st.pdfWriteOk = true;
    st.epubOk = true;
    st.docxOk = true;
    st.mobiOk = true;
    st.toolboxOk = true;
    st.configPath = omni::infra::PortableEnv::portableConfigPath();
    return st;
}

std::filesystem::path ConversionEngine::executeTask(
    const omni::core::TaskItem& item,
    const std::filesystem::path& suggestedOutPath,
    const omni::core::ConversionOptions& options,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    using omni::core::WorkMode;

    switch (item.mode) {
        case WorkMode::Image:
            ImageEngine::convertImageWithOptions(item.inputPath, suggestedOutPath, options, progressCb, isCancelled);
            return suggestedOutPath;

        case WorkMode::Audio:
            MediaEngine::convertAudioWithOptions(item.inputPath, suggestedOutPath, options, progressCb, isCancelled);
            return suggestedOutPath;

        case WorkMode::Video:
            MediaEngine::convertVideoWithOptions(item.inputPath, suggestedOutPath, options, progressCb, isCancelled);
            return suggestedOutPath;

        case WorkMode::Novel:
            NovelEngine::convertNovelWithOptions(item.inputPath, suggestedOutPath, options, progressCb, isCancelled);
            return suggestedOutPath;

        case WorkMode::Document:
            if (options.docAction == "pdf_to_img") {
                return DocumentEngine::exportPdfToImages(
                    item.inputPath,
                    options.outputDir,
                    options.targetFormat,
                    options.docPdfDpi,
                    progressCb,
                    isCancelled);
            }
            if (options.docAction == "table_convert") {
                DocumentEngine::convertTable(
                    item.inputPath,
                    suggestedOutPath,
                    options.targetFormat,
                    progressCb,
                    isCancelled);
                return suggestedOutPath;
            }
            if (options.docAction == "img_to_pdf") {
                DocumentEngine::mergeImagesToPdf(
                    {item.inputPath},
                    suggestedOutPath,
                    options.docPagePreset,
                    [&](std::size_t, int pct) {
                        if (progressCb) progressCb(pct);
                    },
                    isCancelled);
                return suggestedOutPath;
            }
            throw std::runtime_error("不支持的单文件文档处理模式: " + options.docAction);

        case WorkMode::Toolbox:
            if (options.toolAction == "exact_kb") {
                ToolboxEngine::compressImageToExactKb(
                    item.inputPath,
                    suggestedOutPath,
                    options.toolTargetKb,
                    options.targetFormat,
                    progressCb,
                    isCancelled);
                return suggestedOutPath;
            }
            if (options.toolAction == "id_watermark") {
                ToolboxEngine::applyIdSecurityWatermark(
                    item.inputPath,
                    suggestedOutPath,
                    options.toolWatermarkText,
                    options.toolWatermarkOpacity,
                    options.targetFormat,
                    progressCb,
                    isCancelled);
                return suggestedOutPath;
            }
            if (options.toolAction == "subtitle") {
                ToolboxEngine::convertAndShiftSubtitle(
                    item.inputPath,
                    suggestedOutPath,
                    options.targetFormat,
                    options.toolSubtitleShiftMs,
                    progressCb,
                    isCancelled);
                return suggestedOutPath;
            }
            throw std::runtime_error("不支持的单文件百宝箱动作: " + options.toolAction);
    }

    throw std::runtime_error("未知的工作模块");
}

std::filesystem::path ConversionEngine::executeAggregateTasks(
    const std::vector<omni::core::TaskItem>& items,
    const omni::core::ConversionOptions& options,
    const std::function<void(std::size_t taskIdx, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (items.empty()) {
        throw std::runtime_error("任务列表为空");
    }

    std::vector<std::filesystem::path> inputs;
    inputs.reserve(items.size());
    for (const auto& t : items) {
        inputs.push_back(t.inputPath);
    }

    const std::filesystem::path baseDir = options.outputDir.empty()
                                              ? inputs.front().parent_path()
                                              : options.outputDir;

    std::string baseStem = options.docMergedName;
    if (baseStem.empty()) {
        baseStem = (items.front().mode == omni::core::WorkMode::Toolbox) ? "哈希校验清单" : "合并文档";
    }
    const std::filesystem::path fakeSeed = baseDir / omni::core::pathFromUtf8(baseStem + ".tmp");

    if (items.front().mode == omni::core::WorkMode::Document) {
        const std::filesystem::path outPdf = omni::infra::PortableEnv::resolveSafeOutputPath(
            fakeSeed, baseDir, "pdf", "");

        if (options.docAction == "img_to_pdf") {
            DocumentEngine::mergeImagesToPdf(inputs, outPdf, options.docPagePreset, itemProgressCb, isCancelled);
            return outPdf;
        }
        if (options.docAction == "merge_pdf") {
            DocumentEngine::mergePdfs(inputs, outPdf, itemProgressCb, isCancelled);
            return outPdf;
        }
    } else if (items.front().mode == omni::core::WorkMode::Toolbox && options.toolAction == "hash_manifest") {
        const std::string fmt = options.targetFormat.empty() ? "csv" : options.targetFormat;
        const std::filesystem::path outManifest = omni::infra::PortableEnv::resolveSafeOutputPath(
            baseDir / omni::core::pathFromUtf8("SHA256_校验清单.tmp"), baseDir, fmt, "");
        ToolboxEngine::generateHashManifest(inputs, outManifest, fmt, itemProgressCb, isCancelled);
        return outManifest;
    }

    throw std::runtime_error("不支持的批量汇总动作");
}

} // namespace omni::engines
