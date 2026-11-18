#pragma once

#include "omni/core/types.hpp"

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
};

class NovelEngine {
public:
    static BookDocument parseNovelToDocument(const std::filesystem::path& inPath);

    static std::vector<Chapter> splitTextIntoChapters(const std::string& rawUtf8Text, bool autoSplitLong = true);
    static std::vector<Chapter> extractChaptersFromHtml(const std::string& htmlUtf8, const std::string& fallbackTitle = "章节");

    static void writeBookToEpub(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent = true);
    static void writeBookToTxt(const BookDocument& doc, const std::filesystem::path& outPath, const std::string& txtEncoding = "utf-8", bool autoIndent = true);
    static void writeBookToMobi(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent = true);
    static void writeBookToMobiNative(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent = true);
    static void writeBookToPdf(
        const BookDocument& doc,
        const std::filesystem::path& outPath,
        const std::string& pagePreset,
        bool autoIndent,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);
    static void writeBookToDocx(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent = true);
    static void writeBookToHtml(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent = true);
    static void writeBookToMd(const BookDocument& doc, const std::filesystem::path& outPath);
    static void writeBookToFb2(const BookDocument& doc, const std::filesystem::path& outPath);

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
