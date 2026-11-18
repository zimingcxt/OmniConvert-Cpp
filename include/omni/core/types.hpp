#pragma once

#include <QString>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omni::core {

#ifdef _WIN32
inline std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())).toStdWString());
}
inline std::string pathToUtf8(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring()).toUtf8().toStdString();
}
inline QString pathToQString(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}
inline std::filesystem::path qStringToPath(const QString& q) {
    return std::filesystem::path(q.toStdWString());
}
#else
inline std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(s);
}
inline std::string pathToUtf8(const std::filesystem::path& p) {
    return p.string();
}
inline QString pathToQString(const std::filesystem::path& p) {
    const auto& s = p.native();
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
inline std::filesystem::path qStringToPath(const QString& q) {
    const QByteArray u8 = q.toUtf8();
    return std::filesystem::path(std::string(u8.constData(), static_cast<std::size_t>(u8.size())));
}
#endif

enum class WorkMode {
    Image = 0,
    Audio = 1,
    Video = 2,
    Novel = 3
};

enum class CompressionLevel {
    Lossless = 0,
    Light = 1,
    Balanced = 2,
    Extreme = 3
};

struct CompressionPresetInfo {
    CompressionLevel level;
    const char* key;
    const char* description;
};

const std::vector<CompressionPresetInfo>& compressionPresets();
std::string modeToKey(WorkMode mode);
WorkMode modeFromKey(const std::string& key);
std::string modeDisplayName(WorkMode mode);

std::string compressionToKey(CompressionLevel level);
CompressionLevel compressionFromKey(const std::string& key);

std::optional<WorkMode> detectCategoryForFile(const std::filesystem::path& path);
bool isFileAcceptedInMode(const std::filesystem::path& path, WorkMode mode);
std::string formatByteSize(std::uintmax_t bytes);

struct ConversionOptions {
    std::filesystem::path outputDir; // Empty = source file directory
    std::string targetFormat;        // Lowercase e.g. "webp", "mp3", "mp4", "epub"
    CompressionLevel compLevel = CompressionLevel::Balanced;

    // Image options
    int imgMaxDim = 0;
    bool imgStripExif = false;

    // Audio options
    int audSampleRate = 0;
    bool audLoudnorm = false;

    // Video options
    int vidMaxWidth = 0;
    std::string vidCodec = "h264"; // "h264" or "h265"
    bool vidMute = false;

    // Novel options
    bool novAutoIndent = true;
    std::string novPdfPreset = "a4";      // "a4", "b5", "eink6"
    std::string novTxtEncoding = "utf-8"; // "utf-8", "gb18030"
    std::string novCustomAuthor;
};

struct TaskItem {
    int rowId = 0;
    std::filesystem::path inputPath;
    WorkMode mode = WorkMode::Image;
};

struct TaskResult {
    int rowId = 0;
    bool ok = false;
    bool cancelled = false;
    std::filesystem::path outputPath;
    std::string message;
    std::uintmax_t inputBytes = 0;
    std::uintmax_t outputBytes = 0;
};

using ProgressCallback = std::function<void(int percent)>;
using CancelCheckCallback = std::function<bool()>;

} // namespace omni::core
