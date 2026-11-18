#pragma once

#include "omni/core/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace omni::engines {

class MediaEngine {
public:
    static void runFfmpegCommand(
        const std::vector<std::string>& args,
        const std::filesystem::path& outPath,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    static void convertAudio(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& targetFormat,
        omni::core::CompressionLevel compLevel,
        int sampleRate,
        bool loudnorm,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    static void convertVideo(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& targetFormat,
        omni::core::CompressionLevel compLevel,
        int maxWidth,
        const std::string& codecPref,
        bool muteAudio,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);
};

} // namespace omni::engines
