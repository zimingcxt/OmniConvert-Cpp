#include "omni/engines/document_engine.hpp"
#include "omni/engines/novel_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"

#ifdef __APPLE__
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <PDFKit/PDFKit.h>

#include <QImage>
#include <QImageWriter>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace omni::engines::macos_pdf {

namespace {

QString normalizeLineQ(const QString& s) {
    int i = 0;
    while (i < s.size()) {
        const QChar c = s[i];
        if (c == u' ' || c == u'\t' || c == QChar(0x3000) || c == QChar(0x00A0)) {
            ++i;
        } else {
            break;
        }
    }
    return s.mid(i).trimmed();
}

} // namespace

bool extractPdfDocument(const std::filesystem::path& pdfPath, BookDocument& outDoc) {
    @autoreleasepool {
        NSString* nsPath = [NSString stringWithUTF8String:pdfPath.string().c_str()];
        if (!nsPath) return false;
        NSURL* url = [NSURL fileURLWithPath:nsPath];
        PDFDocument* pdf = [[PDFDocument alloc] initWithURL:url];
        if (!pdf || pdf.pageCount == 0) {
            return false;
        }

        NSDictionary* attrs = pdf.documentAttributes;
        if (attrs) {
            NSString* t = attrs[PDFDocumentTitleAttribute];
            if (t && t.length > 0) {
                const QString qt = QString::fromUtf8([t UTF8String]).trimmed();
                if (!qt.isEmpty()) outDoc.title = qt.toUtf8().toStdString();
            }
            NSString* a = attrs[PDFDocumentAuthorAttribute];
            if (a && a.length > 0) {
                const QString qa = QString::fromUtf8([a UTF8String]).trimmed();
                if (!qa.isEmpty()) outDoc.author = qa.toUtf8().toStdString();
            }
        }

        const NSUInteger pageCount = pdf.pageCount;

        // Check top-level PDF outline bookmarks
        std::vector<std::pair<std::string, NSUInteger>> topToc;
        PDFOutline* rootOutline = pdf.outlineRoot;
        if (rootOutline && rootOutline.numberOfChildren >= 2) {
            for (NSUInteger i = 0; i < rootOutline.numberOfChildren; ++i) {
                PDFOutline* child = [rootOutline childAtIndex:i];
                if (!child || !child.label) continue;
                const QString label = QString::fromUtf8([child.label UTF8String]).trimmed();
                if (label.isEmpty()) continue;

                PDFDestination* dest = child.destination;
                if (!dest && child.action && [child.action isKindOfClass:[PDFActionGoTo class]]) {
                    dest = ((PDFActionGoTo*)child.action).destination;
                }
                if (dest && dest.page) {
                    const NSUInteger pageIdx = [pdf indexForPage:dest.page];
                    if (pageIdx != NSNotFound && pageIdx < pageCount) {
                        topToc.emplace_back(label.toUtf8().toStdString(), pageIdx);
                    }
                }
            }
        }

        if (topToc.size() >= 2 && pageCount > 1) {
            std::vector<Chapter> chapters;
            if (topToc[0].second > 0) {
                Chapter intro;
                intro.title = "前言";
                for (NSUInteger p = 0; p < topToc[0].second; ++p) {
                    PDFPage* pg = [pdf pageAtIndex:p];
                    if (!pg || !pg.string) continue;
                    const QString txt = QString::fromUtf8([pg.string UTF8String]);
                    for (const QString& line : txt.split('\n')) {
                        const QString norm = normalizeLineQ(line);
                        if (!norm.isEmpty()) intro.paragraphs.push_back(norm.toUtf8().toStdString());
                    }
                }
                if (!intro.paragraphs.empty()) chapters.push_back(std::move(intro));
            }

            for (std::size_t idx = 0; idx < topToc.size(); ++idx) {
                const std::string& chTitle = topToc[idx].first;
                const QString qChTitle = QString::fromUtf8(chTitle.c_str());
                const NSUInteger startP = topToc[idx].second;
                NSUInteger endP = (idx + 1 < topToc.size()) ? topToc[idx + 1].second : pageCount;
                endP = std::max(startP + 1, std::min(pageCount, endP));

                Chapter ch;
                ch.title = chTitle;
                for (NSUInteger p = startP; p < endP; ++p) {
                    PDFPage* pg = [pdf pageAtIndex:p];
                    if (!pg || !pg.string) continue;
                    const QString txt = QString::fromUtf8([pg.string UTF8String]);
                    for (const QString& line : txt.split('\n')) {
                        const QString norm = normalizeLineQ(line);
                        if (!norm.isEmpty() && norm != qChTitle) {
                            ch.paragraphs.push_back(norm.toUtf8().toStdString());
                        }
                    }
                }
                if (!ch.paragraphs.empty()) {
                    chapters.push_back(std::move(ch));
                }
            }

            if (!chapters.empty()) {
                outDoc.chapters = std::move(chapters);
                return true;
            }
        }

        // Full-text extraction across all pages
        QStringList allPages;
        allPages.reserve(static_cast<qsizetype>(pageCount));
        for (NSUInteger p = 0; p < pageCount; ++p) {
            PDFPage* pg = [pdf pageAtIndex:p];
            if (pg && pg.string) {
                allPages << QString::fromUtf8([pg.string UTF8String]);
            }
        }
        const std::string fullUtf8 = allPages.join('\n').toUtf8().toStdString();
        outDoc.chapters = NovelEngine::splitTextIntoChapters(fullUtf8, true);
        return !outDoc.chapters.empty();
    }
}

bool mergePdfFilesNative(
    const std::vector<std::filesystem::path>& pdfPaths,
    const std::filesystem::path& outPdfPath,
    const std::function<void(std::size_t index, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled) {
    @autoreleasepool {
        if (pdfPaths.empty()) return false;
        PDFDocument* merged = [[PDFDocument alloc] init];
        if (!merged) return false;

        for (std::size_t i = 0; i < pdfPaths.size(); ++i) {
            if (isCancelled && isCancelled()) {
                throw std::runtime_error("CANCELLED");
            }
            NSString* nsIn = [NSString stringWithUTF8String:pdfPaths[i].string().c_str()];
            if (!nsIn) return false;
            PDFDocument* src = [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:nsIn]];
            if (!src || src.pageCount == 0) {
                return false;
            }
            const NSUInteger cnt = src.pageCount;
            for (NSUInteger p = 0; p < cnt; ++p) {
                PDFPage* page = [src pageAtIndex:p];
                if (page) {
                    [merged insertPage:page atIndex:merged.pageCount];
                }
            }
            if (itemProgressCb) {
                itemProgressCb(i, 92);
            }
        }

        if (merged.pageCount == 0) return false;

        omni::infra::AtomicFileTx tx(outPdfPath);
        NSString* nsTemp = [NSString stringWithUTF8String:tx.tempPath().string().c_str()];
        if (!nsTemp || ![merged writeToURL:[NSURL fileURLWithPath:nsTemp]]) {
            tx.rollback();
            return false;
        }
        tx.commit();
        for (std::size_t i = 0; i < pdfPaths.size(); ++i) {
            if (itemProgressCb) itemProgressCb(i, 100);
        }
        return true;
    }
}

bool renderPdfPagesNative(
    const std::filesystem::path& inPdfPath,
    const std::filesystem::path& outFolder,
    const std::string& stemName,
    const std::string& imgExtLower,
    int dpi,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {
    @autoreleasepool {
        NSString* nsIn = [NSString stringWithUTF8String:inPdfPath.string().c_str()];
        if (!nsIn) return false;
        PDFDocument* pdf = [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:nsIn]];
        if (!pdf || pdf.pageCount == 0) {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(outFolder, ec);

        const double clampedDpi = std::clamp(dpi, 72, 600);
        const double scale = clampedDpi / 72.0;
        const NSUInteger totalPages = pdf.pageCount;

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        if (!colorSpace) return false;

        for (NSUInteger p = 0; p < totalPages; ++p) {
            if (isCancelled && isCancelled()) {
                CGColorSpaceRelease(colorSpace);
                throw std::runtime_error("CANCELLED");
            }
            PDFPage* page = [pdf pageAtIndex:p];
            if (!page) continue;

            const CGRect box = [page boundsForBox:kPDFDisplayBoxMediaBox];
            const int w = std::max(16, static_cast<int>(std::lround(box.size.width * scale)));
            const int h = std::max(16, static_cast<int>(std::lround(box.size.height * scale)));

            QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
            img.fill(Qt::white);

            CGContextRef ctx = CGBitmapContextCreate(
                img.bits(),
                static_cast<size_t>(w),
                static_cast<size_t>(h),
                8,
                static_cast<size_t>(img.bytesPerLine()),
                colorSpace,
                static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big));
            if (!ctx) {
                CGColorSpaceRelease(colorSpace);
                return false;
            }

            CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
            CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
            CGContextSaveGState(ctx);
            CGContextScaleCTM(ctx, scale, scale);
            [page drawWithBox:kPDFDisplayBoxMediaBox toContext:ctx];
            CGContextRestoreGState(ctx);
            CGContextRelease(ctx);

            char pageName[64];
            std::snprintf(pageName, sizeof(pageName), "_page_%04lu.%s",
                          static_cast<unsigned long>(p + 1), imgExtLower.c_str());
            const std::filesystem::path pagePath = outFolder / omni::core::pathFromUtf8(stemName + pageName);

            QImage outImg = (imgExtLower == "jpg" || imgExtLower == "jpeg")
                                ? img.convertToFormat(QImage::Format_RGB888)
                                : img;
            const QByteArray fmtBytes = (imgExtLower == "jpg") ? QByteArray("jpeg") : QByteArray::fromStdString(imgExtLower);
            QImageWriter writer(omni::core::pathToQString(pagePath), fmtBytes);
            if (imgExtLower == "jpg" || imgExtLower == "jpeg" || imgExtLower == "webp") {
                writer.setQuality(90);
            }
            if (!writer.write(outImg)) {
                // Fallback to PNG if WebP writer plugin is absent
                const std::filesystem::path pngFallback = outFolder / omni::core::pathFromUtf8(
                    stemName + "_page_" + std::to_string(p + 1) + ".png");
                outImg.save(omni::core::pathToQString(pngFallback), "PNG");
            }

            if (progressCb) {
                progressCb(std::min(99, static_cast<int>((p + 1) * 98 / totalPages)));
            }
        }

        CGColorSpaceRelease(colorSpace);
        if (progressCb) progressCb(100);
        return true;
    }
}

} // namespace omni::engines::macos_pdf
#endif
