#pragma once

#include "omni/core/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace omni::engines {

class ToolboxEngine {
public:
    // 1. Compress ID photo / exam registration photo / attachment to strictly <= targetKb KB
    //    using binary-search quality + adaptive resolution stepping.
    static void compressImageToExactKb(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        int targetKb,
        const std::string& targetFormat, // "jpg" or "webp"
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // 2. Apply full-screen diagonal anti-theft security watermark for ID cards, licenses, and contracts.
    static void applyIdSecurityWatermark(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& watermarkText,
        int opacityPercent,
        const std::string& targetFormat,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // 3. Convert subtitles (.srt / .vtt / .ass / .ssa -> .srt / .vtt / .ass / .txt)
    //    with millisecond timeline shift (+/- ms) and automatic GB18030/Big5/UTF-8 repair.
    static void convertAndShiftSubtitle(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& targetFormat, // "srt", "vtt", "ass", "txt"
        int shiftMilliseconds,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // 4. Streaming SHA-256 + MD5 + file size verification manifest generator (handles multi-GB files with tiny RAM).
    static void generateHashManifest(
        const std::vector<std::filesystem::path>& inPaths,
        const std::filesystem::path& outPath,
        const std::string& targetFormat, // "csv", "txt", "json"
        const std::function<void(std::size_t index, int pct)>& itemProgressCb,
        const omni::core::CancelCheckCallback& isCancelled);
};

} // namespace omni::engines
