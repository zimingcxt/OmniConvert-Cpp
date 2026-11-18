#pragma once

#include "omni/core/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace omni::engines {

struct EngineHealthStatus {
    bool imageCoreOk = true;
    bool avifSupported = false;
    bool heicSupported = false;
    bool ffmpegAvailable = false;
    std::string ffmpegPath;
    bool pdfReadOk = true;
    bool pdfWriteOk = true;
    bool epubOk = true;
    bool docxOk = true;
    bool mobiOk = true;
    std::string calibrePath;
    std::filesystem::path configPath;

    bool allCoreReady() const noexcept {
        return imageCoreOk && ffmpegAvailable && pdfWriteOk && epubOk;
    }
};

class ConversionEngine {
public:
    static EngineHealthStatus inspectHealth();

    static void executeTask(
        const omni::core::TaskItem& item,
        const std::filesystem::path& outPath,
        const omni::core::ConversionOptions& options,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);
};

} // namespace omni::engines
