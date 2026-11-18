#include "omni/engines/conversion_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/engines/media_engine.hpp"
#include "omni/engines/novel_engine.hpp"
#include "omni/infra/portable_env.hpp"

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
    st.configPath = omni::infra::PortableEnv::portableConfigPath();
    return st;
}

void ConversionEngine::executeTask(
    const omni::core::TaskItem& item,
    const std::filesystem::path& outPath,
    const omni::core::ConversionOptions& options,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    switch (item.mode) {
        case omni::core::WorkMode::Image:
            ImageEngine::convertImage(
                item.inputPath,
                outPath,
                options.targetFormat,
                options.compLevel,
                options.imgMaxDim,
                options.imgStripExif,
                progressCb,
                isCancelled);
            break;

        case omni::core::WorkMode::Audio:
            MediaEngine::convertAudio(
                item.inputPath,
                outPath,
                options.targetFormat,
                options.compLevel,
                options.audSampleRate,
                options.audLoudnorm,
                progressCb,
                isCancelled);
            break;

        case omni::core::WorkMode::Video:
            MediaEngine::convertVideo(
                item.inputPath,
                outPath,
                options.targetFormat,
                options.compLevel,
                options.vidMaxWidth,
                options.vidCodec,
                options.vidMute,
                progressCb,
                isCancelled);
            break;

        case omni::core::WorkMode::Novel:
            NovelEngine::convertNovel(
                item.inputPath,
                outPath,
                options.targetFormat,
                options.novAutoIndent,
                options.novCustomAuthor,
                options.novTxtEncoding,
                options.novPdfPreset,
                progressCb,
                isCancelled);
            break;
    }
}

} // namespace omni::engines
