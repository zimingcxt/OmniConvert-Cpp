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
        ".bmp", ".tiff", ".tif", ".gif", ".ico", ".svg", ".tga",
        ".ppm", ".pgm", ".pbm", ".xbm", ".xpm", ".icns", ".wbmp"
    };
    return exts;
}

const std::unordered_set<std::string>& audioOnlyExts() {
    static const std::unordered_set<std::string> exts = {
        ".mp3", ".wav", ".flac", ".aac", ".m4a", ".ogg", ".opus",
        ".wma", ".aiff", ".aif", ".ape", ".amr", ".ac3", ".mka"
    };
    return exts;
}

const std::unordered_set<std::string>& videoInExts() {
    static const std::unordered_set<std::string> exts = {
        ".mp4", ".mkv", ".mov", ".avi", ".webm", ".flv", ".wmv",
        ".m4v", ".ts", ".mts", ".mpg", ".mpeg", ".3gp", ".vob"
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

const std::unordered_set<std::string>& tableInExts() {
    static const std::unordered_set<std::string> exts = {
        ".csv", ".tsv", ".json"
    };
    return exts;
}

const std::unordered_set<std::string>& subtitleInExts() {
    static const std::unordered_set<std::string> exts = {
        ".srt", ".vtt", ".ass", ".ssa"
    };
    return exts;
}

} // namespace

const std::vector<CompressionPresetInfo>& compressionPresets() {
    static const std::vector<CompressionPresetInfo> presets = {
        {CompressionLevel::Lossless, "lossless", "无损 / 原画原音（仅封装转换或最高品质）"},
        {CompressionLevel::Light,    "light",    "轻度压缩 · 高品质（几乎无损观感，省 25%~45% 空间）"},
        {CompressionLevel::Balanced, "balanced", "均衡压缩 · 推荐（画质与体积黄金平衡，省 50%~70% 空间）"},
        {CompressionLevel::Extreme,  "extreme",  "极限压缩 · 最小体积（适合网传/归档，省 70%~88% 空间）"},
        {CompressionLevel::Custom,   "custom",   "🎛️ 自定义精细参数（完全手动掌控品质 / 码率 / CRF 极限）"},
    };
    return presets;
}

std::string modeToKey(WorkMode mode) {
    switch (mode) {
        case WorkMode::Image:    return "image";
        case WorkMode::Audio:    return "audio";
        case WorkMode::Video:    return "video";
        case WorkMode::Novel:    return "novel";
        case WorkMode::Document: return "document";
        case WorkMode::Toolbox:  return "toolbox";
    }
    return "image";
}

WorkMode modeFromKey(const std::string& key) {
    const std::string k = toLowerAscii(key);
    if (k == "audio")    return WorkMode::Audio;
    if (k == "video")    return WorkMode::Video;
    if (k == "novel")    return WorkMode::Novel;
    if (k == "document") return WorkMode::Document;
    if (k == "toolbox")  return WorkMode::Toolbox;
    return WorkMode::Image;
}

std::string modeDisplayName(WorkMode mode) {
    switch (mode) {
        case WorkMode::Image:    return "图片精修与压缩";
        case WorkMode::Audio:    return "音频剪辑与转换";
        case WorkMode::Video:    return "视频截取与压缩";
        case WorkMode::Novel:    return "小说封面与精排";
        case WorkMode::Document: return "PDF 与表格工坊";
        case WorkMode::Toolbox:  return "生活效率百宝箱";
    }
    return "图片精修与压缩";
}

std::string compressionToKey(CompressionLevel level) {
    switch (level) {
        case CompressionLevel::Lossless: return "lossless";
        case CompressionLevel::Light:    return "light";
        case CompressionLevel::Balanced: return "balanced";
        case CompressionLevel::Extreme:  return "extreme";
        case CompressionLevel::Custom:   return "custom";
    }
    return "balanced";
}

CompressionLevel compressionFromKey(const std::string& key) {
    const std::string k = toLowerAscii(key);
    if (k == "lossless") return CompressionLevel::Lossless;
    if (k == "light")    return CompressionLevel::Light;
    if (k == "extreme")  return CompressionLevel::Extreme;
    if (k == "custom")   return CompressionLevel::Custom;
    return CompressionLevel::Balanced;
}

std::optional<WorkMode> detectCategoryForFile(const std::filesystem::path& path) {
    const std::string ext = toLowerAscii(pathToUtf8(path.extension()));
    if (subtitleInExts().count(ext)) return WorkMode::Toolbox;
    if (tableInExts().count(ext))    return WorkMode::Document;
    if (imageInExts().count(ext))    return WorkMode::Image;
    if (audioOnlyExts().count(ext))  return WorkMode::Audio;
    if (videoInExts().count(ext))    return WorkMode::Video;
    if (novelInExts().count(ext))    return WorkMode::Novel;
    return std::nullopt;
}

bool isFileAcceptedInMode(const std::filesystem::path& path, WorkMode mode, const std::string& subAction) {
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
        case WorkMode::Document:
            if (subAction == "img_to_pdf")    return imageInExts().count(ext) > 0;
            if (subAction == "merge_pdf" || subAction == "pdf_to_img") return ext == ".pdf";
            if (subAction == "table_convert") return tableInExts().count(ext) > 0;
            return imageInExts().count(ext) > 0 || ext == ".pdf" || tableInExts().count(ext) > 0;
        case WorkMode::Toolbox:
            if (subAction == "exact_kb" || subAction == "id_watermark") return imageInExts().count(ext) > 0;
            if (subAction == "subtitle") return subtitleInExts().count(ext) > 0;
            if (subAction == "hash_manifest") return !ext.empty() || std::filesystem::is_regular_file(path);
            return true;
    }
    return false;
}

bool isAggregateBatchAction(WorkMode mode, const ConversionOptions& options) {
    if (mode == WorkMode::Document) {
        return options.docAction == "img_to_pdf" || options.docAction == "merge_pdf";
    }
    if (mode == WorkMode::Toolbox) {
        return options.toolAction == "hash_manifest";
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
