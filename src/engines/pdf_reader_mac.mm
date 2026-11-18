#include "omni/engines/novel_engine.hpp"

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <PDFKit/PDFKit.h>

#include <QRegularExpression>
#include <QString>
#include <algorithm>
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

} // namespace omni::engines::macos_pdf
#endif
