#pragma once

#include "omni/core/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace omni::engines {

class DocumentEngine {
public:
    // Merge multiple images in queue order into a single multi-page PDF
    static void mergeImagesToPdf(
        const std::vector<std::filesystem::path>& imagePaths,
        const std::filesystem::path& outPdfPath,
        const std::string& pagePreset, // "fit", "a4", "b5"
        const std::function<void(std::size_t index, int pct)>& itemProgressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // Merge multiple PDFs into a single PDF
    static void mergePdfs(
        const std::vector<std::filesystem::path>& pdfPaths,
        const std::filesystem::path& outPdfPath,
        const std::function<void(std::size_t index, int pct)>& itemProgressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // Export every page of a PDF into high-DPI images inside an output folder
    static std::filesystem::path exportPdfToImages(
        const std::filesystem::path& inPdfPath,
        const std::filesystem::path& customOutDir,
        const std::string& imgFormat, // "png", "jpg", "webp"
        int dpi,
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);

    // Smart encoding-aware CSV / TSV / JSON / Markdown / HTML table conversion (fixes Excel GBK/UTF-8 BOM issues)
    static void convertTable(
        const std::filesystem::path& inPath,
        const std::filesystem::path& outPath,
        const std::string& targetFormat, // "csv", "tsv", "json", "md", "html"
        const omni::core::ProgressCallback& progressCb,
        const omni::core::CancelCheckCallback& isCancelled);
};

#ifdef __APPLE__
namespace macos_pdf {
bool mergePdfFilesNative(
    const std::vector<std::filesystem::path>& pdfPaths,
    const std::filesystem::path& outPdfPath,
    const std::function<void(std::size_t index, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled);

bool renderPdfPagesNative(
    const std::filesystem::path& inPdfPath,
    const std::filesystem::path& outFolder,
    const std::string& stemName,
    const std::string& imgExtLower,
    int dpi,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled);
} // namespace macos_pdf
#endif

} // namespace omni::engines
