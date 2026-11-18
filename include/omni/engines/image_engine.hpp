#pragma once

#include "omni/core/types.hpp"

#include <QImage>
#include <filesystem>
#include <string>

namespace omni::engines {

class ImageEngine {
public:
    static bool supportsAvif();
    static bool supportsHeic();

    // Shared multi-backend image decoder (Qt6 plugins + macOS ImageIO + FFmpeg fallback, auto EXIF-rotated)
    static QImage loadAnyImage(const std::filesystem::path& inPath);

    static void convertImageWithOptions(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const omni::core::ConversionOptions& opts,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    static void convertImage(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& targetFormat,
        omni::core::CompressionLevel compLevel,
        int maxDim,
        bool stripExif,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    static QImage quantizeColors(const QImage& src, int maxColors, bool preserveAlpha);
    static void writeMultiSizeIco(const QImage& src, const std::filesystem::path& outPath);
};

#ifdef __APPLE__
namespace macos_imageio {
bool supportsUtiRead(const char* fmtLower);
bool supportsUtiWrite(const char* fmtLower);
bool readImageViaImageIO(const std::filesystem::path& inPath, QImage& outImg, std::string* errMsg = nullptr);
bool writeImageViaImageIO(
    const QImage& img,
    const std::filesystem::path& outPath,
    const std::string& fmtUpper,
    int quality0to100,
    bool lossless,
    std::string* errMsg = nullptr);
} // namespace macos_imageio
#endif

} // namespace omni::engines
