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
    bool toolboxOk = true;
    std::string calibrePath;
    std::filesystem::path configPath;

    bool allCoreReady() const noexcept {
        return imageCoreOk && ffmpegAvailable && pdfWriteOk && epubOk && toolboxOk;
    }
};

class ConversionEngine {
public:
    static EngineHealthStatus inspectHealth();

    // Per-file 1-to-1 (or 1-to-folder for PDF->Images) execution
    static std::filesystem::path executeTask(
        const omni::core::TaskItem& item,
        const std::filesystem::path& suggestedOutPath,
        const omni::core::ConversionOptions& options,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // Aggregate N-to-1 execution (Merge images to PDF, Merge PDFs, Batch Hash Manifest)
    static std::filesystem::path executeAggregateTasks(
        const std::vector<omni::core::TaskItem>& items,
        const omni::core::ConversionOptions& options,
        const std::function<void(std::size_t taskIdx, int pct)>& itemProgressCb,
        const omni::core::CancelCheckCallback& isCancelled);
};

} // namespace omni::engines
