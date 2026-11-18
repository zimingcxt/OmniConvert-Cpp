#include "omni/core/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>

namespace omni::core {

namespace {

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

const std::unordered_set<std::string>& imageInExts() {
    static const std::unordered_set<std::string> exts = {
        ".jpg", ".jpeg", ".png", ".webp", ".avif", ".heic", ".heif",
        ".bmp", ".tiff", ".tif", ".gif", ".ico"
    };
    return exts;
}

const std::unordered_set<std::string>& audioOnlyExts() {
    static const std::unordered_set<std::string> exts = {
        ".mp3", ".wav", ".flac", ".aac", ".m4a", ".ogg", ".opus",
        ".wma", ".aiff", ".ape", ".amr"
    };
    return exts;
}

const std::unordered_set<std::string>& videoInExts() {
    static const std::unordered_set<std::string> exts = {
        ".mp4", ".mkv", ".mov", ".avi", ".webm", ".flv", ".wmv",
        ".m4v", ".ts", ".mts", ".mpg", ".mpeg", ".3gp"
    };
    return exts;
}

const std::unordered_set<std::string>& novelInExts() {
    static const std::unordered_set<std::string> exts = {
        ".txt", ".epub", ".mobi", ".azw", ".azw3", ".pdf",
        ".docx", ".fb2", ".md", ".html", ".htm"
    };
    return exts;
}

} // namespace

const std::vector<CompressionPresetInfo>& compressionPresets() {
    static const std::vector<CompressionPresetInfo> presets = {
        {CompressionLevel::Lossless, "lossless", "无损 / 原画原音（仅封装转换或最高品质）"},
        {CompressionLevel::Light, "light", "轻度压缩 · 高品质（几乎无损观感，省 25%~45% 空间）"},
        {CompressionLevel::Balanced, "balanced", "均衡压缩 · 推荐（画质与体积黄金平衡，省 50%~70% 空间）"},
        {CompressionLevel::Extreme, "extreme", "极限压缩 · 最小体积（适合网传/归档，省 70%~88% 空间）"},
    };
    return presets;
}

std::string modeToKey(WorkMode mode) {
    switch (mode) {
        case WorkMode::Image: return "image";
        case WorkMode::Audio: return "audio";
        case WorkMode::Video: return "video";
        case WorkMode::Novel: return "novel";
    }
    return "image";
}

WorkMode modeFromKey(const std::string& key) {
    const std::string k = toLowerAscii(key);
    if (k == "audio") return WorkMode::Audio;
    if (k == "video") return WorkMode::Video;
    if (k == "novel") return WorkMode::Novel;
    return WorkMode::Image;
}

std::string modeDisplayName(WorkMode mode) {
    switch (mode) {
        case WorkMode::Image: return "图片压缩与转换";
        case WorkMode::Audio: return "音频转换与提音";
        case WorkMode::Video: return "视频压缩与动图";
        case WorkMode::Novel: return "小说格式互转";
    }
    return "图片压缩与转换";
}

std::string compressionToKey(CompressionLevel level) {
    switch (level) {
        case CompressionLevel::Lossless: return "lossless";
        case CompressionLevel::Light: return "light";
        case CompressionLevel::Balanced: return "balanced";
        case CompressionLevel::Extreme: return "extreme";
    }
    return "balanced";
}

CompressionLevel compressionFromKey(const std::string& key) {
    const std::string k = toLowerAscii(key);
    if (k == "lossless") return CompressionLevel::Lossless;
    if (k == "light") return CompressionLevel::Light;
    if (k == "extreme") return CompressionLevel::Extreme;
    return CompressionLevel::Balanced;
}

std::optional<WorkMode> detectCategoryForFile(const std::filesystem::path& path) {
    const std::string ext = toLowerAscii(pathToUtf8(path.extension()));
    if (imageInExts().count(ext)) return WorkMode::Image;
    if (audioOnlyExts().count(ext)) return WorkMode::Audio;
    if (videoInExts().count(ext)) return WorkMode::Video;
    if (novelInExts().count(ext)) return WorkMode::Novel;
    return std::nullopt;
}

bool isFileAcceptedInMode(const std::filesystem::path& path, WorkMode mode) {
    const std::string ext = toLowerAscii(pathToUtf8(path.extension()));
    switch (mode) {
        case WorkMode::Image:
            return imageInExts().count(ext) > 0;
        case WorkMode::Audio:
            return audioOnlyExts().count(ext) > 0 || videoInExts().count(ext) > 0;
        case WorkMode::Video:
            return videoInExts().count(ext) > 0;
        case WorkMode::Novel:
            return novelInExts().count(ext) > 0;
    }
    return false;
}

std::string formatByteSize(std::uintmax_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    static const char* units[] = {"KB", "MB", "GB", "TB"};
    double val = static_cast<double>(bytes);
    for (int i = 0; i < 4; ++i) {
        val /= 1024.0;
        if (val < 1024.0 || i == 3) {
            char buf[64];
            if (val < 100.0) {
                std::snprintf(buf, sizeof(buf), "%.2f %s", val, units[i]);
            } else {
                std::snprintf(buf, sizeof(buf), "%.1f %s", val, units[i]);
            }
            return std::string(buf);
        }
    }
    return std::to_string(bytes) + " B";
}

} // namespace omni::core
