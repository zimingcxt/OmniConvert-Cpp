#pragma once

#include "omni/core/types.hpp"

#include <QByteArray>
#include <QImage>
#include <filesystem>
#include <string>
#include <vector>

namespace omni::engines {

struct Chapter {
    std::string title;
    std::vector<std::string> paragraphs;
};

struct BookDocument {
    std::string title;
    std::string author = "佚名";
    std::vector<Chapter> chapters;
    QByteArray coverJpegBytes; // Optional custom or auto-generated cover image (JPEG)
};

struct NovelStyleConfig {
    bool autoIndent = true;
    int indentSpaces = 2;
    double fontSizePt = 11.5;
    double lineHeight = 1.8;
    std::string pdfPreset = "a4";
    std::string txtEncoding = "utf-8";
};

class NovelEngine {
public:
    static BookDocument parseNovelToDocument(
        const std::filesystem::path& inPath,
        const std::string& customChapterRegex = "",
        bool cleanAds = false,
        const std::string& zhConvertMode = "none");

    static std::vector<Chapter> splitTextIntoChapters(
        const std::string& rawUtf8Text,
        bool autoSplitLong = true,
        const std::string& customChapterRegex = "",
        bool cleanAds = false);

    static std::vector<Chapter> extractChaptersFromHtml(
        const std::string& htmlUtf8,
        const std::string& fallbackTitle = "章节",
        const std::string& customChapterRegex = "",
        bool cleanAds = false);

    // Generate an elegant procedural book cover with title & author typography
    static QImage generateProceduralCover(const std::string& title, const std::string& author);
    static QByteArray prepareCoverJpeg(const QImage& rawCover);

    // Offline Simplified <-> Traditional Chinese conversion for common literary characters
    static std::string convertChineseVariant(const std::string& utf8Text, const std::string& mode);

    static void writeBookToEpub(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style = {});
    static void writeBookToTxt(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style = {});
    static void writeBookToMobi(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style = {});
    static void writeBookToMobiNative(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style = {});
    static void writeBookToPdf(
        const BookDocument& doc,
        const std::filesystem::path& outPath,
        const NovelStyleConfig& style,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);
    static void writeBookToDocx(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style = {});
    static void writeBookToHtml(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style = {});
    static void writeBookToMd(const BookDocument& doc, const std::filesystem::path& outPath);
    static void writeBookToFb2(const BookDocument& doc, const std::filesystem::path& outPath);

    static void convertNovelWithOptions(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const omni::core::ConversionOptions& opts,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    static void convertNovel(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& targetFormat,
        bool autoIndent,
        const std::string& customAuthor,
        const std::string& txtEncoding,
        const std::string& pdfPreset,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);
};

#ifdef __APPLE__
namespace macos_pdf {
bool extractPdfDocument(const std::filesystem::path& pdfPath, BookDocument& outDoc);
} // namespace macos_pdf
#endif

} // namespace omni::engines
