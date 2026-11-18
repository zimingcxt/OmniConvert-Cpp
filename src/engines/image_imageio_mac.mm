#include "omni/engines/image_engine.hpp"

#ifdef __APPLE__
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <QFile>
#include <QPainter>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace omni::engines::macos_imageio {

namespace {

CFStringRef utiForFormat(const std::string& fmtLower) {
    if (fmtLower == "heic" || fmtLower == "heif") return CFSTR("public.heic");
    if (fmtLower == "avif") return CFSTR("public.avif");
    if (fmtLower == "webp") return CFSTR("org.webmproject.webp");
    if (fmtLower == "png") return CFSTR("public.png");
    if (fmtLower == "jpg" || fmtLower == "jpeg") return CFSTR("public.jpeg");
    if (fmtLower == "tiff" || fmtLower == "tif") return CFSTR("public.tiff");
    if (fmtLower == "gif") return CFSTR("com.compuserve.gif");
    if (fmtLower == "bmp") return CFSTR("com.microsoft.bmp");
    return nullptr;
}

bool cfArrayContainsStr(CFArrayRef arr, CFStringRef target) {
    if (!arr || !target) return false;
    const CFIndex count = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < count; ++i) {
        const auto val = static_cast<CFStringRef>(CFArrayGetValueAtIndex(arr, i));
        if (val && CFStringCompare(val, target, kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            return true;
        }
    }
    return false;
}

CGImageRef createNativeCGImage(const QImage& prepared, bool withAlpha) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, prepared.constBits(), static_cast<std::size_t>(prepared.sizeInBytes()), nullptr);
    const CGBitmapInfo bmpInfo =
        static_cast<CGBitmapInfo>(withAlpha ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst) |
        kCGBitmapByteOrder32Host;

    CGImageRef cgImg = CGImageCreate(
        static_cast<std::size_t>(prepared.width()),
        static_cast<std::size_t>(prepared.height()),
        8,
        32,
        static_cast<std::size_t>(prepared.bytesPerLine()),
        cs,
        bmpInfo,
        provider,
        nullptr,
        false,
        kCGRenderingIntentDefault);

    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    return cgImg;
}

bool encodeCGImageToData(
    CFStringRef uti,
    const QImage& prepared,
    bool withAlpha,
    int quality0to100,
    bool lossless,
    QByteArray& outBytes) {

    CGImageRef cgImg = createNativeCGImage(prepared, withAlpha);
    if (!cgImg) return false;

    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    CGImageDestinationRef dest = data ? CGImageDestinationCreateWithData(data, uti, 1, nullptr) : nullptr;
    if (!dest) {
        if (data) CFRelease(data);
        CGImageRelease(cgImg);
        return false;
    }

    const double qNorm = lossless ? 1.0 : std::clamp(quality0to100 / 100.0, 0.05, 1.0);
    CFNumberRef qNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &qNorm);
    const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
    const void* vals[] = {qNum};
    CFDictionaryRef props = CFDictionaryCreate(
        kCFAllocatorDefault, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CGImageDestinationAddImage(dest, cgImg, props);
    const bool ok = CGImageDestinationFinalize(dest) && CFDataGetLength(data) > 0;
    if (ok) {
        outBytes = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                              static_cast<qsizetype>(CFDataGetLength(data)));
    }

    CFRelease(props);
    CFRelease(qNum);
    CFRelease(dest);
    CFRelease(data);
    CGImageRelease(cgImg);
    return ok;
}

} // namespace

bool supportsUtiRead(const char* fmtLower) {
    CFStringRef uti = utiForFormat(fmtLower);
    if (!uti) return false;
    CFArrayRef types = CGImageSourceCopyTypeIdentifiers();
    const bool ok = cfArrayContainsStr(types, uti);
    if (types) CFRelease(types);
    return ok;
}

bool supportsUtiWrite(const char* fmtLower) {
    static std::mutex cacheMu;
    static std::unordered_map<std::string, bool> probeCache;

    const std::string key(fmtLower ? fmtLower : "");
    {
        std::lock_guard<std::mutex> lk(cacheMu);
        auto it = probeCache.find(key);
        if (it != probeCache.end()) return it->second;
    }

    CFStringRef uti = utiForFormat(key);
    bool supported = false;
    if (uti) {
        CFArrayRef types = CGImageDestinationCopyTypeIdentifiers();
        const bool listed = cfArrayContainsStr(types, uti);
        if (types) CFRelease(types);

        if (listed) {
            // Use >= 256px dimensions so ImageIO exercises the real hardware VideoToolbox encoder path
            QImage probeImg(256, 192, QImage::Format_RGB32);
            probeImg.fill(QColor(64, 128, 220));
            QByteArray encoded;
            supported = encodeCGImageToData(uti, probeImg, false, 80, false, encoded);
        }
    }

    {
        std::lock_guard<std::mutex> lk(cacheMu);
        probeCache[key] = supported;
    }
    return supported;
}

bool readImageViaImageIO(const std::filesystem::path& inPath, QImage& outImg, std::string* errMsg) {
    QFile f(omni::core::pathToQString(inPath));
    if (!f.open(QIODevice::ReadOnly)) {
        if (errMsg) *errMsg = "无法读取图像文件";
        return false;
    }
    const QByteArray rawBytes = f.readAll();
    if (rawBytes.isEmpty()) return false;

    CFDataRef cfData = CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(rawBytes.constData()),
        static_cast<CFIndex>(rawBytes.size()),
        kCFAllocatorNull);
    if (!cfData) return false;

    CGImageSourceRef src = CGImageSourceCreateWithData(cfData, nullptr);
    if (!src) {
        CFRelease(cfData);
        if (errMsg) *errMsg = "ImageIO 无法解析图像数据";
        return false;
    }

    int maxSide = 16384;
    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr);
    if (props) {
        int w = 0, h = 0;
        if (auto nw = static_cast<CFNumberRef>(CFDictionaryGetValue(props, kCGImagePropertyPixelWidth))) {
            CFNumberGetValue(nw, kCFNumberIntType, &w);
        }
        if (auto nh = static_cast<CFNumberRef>(CFDictionaryGetValue(props, kCGImagePropertyPixelHeight))) {
            CFNumberGetValue(nh, kCFNumberIntType, &h);
        }
        if (std::max(w, h) > 0) maxSide = std::max(w, h);
        CFRelease(props);
    }

    CFNumberRef maxNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxSide);
    const void* keys[] = {
        kCGImageSourceCreateThumbnailFromImageAlways,
        kCGImageSourceCreateThumbnailWithTransform,
        kCGImageSourceThumbnailMaxPixelSize
    };
    const void* vals[] = {kCFBooleanTrue, kCFBooleanTrue, maxNum};
    CFDictionaryRef opts = CFDictionaryCreate(
        kCFAllocatorDefault, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CGImageRef cgImg = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);
    if (!cgImg) {
        cgImg = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    }
    CFRelease(opts);
    CFRelease(maxNum);
    CFRelease(src);
    CFRelease(cfData);

    if (!cgImg) {
        if (errMsg) *errMsg = "ImageIO 解码图像帧失败";
        return false;
    }

    const std::size_t width = CGImageGetWidth(cgImg);
    const std::size_t height = CGImageGetHeight(cgImg);
    if (width == 0 || height == 0) {
        CGImageRelease(cgImg);
        return false;
    }

    QImage result(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        result.bits(),
        width,
        height,
        8,
        static_cast<std::size_t>(result.bytesPerLine()),
        cs,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Host);

    if (!ctx) {
        CGColorSpaceRelease(cs);
        CGImageRelease(cgImg);
        return false;
    }

    CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), cgImg);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(cgImg);

    outImg = result.convertToFormat(QImage::Format_RGBA8888);
    return !outImg.isNull();
}

bool writeImageViaImageIO(
    const QImage& img,
    const std::filesystem::path& outPath,
    const std::string& fmtUpper,
    int quality0to100,
    bool lossless,
    std::string* errMsg) {

    std::string fmtLower = fmtUpper;
    std::transform(fmtLower.begin(), fmtLower.end(), fmtLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    CFStringRef uti = utiForFormat(fmtLower);
    if (!uti) {
        if (errMsg) *errMsg = "不支持的 ImageIO 格式: " + fmtUpper;
        return false;
    }

    QByteArray encoded;
    bool ok = false;

    if (img.hasAlphaChannel()) {
        QImage premul = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        ok = encodeCGImageToData(uti, premul, true, quality0to100, lossless, encoded);
        if (!ok) {
            QImage opaque(img.size(), QImage::Format_RGB32);
            opaque.fill(Qt::white);
            {
                QPainter p(&opaque);
                p.drawImage(0, 0, img);
            }
            ok = encodeCGImageToData(uti, opaque, false, quality0to100, lossless, encoded);
        }
    } else {
        QImage opaque = img.convertToFormat(QImage::Format_RGB32);
        ok = encodeCGImageToData(uti, opaque, false, quality0to100, lossless, encoded);
    }

    if (!ok || encoded.isEmpty()) {
        if (errMsg) *errMsg = "ImageIO 编码输出失败: " + fmtUpper;
        return false;
    }

    QFile outFile(omni::core::pathToQString(outPath));
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errMsg) *errMsg = "无法写入目标文件: " + omni::core::pathToUtf8(outPath);
        return false;
    }
    const qint64 written = outFile.write(encoded);
    outFile.close();
    return written == encoded.size();
}

} // namespace omni::engines::macos_imageio
#endif
