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
    Novel = 3,
    Document = 4,
    Toolbox = 5
};

enum class CompressionLevel {
    Lossless = 0,
    Light = 1,
    Balanced = 2,
    Extreme = 3,
    Custom = 4
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
bool isFileAcceptedInMode(const std::filesystem::path& path, WorkMode mode, const std::string& subAction = "");
std::string formatByteSize(std::uintmax_t bytes);

struct ConversionOptions {
    std::filesystem::path outputDir; // Empty = source file directory
    std::string targetFormat;        // Lowercase e.g. "webp", "mp3", "mp4", "epub"
    CompressionLevel compLevel = CompressionLevel::Balanced;

    // 1. Image fine-grained options
    int imgCustomQuality = 80;       // 1..100 (used when compLevel == Custom)
    int imgPngColors = 256;          // 2..256 (0 = truecolor lossless, 2..256 = palette quantize)
    int imgMaxDim = 0;               // 0 = keep, or 16..16384 px
    int imgScalePercent = 100;       // 10..400% (applied when imgMaxDim == 0 and != 100)
    int imgRotateDeg = 0;            // 0, 90, 180, 270
    bool imgFlipHorizontal = false;
    bool imgGrayscale = false;
    bool imgStripExif = false;
    std::string imgBgColor = "#FFFFFF"; // Alpha flatten background color
    std::string imgWatermarkText;       // Optional corner watermark text

    // 2. Audio fine-grained options
    int audCustomBitrateKbps = 192;  // 16..512 kbps (used when compLevel == Custom)
    int audSampleRate = 0;           // 0 = keep, 8000, 16000, 22050, 44100, 48000, 96000
    int audChannels = 0;             // 0 = keep, 1 = mono, 2 = stereo
    double audSpeed = 1.0;           // 0.5..2.0x tempo
    bool audLoudnorm = false;
    std::string mediaTrimStart;      // Optional HH:MM:SS or MM:SS
    std::string mediaTrimEnd;        // Optional HH:MM:SS or MM:SS

    // 3. Video fine-grained options
    int vidCustomCrf = 24;           // 0..51 (used when compLevel == Custom)
    int vidMaxWidth = 0;             // 0 = keep, or 120..7680 px
    int vidFps = 0;                  // 0 = auto/keep, or 5..120 fps
    std::string vidCodec = "h264";   // "h264" or "h265"
    bool vidMute = false;

    // 4. Novel / E-Book rich customization options
    bool novAutoIndent = true;
    int novIndentSpaces = 2;              // 0, 2, 4
    double novFontSizePt = 11.5;          // 9.0 .. 22.0 pt
    double novLineHeight = 1.8;           // 1.2 .. 2.5
    std::string novPdfPreset = "a4";      // "a4", "b5", "eink6"
    std::string novTxtEncoding = "utf-8"; // "utf-8", "gb18030"
    std::string novCustomTitle;           // Optional book title override
    std::string novCustomAuthor;          // Optional author override
    std::filesystem::path novCoverPath;   // Optional custom cover image (JPG/PNG/WEBP)
    bool novAutoGenerateCover = false;    // Generate procedural cover when novCoverPath is empty
    bool novCleanAds = true;              // Clean web-novel URL/ad junk lines
    std::string novZhConvert = "none";    // "none", "t2s" (Traditional->Simplified), "s2t" (Simplified->Traditional)
    std::string novCustomChapterRegex;    // Optional user regex for chapter splitting

    // 5. Document & Data Table Workshop options
    // docAction: "img_to_pdf", "merge_pdf", "pdf_to_img", "table_convert"
    std::string docAction = "img_to_pdf";
    std::string docPagePreset = "fit";    // "fit" (match image), "a4", "b5"
    int docPdfDpi = 150;                  // 72..300 DPI for pdf_to_img
    std::string docMergedName = "合并文档";

    // 6. Everyday Life Toolbox options
    // toolAction: "exact_kb", "id_watermark", "subtitle", "hash_manifest"
    std::string toolAction = "exact_kb";
    int toolTargetKb = 100;               // e.g. 20..10240 KB exact photo size cap
    std::string toolWatermarkText = "仅供本次业务办理使用 他用无效";
    int toolWatermarkOpacity = 28;        // 8..85%
    int toolSubtitleShiftMs = 0;          // -600000 .. +600000 ms
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

bool isAggregateBatchAction(WorkMode mode, const ConversionOptions& options);

} // namespace omni::core
