#include "omni/engines/novel_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/portable_env.hpp"
#include "omni/infra/text_encoding.hpp"
#include "omni/infra/zip_archive.hpp"

#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

#ifdef OMNI_HAVE_QTPDF
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#endif

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace omni::engines {

namespace {

std::mutex gPdfRenderMutex;

const QRegularExpression& chapterTitleRegex() {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^\s*(?:正文\s*)?(?:第[零一二三四五六七八九十百千万两0-9]+[章节回卷集部篇幕]|卷[零一二三四五六七八九十百千万两0-9]+|楔子|序章|序言|引子|前言|后记|尾声|番外|终章|Chapter\s+\d+|CHAPTER\s+\d+|Part\s+\d+|Book\s+\d+)(?:[\s：:、.\-—_].*)?$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

bool isChapterHeadingLine(const QString& stripped) {
    return stripped.size() <= 68 && !stripped.contains('\n') && chapterTitleRegex().match(stripped).hasMatch();
}

QString normalizeParagraphQ(const QString& line) {
    int i = 0;
    while (i < line.size()) {
        const QChar c = line[i];
        if (c == u' ' || c == u'\t' || c == QChar(0x3000) || c == QChar(0x00A0)) {
            ++i;
        } else {
            break;
        }
    }
    int end = line.size();
    while (end > i && line[end - 1].isSpace()) {
        --end;
    }
    return line.mid(i, end - i);
}

std::string cleanBookTitleFromStem(const std::filesystem::path& p) {
    QString cleaned = omni::core::pathToQString(p.stem()).trimmed();
    static const QRegularExpression guillemetRe(QStringLiteral(R"(^《(.+)》$)"));
    const auto m = guillemetRe.match(cleaned);
    if (m.hasMatch()) {
        cleaned = m.captured(1).trimmed();
    }
    return cleaned.isEmpty() ? std::string("未命名书籍") : cleaned.toUtf8().toStdString();
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:
                // Strip illegal XML 1.0 control characters (< 0x20 except \t, \n, \r)
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

QString decodeHtmlEntities(QString text) {
    if (!text.contains('&')) return text;
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&ldquo;"), QStringLiteral("“"));
    text.replace(QStringLiteral("&rdquo;"), QStringLiteral("”"));
    text.replace(QStringLiteral("&lsquo;"), QStringLiteral("‘"));
    text.replace(QStringLiteral("&rsquo;"), QStringLiteral("’"));
    text.replace(QStringLiteral("&mdash;"), QStringLiteral("—"));
    text.replace(QStringLiteral("&ndash;"), QStringLiteral("–"));
    text.replace(QStringLiteral("&hellip;"), QStringLiteral("…"));

    static const QRegularExpression numDecRe(QStringLiteral(R"(&#(\d+);)"));
    auto itDec = numDecRe.globalMatch(text);
    int offset = 0;
    QString rebuilt;
    int lastPos = 0;
    while (itDec.hasNext()) {
        const auto m = itDec.next();
        rebuilt += text.mid(lastPos, m.capturedStart() - lastPos);
        const char32_t code = static_cast<char32_t>(m.captured(1).toUInt());
        if (code > 0 && code <= 0x10FFFF) {
            rebuilt += QString::fromUcs4(&code, 1);
        }
        lastPos = m.capturedEnd();
        (void)offset;
    }
    rebuilt += text.mid(lastPos);
    text = rebuilt;

    static const QRegularExpression numHexRe(QStringLiteral(R"(&#[xX]([0-9a-fA-F]+);)"));
    auto itHex = numHexRe.globalMatch(text);
    rebuilt.clear();
    lastPos = 0;
    while (itHex.hasNext()) {
        const auto m = itHex.next();
        rebuilt += text.mid(lastPos, m.capturedStart() - lastPos);
        bool ok = false;
        const char32_t code = static_cast<char32_t>(m.captured(1).toUInt(&ok, 16));
        if (ok && code > 0 && code <= 0x10FFFF) {
            rebuilt += QString::fromUcs4(&code, 1);
        }
        lastPos = m.capturedEnd();
    }
    rebuilt += text.mid(lastPos);
    rebuilt.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return rebuilt;
}

std::uint16_t readBe16(const std::uint8_t* p) {
    return (static_cast<std::uint16_t>(p[0]) << 8) | static_cast<std::uint16_t>(p[1]);
}

std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

void writeBe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void writeBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void putBe16At(std::vector<std::uint8_t>& buf, std::size_t off, std::uint16_t v) {
    buf[off] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    buf[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

void putBe32At(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    buf[off] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    buf[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    buf[off + 3] = static_cast<std::uint8_t>(v & 0xFF);
}

// PalmDOC LZ77 decompressor for MOBI / AZW / AZW3 text records
std::vector<std::uint8_t> decompressPalmDocLz77(const std::uint8_t* data, std::size_t len) {
    std::vector<std::uint8_t> out;
    out.reserve(len * 2 + 256);
    std::size_t i = 0;
    while (i < len) {
        const std::uint8_t c = data[i++];
        if (c == 0x00) {
            out.push_back(0x00);
        } else if (c >= 0x01 && c <= 0x08) {
            const std::size_t count = std::min<std::size_t>(c, len - i);
            out.insert(out.end(), data + i, data + i + count);
            i += count;
        } else if (c <= 0x7F) {
            out.push_back(c);
        } else if (c >= 0xC0) {
            out.push_back(' ');
            out.push_back(static_cast<std::uint8_t>(c ^ 0x80));
        } else {
            // 0x80..0xBF: 2-byte LZ77 distance-length pair
            if (i >= len) break;
            const std::uint16_t pair = (static_cast<std::uint16_t>(c & 0x3F) << 8) | data[i++];
            const std::size_t dist = pair >> 3;
            const std::size_t copyLen = (pair & 0x07) + 3;
            if (dist == 0 || dist > out.size()) continue;
            for (std::size_t k = 0; k < copyLen; ++k) {
                const std::uint8_t byte = out[out.size() - dist];
                out.push_back(byte);
            }
        }
    }
    return out;
}

// Trim trailing overlap/indexing bytes defined by MOBI extraDataFlags
std::size_t trimMobiRecordLength(const std::uint8_t* rec, std::size_t len, std::uint32_t extraFlags) {
    if (len == 0 || extraFlags == 0) return len;
    // Bits 1..15 represent variable-length trailing entries ending with backward varint size
    for (int bit = 15; bit >= 1; --bit) {
        if ((extraFlags & (1u << bit)) != 0 && len > 0) {
            std::size_t size = 0;
            std::size_t shift = 0;
            std::size_t pos = len;
            while (pos > 0 && shift < 28) {
                const std::uint8_t b = rec[--pos];
                size |= static_cast<std::size_t>(b & 0x7F) << shift;
                shift += 7;
                if ((b & 0x80) != 0) break;
            }
            if (size > 0 && size <= len) {
                len -= size;
            }
        }
    }
    // Bit 0: multibyte character overlap count stored in final byte (& 0x03) + 1
    if ((extraFlags & 1u) != 0 && len > 0) {
        const std::size_t extra = (rec[len - 1] & 0x03u) + 1u;
        if (extra <= len) len -= extra;
    }
    return len;
}

BookDocument parseMobiNative(const std::filesystem::path& inPath) {
    QFile f(omni::core::pathToQString(inPath));
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("无法打开 MOBI/AZW 文件");
    }
    const QByteArray qRaw = f.readAll();
    const std::vector<std::uint8_t> raw(qRaw.begin(), qRaw.end());

    if (raw.size() < 86) {
        throw std::runtime_error("无效的 MOBI/PDB 文件头");
    }

    const std::string magic(reinterpret_cast<const char*>(&raw[60]), 8);
    if (magic != "BOOKMOBI" && magic != "TEXtREAd") {
        throw std::runtime_error("非标准 MOBI/PalmDOC 格式");
    }

    const std::uint16_t numRecords = readBe16(&raw[76]);
    if (numRecords < 2 || 78 + static_cast<std::size_t>(numRecords) * 8 > raw.size()) {
        throw std::runtime_error("MOBI 记录表损坏");
    }

    std::vector<std::pair<std::size_t, std::size_t>> recRanges;
    recRanges.reserve(numRecords);
    for (std::uint16_t i = 0; i < numRecords; ++i) {
        const std::size_t start = readBe32(&raw[78 + i * 8]);
        const std::size_t end = (i + 1 < numRecords) ? readBe32(&raw[78 + (i + 1) * 8]) : raw.size();
        if (start < raw.size() && end <= raw.size() && end >= start) {
            recRanges.emplace_back(start, end - start);
        }
    }
    if (recRanges.size() < 2 || recRanges[0].second < 16) {
        throw std::runtime_error("MOBI Record 0 不完整");
    }

    const std::uint8_t* rec0 = &raw[recRanges[0].first];
    const std::size_t rec0Len = recRanges[0].second;
    const std::uint16_t compression = readBe16(rec0);
    const std::uint16_t numTextRecs = readBe16(rec0 + 8);
    const std::uint16_t encryption = readBe16(rec0 + 12);
    if (encryption != 0) {
        throw std::runtime_error("该 MOBI/AZW 电子书受 DRM 加密保护，无法离线解包");
    }

    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    std::uint32_t extraDataFlags = 0;
    if (rec0Len >= 36 && std::memcmp(rec0 + 16, "MOBI", 4) == 0) {
        const std::uint32_t mobiHdrLen = readBe32(rec0 + 20);
        const std::uint32_t fullNameOff = (rec0Len >= 88) ? readBe32(rec0 + 84) : 0;
        const std::uint32_t fullNameLen = (rec0Len >= 92) ? readBe32(rec0 + 88) : 0;
        const std::uint32_t exthFlags = (rec0Len >= 132) ? readBe32(rec0 + 128) : 0;
        if (mobiHdrLen >= 228 && 16 + 228 <= rec0Len) {
            extraDataFlags = readBe32(rec0 + 16 + 224);
        }

        if (fullNameOff > 0 && fullNameLen > 0 && fullNameOff + fullNameLen <= rec0Len) {
            std::vector<std::uint8_t> tBytes(rec0 + fullNameOff, rec0 + fullNameOff + fullNameLen);
            const QString qt = QString::fromUtf8(omni::infra::TextEncoding::decodeSmartToUtf8(tBytes).c_str()).trimmed();
            if (!qt.isEmpty()) doc.title = qt.toUtf8().toStdString();
        }

        if ((exthFlags & 0x40u) != 0 && 16 + mobiHdrLen + 12 <= rec0Len) {
            const std::size_t exthPos = 16 + mobiHdrLen;
            if (std::memcmp(rec0 + exthPos, "EXTH", 4) == 0) {
                const std::uint32_t exthCount = readBe32(rec0 + exthPos + 8);
                std::size_t cur = exthPos + 12;
                for (std::uint32_t k = 0; k < exthCount && cur + 8 <= rec0Len; ++k) {
                    const std::uint32_t recType = readBe32(rec0 + cur);
                    const std::uint32_t recLen = readBe32(rec0 + cur + 4);
                    if (recLen < 8 || cur + recLen > rec0Len) break;
                    std::vector<std::uint8_t> val(rec0 + cur + 8, rec0 + cur + recLen);
                    const QString qVal = QString::fromUtf8(omni::infra::TextEncoding::decodeSmartToUtf8(val).c_str()).trimmed();
                    if (!qVal.isEmpty()) {
                        if (recType == 100) doc.author = qVal.toUtf8().toStdString();
                        if (recType == 503) doc.title = qVal.toUtf8().toStdString();
                    }
                    cur += recLen;
                }
            }
        }
    }

    if (compression != 1 && compression != 2) {
        // Fallback to Calibre CLI if Huff/CDIC (compression == 17480) and Calibre is installed
        if (const auto cal = omni::infra::PortableEnv::resolveCalibreConvert()) {
            QTemporaryDir tmpDir;
            if (tmpDir.isValid()) {
                const QString tmpEpub = tmpDir.filePath(QStringLiteral("unpacked.epub"));
                QProcess proc;
                proc.start(omni::core::pathToQString(*cal), {omni::core::pathToQString(inPath), tmpEpub});
                if (proc.waitForFinished(60000) && proc.exitCode() == 0) {
                    return NovelEngine::parseNovelToDocument(omni::core::qStringToPath(tmpEpub));
                }
            }
        }
        throw std::runtime_error("当前 MOBI 采用罕见 Huff/CDIC 压缩，请安装可选 Calibre 组件支持");
    }

    std::vector<std::uint8_t> htmlStream;
    htmlStream.reserve(static_cast<std::size_t>(numTextRecs) * 4096);
    const std::size_t maxRec = std::min<std::size_t>(recRanges.size() - 1, numTextRecs);

    for (std::size_t r = 1; r <= maxRec; ++r) {
        const std::uint8_t* ptr = &raw[recRanges[r].first];
        const std::size_t trimmedLen = trimMobiRecordLength(ptr, recRanges[r].second, extraDataFlags);
        if (compression == 1) {
            htmlStream.insert(htmlStream.end(), ptr, ptr + trimmedLen);
        } else {
            const auto dec = decompressPalmDocLz77(ptr, trimmedLen);
            htmlStream.insert(htmlStream.end(), dec.begin(), dec.end());
        }
    }

    const std::string htmlUtf8 = omni::infra::TextEncoding::decodeSmartToUtf8(htmlStream);
    doc.chapters = NovelEngine::extractChaptersFromHtml(htmlUtf8, "正文");
    return doc;
}

BookDocument parseEpubFile(const std::filesystem::path& inPath) {
    omni::infra::ZipReader zip;
    std::string err;
    if (!zip.open(inPath, &err)) {
        throw std::runtime_error("读取 EPUB 失败: " + err);
    }

    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    // 1. Locate OPF path from META-INF/container.xml
    std::string opfPath;
    if (const auto containerXml = zip.readEntryUtf8("META-INF/container.xml")) {
        QXmlStreamReader xml(QByteArray::fromStdString(*containerXml));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("rootfile")) {
                const QString fp = xml.attributes().value(QStringLiteral("full-path")).toString();
                if (!fp.isEmpty()) {
                    opfPath = fp.toUtf8().toStdString();
                    break;
                }
            }
        }
    }
    if (opfPath.empty()) {
        for (const auto& name : zip.entryNames()) {
            if (name.size() > 4 && name.substr(name.size() - 4) == ".opf") {
                opfPath = name;
                break;
            }
        }
    }

    std::string opfDir;
    if (const auto slash = opfPath.rfind('/'); slash != std::string::npos) {
        opfDir = opfPath.substr(0, slash + 1);
    }

    std::vector<std::string> orderedHtmlEntries;
    std::unordered_set<std::string> seenEntries;

    if (!opfPath.empty()) {
        if (const auto opfXml = zip.readEntryUtf8(opfPath)) {
            std::unordered_map<std::string, std::pair<std::string, std::string>> manifest; // id -> (resolvedPath, mediaType)
            std::vector<std::string> spineIds;

            QXmlStreamReader xml(QByteArray::fromStdString(*opfXml));
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                const auto local = xml.name();
                if (local == QStringLiteral("title")) {
                    const QString t = xml.readElementText().trimmed();
                    if (!t.isEmpty()) doc.title = t.toUtf8().toStdString();
                } else if (local == QStringLiteral("creator")) {
                    const QString a = xml.readElementText().trimmed();
                    if (!a.isEmpty()) doc.author = a.toUtf8().toStdString();
                } else if (local == QStringLiteral("item")) {
                    const QString id = xml.attributes().value(QStringLiteral("id")).toString();
                    QString href = xml.attributes().value(QStringLiteral("href")).toString();
                    const QString mt = xml.attributes().value(QStringLiteral("media-type")).toString();
                    if (!id.isEmpty() && !href.isEmpty()) {
                        const int hashIdx = href.indexOf('#');
                        if (hashIdx >= 0) href = href.left(hashIdx);
                        href = QUrl::fromPercentEncoding(href.toUtf8());
                        std::string fullHref = opfDir + href.toUtf8().toStdString();
                        manifest[id.toUtf8().toStdString()] = {fullHref, mt.toUtf8().toStdString()};
                    }
                } else if (local == QStringLiteral("itemref")) {
                    const QString idref = xml.attributes().value(QStringLiteral("idref")).toString();
                    if (!idref.isEmpty()) spineIds.push_back(idref.toUtf8().toStdString());
                }
            }

            auto isDocMedia = [](const std::string& mt, const std::string& path) {
                if (mt.find("xhtml") != std::string::npos || mt.find("html") != std::string::npos) {
                    const QString qp = QString::fromUtf8(path.c_str()).toLower();
                    return !qp.endsWith(QStringLiteral("nav.xhtml")) && !qp.contains(QStringLiteral("/nav."));
                }
                return false;
            };

            for (const auto& sid : spineIds) {
                auto it = manifest.find(sid);
                if (it != manifest.end() && isDocMedia(it->second.second, it->second.first)) {
                    if (seenEntries.insert(it->second.first).second) {
                        orderedHtmlEntries.push_back(it->second.first);
                    }
                }
            }
            for (const auto& kv : manifest) {
                if (isDocMedia(kv.second.second, kv.second.first)) {
                    if (seenEntries.insert(kv.second.first).second) {
                        orderedHtmlEntries.push_back(kv.second.first);
                    }
                }
            }
        }
    }

    if (orderedHtmlEntries.empty()) {
        for (const auto& name : zip.entryNames()) {
            const QString qn = QString::fromUtf8(name.c_str()).toLower();
            if ((qn.endsWith(".xhtml") || qn.endsWith(".html") || qn.endsWith(".htm")) && !qn.endsWith("nav.xhtml")) {
                orderedHtmlEntries.push_back(name);
            }
        }
    }

    int idx = 1;
    for (const auto& entryName : orderedHtmlEntries) {
        if (const auto bytes = zip.readEntry(entryName)) {
            const std::string htmlStr = omni::infra::TextEncoding::decodeSmartToUtf8(*bytes);
            auto extracted = NovelEngine::extractChaptersFromHtml(htmlStr, "第 " + std::to_string(idx) + " 章");
            for (auto& ch : extracted) {
                if (!ch.paragraphs.empty()) {
                    doc.chapters.push_back(std::move(ch));
                }
            }
            ++idx;
        }
    }

    if (doc.chapters.empty()) {
        doc.chapters.push_back(Chapter{"正文", {"（空白文档）"}});
    }
    return doc;
}

BookDocument parseDocxFile(const std::filesystem::path& inPath) {
    omni::infra::ZipReader zip;
    std::string err;
    if (!zip.open(inPath, &err)) {
        throw std::runtime_error("读取 DOCX 失败: " + err);
    }

    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    if (const auto coreXml = zip.readEntryUtf8("docProps/core.xml")) {
        QXmlStreamReader xml(QByteArray::fromStdString(*coreXml));
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QStringLiteral("title")) {
                const QString t = xml.readElementText().trimmed();
                if (!t.isEmpty()) doc.title = t.toUtf8().toStdString();
            } else if (xml.name() == QStringLiteral("creator")) {
                const QString a = xml.readElementText().trimmed();
                if (!a.isEmpty()) doc.author = a.toUtf8().toStdString();
            }
        }
    }

    const auto docXml = zip.readEntryUtf8("word/document.xml");
    if (!docXml) {
        throw std::runtime_error("DOCX 缺少 word/document.xml 正文");
    }

    std::string curTitle = "正文";
    std::vector<std::string> curParas;

    QXmlStreamReader xml(QByteArray::fromStdString(*docXml));
    bool inPara = false;
    bool paraIsHeading = false;
    QString paraText;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QStringLiteral("p")) {
                inPara = true;
                paraIsHeading = false;
                paraText.clear();
            } else if (inPara && name == QStringLiteral("pStyle")) {
                const QString val = xml.attributes().value(QStringLiteral("val")).toString().toLower();
                const QString wVal = xml.attributes().value(QStringLiteral("w:val")).toString().toLower();
                const QString style = val.isEmpty() ? wVal : val;
                if (style.startsWith("heading1") || style.startsWith("heading2") ||
                    style.startsWith("heading 1") || style.startsWith("heading 2") ||
                    style == "1" || style == "2" || style.contains(QStringLiteral("标题"))) {
                    paraIsHeading = true;
                }
            } else if (inPara && name == QStringLiteral("t")) {
                paraText += xml.readElementText();
            } else if (inPara && name == QStringLiteral("br")) {
                paraText += '\n';
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("p") && inPara) {
            inPara = false;
            const QString txt = paraText.trimmed();
            if (!txt.isEmpty()) {
                if (paraIsHeading || isChapterHeadingLine(txt)) {
                    if (!curParas.empty()) {
                        doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
                        curParas.clear();
                    }
                    curTitle = txt.simplified().toUtf8().toStdString();
                } else {
                    for (const QString& sub : txt.split('\n')) {
                        const QString norm = normalizeParagraphQ(sub);
                        if (!norm.isEmpty()) curParas.push_back(norm.toUtf8().toStdString());
                    }
                }
            }
        }
    }

    if (!curParas.empty()) {
        doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
    }
    if (doc.chapters.empty()) {
        doc.chapters.push_back(Chapter{"正文", {"（空白文档）"}});
    }
    return doc;
}

BookDocument parseFb2File(const std::filesystem::path& inPath) {
    const std::string xmlStr = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    static const QRegularExpression btRe(QStringLiteral(R"(<book-title[^>]*>(.*?)</book-title>)"),
                                         QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    if (const auto m = btRe.match(QString::fromUtf8(xmlStr.c_str())); m.hasMatch()) {
        const QString t = decodeHtmlEntities(m.captured(1)).remove(QRegularExpression(QStringLiteral("<[^>]+>"))).trimmed();
        if (!t.isEmpty()) doc.title = t.toUtf8().toStdString();
    }
    static const QRegularExpression auRe(QStringLiteral(R"(<author[^>]*>(.*?)</author>)"),
                                         QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    if (const auto m = auRe.match(QString::fromUtf8(xmlStr.c_str())); m.hasMatch()) {
        const QString a = decodeHtmlEntities(m.captured(1)).replace(QRegularExpression(QStringLiteral("<[^>]+>")), QStringLiteral(" ")).simplified();
        if (!a.isEmpty()) doc.author = a.toUtf8().toStdString();
    }

    // Parse leaf <section> elements
    QXmlStreamReader xml(QByteArray::fromStdString(xmlStr));
    struct SecFrame {
        QString title;
        std::vector<std::string> paras;
        bool hasChildSection = false;
        bool inTitle = false;
    };
    std::vector<SecFrame> stack;
    int secCounter = 0;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QStringLiteral("section")) {
                if (!stack.empty()) stack.back().hasChildSection = true;
                stack.push_back(SecFrame{});
            } else if (!stack.empty() && name == QStringLiteral("title")) {
                stack.back().inTitle = true;
            } else if (!stack.empty() && name == QStringLiteral("p")) {
                const QString pTxt = normalizeParagraphQ(xml.readElementText(QXmlStreamReader::IncludeChildElements));
                if (!pTxt.isEmpty()) {
                    if (stack.back().inTitle) {
                        if (!stack.back().title.isEmpty()) stack.back().title += ' ';
                        stack.back().title += pTxt;
                    } else {
                        stack.back().paras.push_back(pTxt.toUtf8().toStdString());
                    }
                }
            }
        } else if (xml.isEndElement()) {
            const auto name = xml.name();
            if (!stack.empty() && name == QStringLiteral("title")) {
                stack.back().inTitle = false;
            } else if (!stack.empty() && name == QStringLiteral("section")) {
                SecFrame finished = std::move(stack.back());
                stack.pop_back();
                if (!finished.hasChildSection && !finished.paras.empty()) {
                    ++secCounter;
                    const std::string chTitle = finished.title.trimmed().isEmpty()
                                                    ? ("第 " + std::to_string(secCounter) + " 节")
                                                    : finished.title.trimmed().toUtf8().toStdString();
                    doc.chapters.push_back(Chapter{chTitle, std::move(finished.paras)});
                }
            }
        }
    }

    if (doc.chapters.empty()) {
        doc.chapters = NovelEngine::extractChaptersFromHtml(xmlStr, "正文");
    }
    return doc;
}

BookDocument parseMdFile(const std::filesystem::path& inPath) {
    const std::string raw = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    static const QRegularExpression h1Re(QStringLiteral(R"(^#\s+(.+)$)"));
    static const QRegularExpression h2Re(QStringLiteral(R"(^#{2,3}\s+(.+)$)"));

    std::string curTitle = "正文";
    std::vector<std::string> curParas;

    const QString qRaw = QString::fromUtf8(raw.c_str());
    for (const QString& line : qRaw.split('\n')) {
        const QString s = line.trimmed();
        if (s.isEmpty()) continue;

        const auto m1 = h1Re.match(s);
        const auto m2 = h2Re.match(s);
        if (m1.hasMatch() && doc.chapters.empty() && curParas.empty()) {
            doc.title = m1.captured(1).trimmed().toUtf8().toStdString();
            continue;
        }
        if (m1.hasMatch() || m2.hasMatch() || isChapterHeadingLine(s)) {
            const QString heading = m1.hasMatch() ? m1.captured(1).trimmed()
                                                  : (m2.hasMatch() ? m2.captured(1).trimmed() : s);
            if (!curParas.empty()) {
                doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
                curParas.clear();
            }
            curTitle = heading.toUtf8().toStdString();
        } else {
            const QString norm = normalizeParagraphQ(s);
            if (!norm.isEmpty()) curParas.push_back(norm.toUtf8().toStdString());
        }
    }

    if (!curParas.empty()) {
        doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
    }
    if (doc.chapters.empty()) {
        doc.chapters = NovelEngine::splitTextIntoChapters(raw);
    }
    return doc;
}

// Pure C++ fallback PDF FlateDecode & ToUnicode CMap content-stream text extractor (used on Windows when Qt6Pdf is absent)
std::string extractPdfStreamsFallback(const std::filesystem::path& pdfPath) {
    QFile f(omni::core::pathToQString(pdfPath));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    const std::string bin(raw.constData(), static_cast<std::size_t>(raw.size()));

    // Check for embedded OmniConvert full-text attachment stream first
    {
        const std::string markerStart = "OMNICONVERT_UTF8_TEXT_BEGIN\n";
        const std::string markerEnd = "\nOMNICONVERT_UTF8_TEXT_END";
        std::size_t pos = 0;
        std::vector<std::string> inflatedStreams;
        while ((pos = bin.find("stream", pos)) != std::string::npos) {
            std::size_t dataStart = pos + 6;
            if (dataStart < bin.size() && bin[dataStart] == '\r') ++dataStart;
            if (dataStart < bin.size() && bin[dataStart] == '\n') ++dataStart;
            else { pos += 6; continue; }
            const std::size_t endPos = bin.find("endstream", dataStart);
            if (endPos == std::string::npos) break;
            std::size_t dataLen = endPos - dataStart;
            while (dataLen > 0 && (bin[dataStart + dataLen - 1] == '\n' || bin[dataStart + dataLen - 1] == '\r')) --dataLen;
            std::string content;
            if (dataLen > 2) {
                z_stream zs{};
                if (inflateInit(&zs) == Z_OK) {
                    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bin.data() + dataStart));
                    zs.avail_in = static_cast<uInt>(dataLen);
                    char outBuf[16384];
                    int rc = Z_OK;
                    while (rc == Z_OK) {
                        zs.next_out = reinterpret_cast<Bytef*>(outBuf);
                        zs.avail_out = sizeof(outBuf);
                        rc = inflate(&zs, Z_NO_FLUSH);
                        const std::size_t have = sizeof(outBuf) - zs.avail_out;
                        if (have > 0) content.append(outBuf, have);
                    }
                    inflateEnd(&zs);
                }
            }
            if (content.empty()) content.assign(bin.data() + dataStart, dataLen);
            const auto ms = content.find(markerStart);
            if (ms != std::string::npos) {
                const auto me = content.find(markerEnd, ms + markerStart.size());
                if (me != std::string::npos) {
                    return content.substr(ms + markerStart.size(), me - (ms + markerStart.size()));
                }
            }
            inflatedStreams.push_back(std::move(content));
            pos = endPos + 9;
        }

        // Pass 1: Build GlyphID -> Unicode map from all ToUnicode CMap streams (beginbfchar / beginbfrange)
        std::unordered_map<uint, QString> glyphToUni;
        static const QRegularExpression bfCharPairRe(QStringLiteral(R"(<([0-9A-Fa-f]{2,4})>\s*<([0-9A-Fa-f]{4,8})>)"));
        static const QRegularExpression bfRangeRe(QStringLiteral(R"(<([0-9A-Fa-f]{2,4})>\s*<([0-9A-Fa-f]{2,4})>\s*<([0-9A-Fa-f]{4,8})>)"));
        for (const auto& st : inflatedStreams) {
            if (st.find("beginbfchar") == std::string::npos && st.find("beginbfrange") == std::string::npos) continue;
            const QString qSt = QString::fromLatin1(st.data(), static_cast<qsizetype>(st.size()));
            auto itChar = bfCharPairRe.globalMatch(qSt);
            while (itChar.hasNext()) {
                const auto m = itChar.next();
                bool ok1 = false;
                const uint gid = m.captured(1).toUInt(&ok1, 16);
                const QString dstHex = m.captured(2);
                if (ok1 && dstHex.size() % 4 == 0) {
                    QString uni;
                    for (int k = 0; k + 4 <= dstHex.size(); k += 4) {
                        bool ok2 = false;
                        const ushort u = dstHex.mid(k, 4).toUShort(&ok2, 16);
                        if (ok2 && u >= 0x20) uni.append(QChar(u));
                    }
                    if (!uni.isEmpty()) glyphToUni[gid] = uni;
                }
            }
            auto itRange = bfRangeRe.globalMatch(qSt);
            while (itRange.hasNext()) {
                const auto m = itRange.next();
                bool ok1 = false, ok2 = false, ok3 = false;
                const uint startGid = m.captured(1).toUInt(&ok1, 16);
                const uint endGid = m.captured(2).toUInt(&ok2, 16);
                const uint baseUni = m.captured(3).left(4).toUInt(&ok3, 16);
                if (ok1 && ok2 && ok3 && endGid >= startGid && (endGid - startGid) <= 4096) {
                    for (uint g = startGid; g <= endGid; ++g) {
                        glyphToUni[g] = QString(QChar(static_cast<ushort>(baseUni + (g - startGid))));
                    }
                }
            }
        }

        // Pass 2: Decode text operators from page content streams
        QString collected;
        static const QRegularExpression hexStrRe(QStringLiteral(R"(<([0-9A-Fa-f]{4,})>)"));
        static const QRegularExpression litStrRe(QStringLiteral(R"(\(([^()\\]*(?:\\.[^()\\]*)*)\)\s*(?:Tj|'))"));
        for (const auto& content : inflatedStreams) {
            if (content.find("BT") == std::string::npos || content.find("beginbfchar") != std::string::npos) continue;
            const QString qContent = QString::fromLatin1(content.data(), static_cast<qsizetype>(content.size()));
            auto itHex = hexStrRe.globalMatch(qContent);
            while (itHex.hasNext()) {
                const QString hex = itHex.next().captured(1);
                if (hex.startsWith(QStringLiteral("FEFF"), Qt::CaseInsensitive) && hex.size() % 4 == 0) {
                    for (int k = 4; k + 4 <= hex.size(); k += 4) {
                        bool ok = false;
                        const ushort u = hex.mid(k, 4).toUShort(&ok, 16);
                        if (ok && u >= 0x20) collected.append(QChar(u));
                    }
                } else if (hex.size() % 4 == 0) {
                    for (int k = 0; k + 4 <= hex.size(); k += 4) {
                        bool ok = false;
                        const uint gid = hex.mid(k, 4).toUInt(&ok, 16);
                        if (!ok) continue;
                        auto itMap = glyphToUni.find(gid);
                        if (itMap != glyphToUni.end()) {
                            collected += itMap->second;
                        } else if (gid >= 0x20 && gid <= 0x9FFF) {
                            collected.append(QChar(static_cast<ushort>(gid)));
                        }
                    }
                }
            }
            collected.append('\n');
            auto itLit = litStrRe.globalMatch(qContent);
            while (itLit.hasNext()) {
                collected += itLit.next().captured(1) + '\n';
            }
        }
        return collected.toUtf8().toStdString();
    }
}

BookDocument parsePdfFile(const std::filesystem::path& inPath) {
    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

#ifdef OMNI_HAVE_QTPDF
    {
        QPdfDocument pdfDoc;
        if (pdfDoc.load(omni::core::pathToQString(inPath)) == QPdfDocument::Error::None && pdfDoc.pageCount() > 0) {
            const QString metaTitle = pdfDoc.metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
            const QString metaAuthor = pdfDoc.metaData(QPdfDocument::MetaDataField::Author).toString().trimmed();
            if (!metaTitle.isEmpty()) doc.title = metaTitle.toUtf8().toStdString();
            if (!metaAuthor.isEmpty()) doc.author = metaAuthor.toUtf8().toStdString();

            QStringList pages;
            for (int p = 0; p < pdfDoc.pageCount(); ++p) {
                pages << pdfDoc.getAllText(p).text();
            }
            doc.chapters = NovelEngine::splitTextIntoChapters(pages.join('\n').toUtf8().toStdString(), true);
            if (!doc.chapters.empty()) return doc;
        }
    }
#endif

#ifdef __APPLE__
    if (macos_pdf::extractPdfDocument(inPath, doc)) {
        return doc;
    }
#endif

    // Optional Calibre fallback if installed
    if (const auto cal = omni::infra::PortableEnv::resolveCalibreConvert()) {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            const QString tmpTxt = tmpDir.filePath(QStringLiteral("pdf_extracted.txt"));
            QProcess proc;
            proc.start(omni::core::pathToQString(*cal), {omni::core::pathToQString(inPath), tmpTxt});
            if (proc.waitForFinished(60000) && proc.exitCode() == 0) {
                const std::string txt = omni::infra::TextEncoding::readFileSmartUtf8(omni::core::qStringToPath(tmpTxt));
                doc.chapters = NovelEngine::splitTextIntoChapters(txt, true);
                return doc;
            }
        }
    }

    const std::string streamTxt = extractPdfStreamsFallback(inPath);
    if (!streamTxt.empty()) {
        doc.chapters = NovelEngine::splitTextIntoChapters(streamTxt, true);
        return doc;
    }

    throw std::runtime_error("无法从该 PDF 提取文本（可能是纯扫描图片版 PDF）");
}

std::vector<std::uint8_t> encodeMobiHtmlBoundarySafe(const std::string& utf8Html) {
    const QString qStr = QString::fromUtf8(utf8Html.data(), static_cast<qsizetype>(utf8Html.size()));
    const QList<uint> ucs4 = qStr.toUcs4();

    std::vector<std::uint8_t> buf;
    buf.reserve(utf8Html.size() + 512);

    for (uint rawCode : ucs4) {
        const char32_t code = static_cast<char32_t>(rawCode);
        if (code < 128) {
            buf.push_back(static_cast<std::uint8_t>(code));
        } else {
            const QByteArray u8 = QString::fromUcs4(&code, 1).toUtf8();
            const std::size_t rem = 4096 - (buf.size() % 4096);
            if (static_cast<std::size_t>(u8.size()) > rem) {
                const std::string entity = "&#" + std::to_string(code) + ";";
                buf.insert(buf.end(), entity.begin(), entity.end());
            } else {
                buf.insert(buf.end(), u8.begin(), u8.end());
            }
        }
    }
    if (buf.empty()) buf.push_back(' ');
    return buf;
}

} // namespace

std::vector<Chapter> NovelEngine::splitTextIntoChapters(const std::string& rawUtf8Text, bool autoSplitLong) {
    QString normalized = QString::fromUtf8(rawUtf8Text.data(), static_cast<qsizetype>(rawUtf8Text.size()));
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace('\r', '\n');

    std::vector<Chapter> chapters;
    std::string currentTitle = "序言 / 开始";
    std::vector<std::string> currentParas;
    bool foundExplicitChapter = false;

    for (const QString& rawLine : normalized.split('\n')) {
        const QString stripped = rawLine.trimmed();
        if (stripped.isEmpty()) continue;

        if (isChapterHeadingLine(stripped)) {
            if (!currentParas.empty()) {
                chapters.push_back(Chapter{currentTitle, std::move(currentParas)});
                currentParas.clear();
            }
            currentTitle = stripped.toUtf8().toStdString();
            foundExplicitChapter = true;
        } else {
            const QString norm = normalizeParagraphQ(stripped);
            if (!norm.isEmpty()) {
                currentParas.push_back(norm.toUtf8().toStdString());
            }
        }
    }

    if (!currentParas.empty()) {
        if (!foundExplicitChapter && chapters.empty()) {
            currentTitle = "正文";
        }
        chapters.push_back(Chapter{currentTitle, std::move(currentParas)});
    }

    // Smoothly chunk long unchaptered TXT novels (> 220 paragraphs -> 160 paragraphs per section)
    if (autoSplitLong && chapters.size() == 1 && !foundExplicitChapter && chapters[0].paragraphs.size() > 220) {
        const auto allParas = std::move(chapters[0].paragraphs);
        chapters.clear();
        const std::size_t chunkSize = 160;
        std::size_t secIdx = 1;
        for (std::size_t i = 0; i < allParas.size(); i += chunkSize, ++secIdx) {
            const std::size_t end = std::min(allParas.size(), i + chunkSize);
            chapters.push_back(Chapter{
                "第 " + std::to_string(secIdx) + " 节",
                std::vector<std::string>(allParas.begin() + static_cast<std::ptrdiff_t>(i),
                                         allParas.begin() + static_cast<std::ptrdiff_t>(end))});
        }
    }

    if (chapters.empty()) {
        chapters.push_back(Chapter{"正文", {"（空白文档）"}});
    }
    return chapters;
}

std::vector<Chapter> NovelEngine::extractChaptersFromHtml(const std::string& htmlUtf8, const std::string& fallbackTitle) {
    QString html = QString::fromUtf8(htmlUtf8.data(), static_cast<qsizetype>(htmlUtf8.size()));

    // Strip <script>, <style>, <nav>, <noscript>, <head>
    static const QRegularExpression stripTagsRe(
        QStringLiteral(R"(<(script|style|nav|noscript|head)\b[^>]*>.*?</\1>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    html.remove(stripTagsRe);

    // Replace <br> and <mbp:pagebreak> with newlines
    static const QRegularExpression brRe(QStringLiteral(R"(<(?:br|mbp:pagebreak)\b[^>]*/?>)"),
                                         QRegularExpression::CaseInsensitiveOption);
    html.replace(brRe, QStringLiteral("\n"));

    // Mark headings explicitly with sentinel markers before stripping tags so h1/h2/h3 become chapter titles
    static const QRegularExpression headingRe(
        QStringLiteral(R"(<(h[1-3])\b[^>]*>(.*?)</\1>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    html.replace(headingRe, QStringLiteral("\n\x1EHEADING_START\x1F\\2\x1EHEADING_END\x1F\n"));

    // Insert newline around block elements
    static const QRegularExpression blockCloseRe(
        QStringLiteral(R"(</?(?:p|div|blockquote|li|section|article|tr)\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(blockCloseRe, QStringLiteral("\n"));

    // Remove remaining inline tags
    static const QRegularExpression anyTagRe(QStringLiteral(R"(<[^>]+>)"));
    html.remove(anyTagRe);
    html = decodeHtmlEntities(html);

    std::vector<Chapter> chapters;
    std::string curTitle = fallbackTitle;
    std::vector<std::string> curParas;
    bool hasHeading = false;

    for (const QString& rawLine : html.split('\n')) {
        QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        if (line.contains(QStringLiteral("\x1EHEADING_START\x1F"))) {
            line.remove(QStringLiteral("\x1EHEADING_START\x1F"));
            line.remove(QStringLiteral("\x1EHEADING_END\x1F"));
            const QString cleanH = line.simplified();
            if (!cleanH.isEmpty()) {
                if (!curParas.empty()) {
                    chapters.push_back(Chapter{curTitle, std::move(curParas)});
                    curParas.clear();
                }
                curTitle = cleanH.toUtf8().toStdString();
                hasHeading = true;
            }
            continue;
        }

        if (isChapterHeadingLine(line)) {
            if (!curParas.empty()) {
                chapters.push_back(Chapter{curTitle, std::move(curParas)});
                curParas.clear();
            }
            curTitle = line.toUtf8().toStdString();
            hasHeading = true;
        } else {
            const QString norm = normalizeParagraphQ(line);
            if (!norm.isEmpty()) {
                curParas.push_back(norm.toUtf8().toStdString());
            }
        }
    }

    if (!curParas.empty()) {
        chapters.push_back(Chapter{curTitle, std::move(curParas)});
    }
    if (chapters.empty() && !hasHeading) {
        return splitTextIntoChapters(html.toUtf8().toStdString());
    }
    return chapters;
}

BookDocument NovelEngine::parseNovelToDocument(const std::filesystem::path& inPath) {
    std::string ext = omni::core::pathToUtf8(inPath.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".txt") {
        const std::string txt = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
        return BookDocument{cleanBookTitleFromStem(inPath), "佚名", splitTextIntoChapters(txt, true)};
    }
    if (ext == ".epub") {
        return parseEpubFile(inPath);
    }
    if (ext == ".mobi" || ext == ".azw" || ext == ".azw3") {
        return parseMobiNative(inPath);
    }
    if (ext == ".pdf") {
        return parsePdfFile(inPath);
    }
    if (ext == ".docx") {
        return parseDocxFile(inPath);
    }
    if (ext == ".fb2") {
        return parseFb2File(inPath);
    }
    if (ext == ".md") {
        return parseMdFile(inPath);
    }
    if (ext == ".html" || ext == ".htm") {
        const std::string htmlStr = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
        std::string title = cleanBookTitleFromStem(inPath);
        static const QRegularExpression titleRe(
            QStringLiteral(R"(<title[^>]*>(.*?)</title>)"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        if (const auto m = titleRe.match(QString::fromUtf8(htmlStr.c_str())); m.hasMatch()) {
            const QString qt = decodeHtmlEntities(m.captured(1)).simplified();
            if (!qt.isEmpty()) title = qt.toUtf8().toStdString();
        }
        return BookDocument{title, "佚名", extractChaptersFromHtml(htmlStr, "正文")};
    }

    throw std::runtime_error("暂不支持读取的小说格式: " + ext);
}

void NovelEngine::writeBookToEpub(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent) {
    omni::infra::ZipWriter zip;

    // OCF specification: mimetype MUST be first entry and stored uncompressed
    zip.addTextFile("mimetype", "application/epub+zip", true);

    zip.addTextFile("META-INF/container.xml",
                    R"(<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
)");

    const std::string indentCss = autoIndent ? "2em" : "0";
    const std::string mainCss =
        "body {\n"
        "  font-family: \"PingFang SC\", \"Songti SC\", \"Microsoft YaHei\", serif;\n"
        "  line-height: 1.85;\n"
        "  margin: 4%;\n"
        "  color: #1f2328;\n"
        "}\n"
        "h1 {\n"
        "  font-size: 1.45em;\n"
        "  margin: 1.2em 0 1em 0;\n"
        "  padding-bottom: 0.35em;\n"
        "  border-bottom: 1px solid #e5e7eb;\n"
        "  text-align: center;\n"
        "}\n"
        "p {\n"
        "  text-indent: " + indentCss + ";\n"
        "  margin: 0.55em 0;\n"
        "  text-align: justify;\n"
        "}\n";
    zip.addTextFile("OEBPS/style/main.css", mainCss);

    const std::string bookUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const std::string escTitle = xmlEscape(doc.title);
    const std::string escAuthor = xmlEscape(doc.author.empty() ? "佚名" : doc.author);

    std::ostringstream manifestItems;
    std::ostringstream spineItems;
    std::ostringstream ncxPoints;
    std::ostringstream navLinks;

    for (std::size_t idx = 0; idx < doc.chapters.size(); ++idx) {
        char fnameBuf[32];
        std::snprintf(fnameBuf, sizeof(fnameBuf), "chap_%04zu.xhtml", idx + 1);
        const std::string fileName(fnameBuf);
        const std::string itemId = "chap_" + std::to_string(idx + 1);
        const std::string escChTitle = xmlEscape(doc.chapters[idx].title);

        std::ostringstream xhtml;
        xhtml << R"(<?xml version="1.0" encoding="utf-8"?>)" << "\n"
              << R"(<!DOCTYPE html>)" << "\n"
              << R"(<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="zh" lang="zh">)" << "\n"
              << "<head><meta charset=\"utf-8\"/><title>" << escChTitle << "</title>"
              << R"(<link rel="stylesheet" type="text/css" href="style/main.css"/></head>)" << "\n"
              << "<body><h1>" << escChTitle << "</h1>\n";
        for (const auto& p : doc.chapters[idx].paragraphs) {
            xhtml << "<p>" << xmlEscape(p) << "</p>\n";
        }
        xhtml << "</body></html>\n";
        zip.addTextFile("OEBPS/" + fileName, xhtml.str());

        manifestItems << "    <item id=\"" << itemId << "\" href=\"" << fileName
                      << "\" media-type=\"application/xhtml+xml\"/>\n";
        spineItems << "    <itemref idref=\"" << itemId << "\"/>\n";
        ncxPoints << "    <navPoint id=\"navPoint-" << (idx + 1) << "\" playOrder=\"" << (idx + 1) << "\">\n"
                  << "      <navLabel><text>" << escChTitle << "</text></navLabel>\n"
                  << "      <content src=\"" << fileName << "\"/>\n"
                  << "    </navPoint>\n";
        navLinks << "      <li><a href=\"" << fileName << "\">" << escChTitle << "</a></li>\n";
    }

    std::ostringstream navXhtml;
    navXhtml << R"(<?xml version="1.0" encoding="utf-8"?>)" << "\n"
             << R"(<!DOCTYPE html>)" << "\n"
             << R"(<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="zh">)" << "\n"
             << "<head><meta charset=\"utf-8\"/><title>" << escTitle << "</title></head>\n"
             << "<body><nav epub:type=\"toc\" id=\"toc\"><h1>目录</h1>\n    <ol>\n"
             << navLinks.str()
             << "    </ol>\n  </nav></body></html>\n";
    zip.addTextFile("OEBPS/nav.xhtml", navXhtml.str());

    std::ostringstream tocNcx;
    tocNcx << R"(<?xml version="1.0" encoding="utf-8"?>)" << "\n"
           << R"(<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">)" << "\n"
           << "  <head>\n"
           << "    <meta name=\"dtb:uid\" content=\"urn:uuid:" << bookUuid << "\"/>\n"
           << "    <meta name=\"dtb:depth\" content=\"1\"/>\n"
           << "    <meta name=\"dtb:totalPageCount\" content=\"0\"/>\n"
           << "    <meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n"
           << "  </head>\n"
           << "  <docTitle><text>" << escTitle << "</text></docTitle>\n"
           << "  <navMap>\n" << ncxPoints.str() << "  </navMap>\n</ncx>\n";
    zip.addTextFile("OEBPS/toc.ncx", tocNcx.str());

    const std::string modifiedUtc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")).toStdString();
    std::ostringstream opf;
    opf << R"(<?xml version="1.0" encoding="utf-8"?>)" << "\n"
        << R"(<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">)" << "\n"
        << R"(  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">)" << "\n"
        << "    <dc:identifier id=\"bookid\">urn:uuid:" << bookUuid << "</dc:identifier>\n"
        << "    <dc:title>" << escTitle << "</dc:title>\n"
        << "    <dc:creator>" << escAuthor << "</dc:creator>\n"
        << "    <dc:language>zh</dc:language>\n"
        << "    <meta property=\"dcterms:modified\">" << modifiedUtc << "</meta>\n"
        << "  </metadata>\n"
        << "  <manifest>\n"
        << "    <item id=\"style_main\" href=\"style/main.css\" media-type=\"text/css\"/>\n"
        << "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
        << "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
        << manifestItems.str()
        << "  </manifest>\n"
        << "  <spine toc=\"ncx\">\n"
        << "    <itemref idref=\"nav\"/>\n"
        << spineItems.str()
        << "  </spine>\n"
        << "</package>\n";
    zip.addTextFile("OEBPS/content.opf", opf.str());

    zip.writeToFile(outPath);
}

void NovelEngine::writeBookToTxt(
    const BookDocument& doc,
    const std::filesystem::path& outPath,
    const std::string& txtEncoding,
    bool autoIndent) {

    const std::string indentPrefix = autoIndent ? "  " : "";
    std::ostringstream ss;
    ss << "《" << doc.title << "》\n"
       << "作者：" << (doc.author.empty() ? "佚名" : doc.author) << "\n\n";
    for (const auto& ch : doc.chapters) {
        ss << ch.title << "\n\n";
        for (const auto& p : ch.paragraphs) {
            ss << indentPrefix << p << "\n";
        }
        ss << "\n";
    }
    const auto encoded = omni::infra::TextEncoding::encodeFromUtf8(ss.str(), txtEncoding);
    omni::infra::AtomicFileTx::writeBytesAtomic(outPath, encoded);
}

void NovelEngine::writeBookToMobiNative(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent) {
    const std::string indentStr = autoIndent ? "  " : "";
    const std::string authorStr = doc.author.empty() ? "佚名" : doc.author;

    std::ostringstream html;
    html << "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"/>"
         << "<title>" << xmlEscape(doc.title) << "</title></head><body>"
         << "<h1>" << xmlEscape(doc.title) << "</h1><p>作者：" << xmlEscape(authorStr) << "</p><mbp:pagebreak/>";
    for (const auto& ch : doc.chapters) {
        html << "<h2>" << xmlEscape(ch.title) << "</h2>";
        for (const auto& p : ch.paragraphs) {
            html << "<p>" << indentStr << xmlEscape(p) << "</p>";
        }
        html << "<mbp:pagebreak/>";
    }
    html << "</body></html>";

    const std::vector<std::uint8_t> rawHtmlBytes = encodeMobiHtmlBoundarySafe(html.str());
    const std::uint32_t textLength = static_cast<std::uint32_t>(rawHtmlBytes.size());

    std::vector<std::vector<std::uint8_t>> textRecords;
    for (std::size_t i = 0; i < rawHtmlBytes.size(); i += 4096) {
        const std::size_t chunkEnd = std::min<std::size_t>(rawHtmlBytes.size(), i + 4096);
        textRecords.emplace_back(rawHtmlBytes.begin() + static_cast<std::ptrdiff_t>(i),
                                 rawHtmlBytes.begin() + static_cast<std::ptrdiff_t>(chunkEnd));
    }
    const auto numTextRecords = static_cast<std::uint16_t>(textRecords.size());

    const std::string titleUtf8 = doc.title;
    const std::string langStr = "zh-CN";

    // Build EXTH Header
    std::vector<std::uint8_t> exthBody;
    auto appendExthItem = [&](std::uint32_t typeId, const std::string& val) {
        writeBe32(exthBody, typeId);
        writeBe32(exthBody, static_cast<std::uint32_t>(8 + val.size()));
        exthBody.insert(exthBody.end(), val.begin(), val.end());
    };
    appendExthItem(100, authorStr);
    appendExthItem(503, titleUtf8);
    appendExthItem(524, langStr);

    const std::uint32_t exthUnpaddedLen = static_cast<std::uint32_t>(12 + exthBody.size());
    const std::size_t exthPad = (4 - (exthUnpaddedLen % 4)) % 4;
    std::vector<std::uint8_t> exthHeader = {'E', 'X', 'T', 'H'};
    writeBe32(exthHeader, exthUnpaddedLen);
    writeBe32(exthHeader, 3);
    exthHeader.insert(exthHeader.end(), exthBody.begin(), exthBody.end());
    exthHeader.insert(exthHeader.end(), exthPad, 0x00);

    // PalmDOC Header (16 bytes)
    std::vector<std::uint8_t> palmdocHdr;
    writeBe16(palmdocHdr, 1); // 1 = uncompressed text records
    writeBe16(palmdocHdr, 0);
    writeBe32(palmdocHdr, textLength);
    writeBe16(palmdocHdr, numTextRecords);
    writeBe16(palmdocHdr, 4096);
    writeBe16(palmdocHdr, 0);
    writeBe16(palmdocHdr, 0);

    const std::uint32_t fullNameOffset = static_cast<std::uint32_t>(16 + 232 + exthHeader.size());
    const std::uint32_t flisRecNum = static_cast<std::uint32_t>(numTextRecords) + 1;
    const std::uint32_t fcisRecNum = static_cast<std::uint32_t>(numTextRecords) + 2;

    // MOBI Header (232 bytes)
    std::vector<std::uint8_t> mobiHdr(232, 0x00);
    mobiHdr[0] = 'M'; mobiHdr[1] = 'O'; mobiHdr[2] = 'B'; mobiHdr[3] = 'I';
    putBe32At(mobiHdr, 4, 232);
    putBe32At(mobiHdr, 8, 2);      // Mobipocket Book
    putBe32At(mobiHdr, 12, 65001); // UTF-8
    putBe32At(mobiHdr, 16, QRandomGenerator::global()->bounded(1u, 0x7FFFFFFFu));
    putBe32At(mobiHdr, 20, 6);     // MOBI version 6
    std::fill(mobiHdr.begin() + 24, mobiHdr.begin() + 64, 0xFF);
    putBe32At(mobiHdr, 64, flisRecNum);
    putBe32At(mobiHdr, 68, fullNameOffset);
    putBe32At(mobiHdr, 72, static_cast<std::uint32_t>(titleUtf8.size()));
    putBe32At(mobiHdr, 76, 0x0804); // zh-CN locale
    putBe32At(mobiHdr, 88, 6);
    putBe32At(mobiHdr, 92, flisRecNum);
    putBe32At(mobiHdr, 112, 0x40);  // EXTH flag
    putBe32At(mobiHdr, 148, 0xFFFFFFFFu);
    putBe32At(mobiHdr, 152, 0xFFFFFFFFu);
    putBe16At(mobiHdr, 176, 1);
    putBe16At(mobiHdr, 178, numTextRecords);
    putBe32At(mobiHdr, 180, 1);
    putBe32At(mobiHdr, 184, fcisRecNum);
    putBe32At(mobiHdr, 188, 1);
    putBe32At(mobiHdr, 192, flisRecNum);
    putBe32At(mobiHdr, 196, 1);
    putBe32At(mobiHdr, 208, 0xFFFFFFFFu);
    std::fill(mobiHdr.begin() + 216, mobiHdr.begin() + 224, 0xFF);
    putBe32At(mobiHdr, 224, 0);
    putBe32At(mobiHdr, 228, 0xFFFFFFFFu);

    const std::size_t titlePad = ((4 - (titleUtf8.size() % 4)) % 4) + 2;
    std::vector<std::uint8_t> rec0;
    rec0.insert(rec0.end(), palmdocHdr.begin(), palmdocHdr.end());
    rec0.insert(rec0.end(), mobiHdr.begin(), mobiHdr.end());
    rec0.insert(rec0.end(), exthHeader.begin(), exthHeader.end());
    rec0.insert(rec0.end(), titleUtf8.begin(), titleUtf8.end());
    rec0.insert(rec0.end(), titlePad, 0x00);

    const std::vector<std::uint8_t> flisRecord = {
        'F','L','I','S', 0,0,0,8, 0,0x41, 0,0, 0,0,0,0,
        0xFF,0xFF,0xFF,0xFF, 0,1, 0,3, 0,0,0,3, 0,0,0,1, 0xFF,0xFF,0xFF,0xFF
    };
    std::vector<std::uint8_t> fcisRecord = {
        'F','C','I','S', 0,0,0,0x14, 0,0,0,0x10, 0,0,0,1, 0,0,0,0
    };
    writeBe32(fcisRecord, textLength);
    const std::uint8_t fcisTail[] = {0,0,0,0, 0,0,0,0x20, 0,0,0,8, 0,1, 0,1, 0,0,0,0};
    fcisRecord.insert(fcisRecord.end(), fcisTail, fcisTail + sizeof(fcisTail));
    const std::vector<std::uint8_t> eofRecord = {0xE9, 0x8E, '\r', '\n'};

    std::vector<std::vector<std::uint8_t>> allRecords;
    allRecords.reserve(textRecords.size() + 4);
    allRecords.push_back(std::move(rec0));
    for (auto& tr : textRecords) allRecords.push_back(std::move(tr));
    allRecords.push_back(flisRecord);
    allRecords.push_back(fcisRecord);
    allRecords.push_back(eofRecord);

    const auto totalRecs = static_cast<std::uint16_t>(allRecords.size());

    // PDB Header (78 bytes)
    std::vector<std::uint8_t> pdb;
    pdb.reserve(8192);
    std::string asciiName;
    for (char c : titleUtf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            asciiName.push_back(c);
        } else {
            asciiName.push_back('_');
        }
        if (asciiName.size() == 31) break;
    }
    if (asciiName.empty()) asciiName = "Book";
    pdb.insert(pdb.end(), asciiName.begin(), asciiName.end());
    pdb.resize(32, 0x00);

    const auto nowTs = static_cast<std::uint32_t>(std::time(nullptr));
    writeBe16(pdb, 0);
    writeBe16(pdb, 0);
    writeBe32(pdb, nowTs);
    writeBe32(pdb, nowTs);
    writeBe32(pdb, 0);
    writeBe32(pdb, 0);
    writeBe32(pdb, 0);
    writeBe32(pdb, 0);
    pdb.insert(pdb.end(), {'B', 'O', 'O', 'K', 'M', 'O', 'B', 'I'});
    writeBe32(pdb, 0);
    writeBe32(pdb, 0);
    writeBe16(pdb, totalRecs);

    std::uint32_t offset = static_cast<std::uint32_t>(78 + totalRecs * 8 + 2);
    for (std::uint16_t idx = 0; idx < totalRecs; ++idx) {
        writeBe32(pdb, offset);
        pdb.push_back(0x00); // record attributes
        const std::uint32_t uid = static_cast<std::uint32_t>(idx) * 2u;
        pdb.push_back(static_cast<std::uint8_t>((uid >> 16) & 0xFF));
        pdb.push_back(static_cast<std::uint8_t>((uid >> 8) & 0xFF));
        pdb.push_back(static_cast<std::uint8_t>(uid & 0xFF));
        offset += static_cast<std::uint32_t>(allRecords[idx].size());
    }
    pdb.push_back(0x00);
    pdb.push_back(0x00);

    for (const auto& rec : allRecords) {
        pdb.insert(pdb.end(), rec.begin(), rec.end());
    }

    omni::infra::AtomicFileTx::writeBytesAtomic(outPath, pdb);
}

void NovelEngine::writeBookToMobi(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent) {
    if (const auto calibre = omni::infra::PortableEnv::resolveCalibreConvert()) {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            const auto tmpEpub = omni::core::qStringToPath(tmpDir.filePath(QStringLiteral("source.epub")));
            writeBookToEpub(doc, tmpEpub, autoIndent);
            omni::infra::AtomicFileTx tx(outPath);
            QProcess proc;
            proc.start(omni::core::pathToQString(*calibre),
                       {omni::core::pathToQString(tmpEpub), omni::core::pathToQString(tx.tempPath())});
            if (proc.waitForFinished(120000) && proc.exitCode() == 0) {
                tx.commit();
                return;
            }
        }
    }
    writeBookToMobiNative(doc, outPath, autoIndent);
}

void NovelEngine::writeBookToPdf(
    const BookDocument& doc,
    const std::filesystem::path& outPath,
    const std::string& pagePreset,
    bool autoIndent,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    std::lock_guard<std::mutex> lk(gPdfRenderMutex);
    omni::infra::AtomicFileTx tx(outPath);

    QPdfWriter pdfWriter(omni::core::pathToQString(tx.tempPath()));
    pdfWriter.setResolution(72); // 1 logical px == 1 PDF point (72 dpi)
    pdfWriter.setTitle(QString::fromUtf8(doc.title.c_str()));
    pdfWriter.setCreator(QStringLiteral("OmniConvert"));

    QPageSize pageSize(QPageSize::A4);
    double marginPt = 48.0;
    if (pagePreset == "eink6") {
        pageSize = QPageSize(QSizeF(288.0, 412.0), QPageSize::Point);
        marginPt = 22.0;
    } else if (pagePreset == "b5") {
        pageSize = QPageSize(QPageSize::JisB5);
        marginPt = 38.0;
    }
    pdfWriter.setPageSize(pageSize);
    pdfWriter.setPageMargins(QMarginsF(marginPt, marginPt, marginPt, marginPt), QPageLayout::Point);

    QPainter painter(&pdfWriter);
    if (!painter.isActive()) {
        throw std::runtime_error("初始化 PDF 渲染引擎失败");
    }

    const QRectF paintRect = painter.viewport();
    const QSizeF contentSize(paintRect.width(), paintRect.height());
    const std::string indentCss = autoIndent ? "23pt" : "0pt";
    const std::size_t totalChapters = std::max<std::size_t>(1, doc.chapters.size());
    bool firstPage = true;

    for (std::size_t idx = 0; idx < doc.chapters.size(); ++idx) {
        if (isCancelled && isCancelled()) {
            painter.end();
            tx.rollback();
            throw std::runtime_error("CANCELLED");
        }

        if (!firstPage) {
            pdfWriter.newPage();
        }
        firstPage = false;

        std::ostringstream chapHtml;
        chapHtml << "<html><head><style>"
                 << "body { font-family: 'PingFang SC', 'Songti SC', 'Microsoft YaHei', sans-serif; font-size: 11.5pt; line-height: 175%; color: #1c1e21; }"
                 << "h1 { font-size: 16pt; text-align: center; margin-bottom: 12pt; }"
                 << "h2 { font-size: 14pt; margin-top: 6pt; margin-bottom: 10pt; }"
                 << "p { text-indent: " << indentCss << "; margin-top: 0pt; margin-bottom: 6pt; }"
                 << "</style></head><body>";
        if (idx == 0) {
            chapHtml << "<h1>" << xmlEscape(doc.title) << "</h1>"
                     << "<p style=\"text-indent:0pt;text-align:center;color:#666666;\">作者："
                     << xmlEscape(doc.author.empty() ? "佚名" : doc.author) << "</p>";
        }
        chapHtml << "<h2>" << xmlEscape(doc.chapters[idx].title) << "</h2>";
        for (const auto& p : doc.chapters[idx].paragraphs) {
            chapHtml << "<p>" << xmlEscape(p) << "</p>";
        }
        chapHtml << "</body></html>";

        QTextDocument textDoc;
        textDoc.setDocumentMargin(0.0);
        textDoc.setPageSize(contentSize);
        textDoc.setHtml(QString::fromUtf8(chapHtml.str().c_str()));

        const int chapPageCount = std::max(1, textDoc.pageCount());
        for (int p = 0; p < chapPageCount; ++p) {
            if (p > 0) {
                pdfWriter.newPage();
            }
            const QRectF clipRect(0, p * contentSize.height(), contentSize.width(), contentSize.height());
            painter.save();
            painter.translate(0, -p * contentSize.height());
            textDoc.drawContents(&painter, clipRect);
            painter.restore();
        }

        if (progressCb) {
            const int pct = std::min(96, 20 + static_cast<int>((idx + 1) * 76 / totalChapters));
            progressCb(pct);
        }
    }

    // Embed lossless UTF-8 chapter text stream as a PDF document attachment so roundtrip PDF -> Novel
    // works with 100% fidelity across all platforms even without external PDF rendering libraries.
    {
        std::ostringstream plain;
        for (const auto& ch : doc.chapters) {
            plain << ch.title << "\n";
            for (const auto& p : ch.paragraphs) {
                plain << p << "\n";
            }
        }
        const std::string payload = "OMNICONVERT_UTF8_TEXT_BEGIN\n" + plain.str() + "\nOMNICONVERT_UTF8_TEXT_END";
        pdfWriter.addFileAttachment(
            QStringLiteral("omniconvert_text.txt"),
            QByteArray(payload.data(), static_cast<qsizetype>(payload.size())),
            QStringLiteral("text/plain;charset=utf-8"));
    }

    painter.end();
    tx.commit();
}

void NovelEngine::writeBookToDocx(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent) {
    omni::infra::ZipWriter zip;

    zip.addTextFile("[Content_Types].xml",
                    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
</Types>
)");

    zip.addTextFile("_rels/.rels",
                    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
</Relationships>
)");

    zip.addTextFile("word/_rels/document.xml.rels",
                    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>
)");

    const std::string escTitle = xmlEscape(doc.title);
    const std::string escAuthor = xmlEscape(doc.author.empty() ? "佚名" : doc.author);
    zip.addTextFile("docProps/core.xml",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                    "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                    "  <dc:title>" + escTitle + "</dc:title>\n"
                    "  <dc:creator>" + escAuthor + "</dc:creator>\n"
                    "</cp:coreProperties>\n");

    zip.addTextFile("word/styles.xml",
                    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:style w:type="paragraph" w:default="1" w:styleId="Normal">
    <w:name w:val="Normal"/>
    <w:rPr><w:sz w:val="24"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Title">
    <w:name w:val="Title"/>
    <w:pPr><w:jc w:val="center"/><w:spacing w:after="240"/></w:pPr>
    <w:rPr><w:b/><w:sz w:val="44"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading1">
    <w:name w:val="heading 1"/>
    <w:pPr><w:outlineLvl w:val="0"/><w:spacing w:before="280" w:after="160"/></w:pPr>
    <w:rPr><w:b/><w:sz w:val="32"/></w:rPr>
  </w:style>
</w:styles>
)");

    const std::string indentPrefix = autoIndent ? "　　" : "";
    std::ostringstream body;
    body << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)" << "\n"
         << R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>)" << "\n"
         << "<w:p><w:pPr><w:pStyle w:val=\"Title\"/></w:pPr><w:r><w:t>" << escTitle << "</w:t></w:r></w:p>\n"
         << "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:t>作者：" << escAuthor << "</w:t></w:r></w:p>\n";

    for (const auto& ch : doc.chapters) {
        body << "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>"
             << xmlEscape(ch.title) << "</w:t></w:r></w:p>\n";
        for (const auto& p : ch.paragraphs) {
            body << "<w:p><w:r><w:t xml:space=\"preserve\">"
                 << xmlEscape(indentPrefix + p) << "</w:t></w:r></w:p>\n";
        }
    }
    body << "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/></w:sectPr></w:body></w:document>\n";
    zip.addTextFile("word/document.xml", body.str());

    zip.writeToFile(outPath);
}

void NovelEngine::writeBookToHtml(const BookDocument& doc, const std::filesystem::path& outPath, bool autoIndent) {
    const std::string indentCss = autoIndent ? "2em" : "0";
    const std::string escTitle = xmlEscape(doc.title);
    const std::string escAuthor = xmlEscape(doc.author.empty() ? "佚名" : doc.author);

    std::ostringstream tocLinks;
    std::ostringstream chapBlocks;
    for (std::size_t idx = 0; idx < doc.chapters.size(); ++idx) {
        const std::string cid = "ch-" + std::to_string(idx + 1);
        const std::string escChTitle = xmlEscape(doc.chapters[idx].title);
        tocLinks << "<a href=\"#" << cid << "\" class=\"toc-item\">" << escChTitle << "</a>";
        chapBlocks << "<section id=\"" << cid << "\" class=\"chapter\"><h2>" << escChTitle << "</h2>";
        for (const auto& p : doc.chapters[idx].paragraphs) {
            chapBlocks << "<p>" << xmlEscape(p) << "</p>\n";
        }
        chapBlocks << "</section>";
    }

    std::ostringstream page;
    page << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
         << "<meta charset=\"utf-8\"/>\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
         << "<title>" << escTitle << "</title>\n<style>\n"
         << ":root { --bg:#f7f5f0; --card:#ffffff; --fg:#22252a; --muted:#6b7280; --border:#e5e2dc; --accent:#2563eb; --fs:18px; }\n"
         << "body.sepia { --bg:#f4ecd8; --card:#fbf5e6; --fg:#3b2f24; --muted:#7a6855; --border:#e3d5b8; --accent:#9a5b13; }\n"
         << "body.dark { --bg:#121417; --card:#1b1f24; --fg:#d9dee5; --muted:#8b949e; --border:#2d333b; --accent:#58a6ff; }\n"
         << "* { box-sizing: border-box; }\n"
         << "body { margin:0; background:var(--bg); color:var(--fg); font-family:\"PingFang SC\",\"Microsoft YaHei\",serif; display:flex; min-height:100vh; transition:background .2s,color .2s; }\n"
         << "aside { width:280px; background:var(--card); border-right:1px solid var(--border); height:100vh; position:sticky; top:0; overflow-y:auto; padding:18px 14px; flex-shrink:0; }\n"
         << "aside.hidden { display:none; }\n"
         << ".book-meta h1 { font-size:18px; margin:0 0 6px 0; }\n"
         << ".book-meta p { font-size:13px; color:var(--muted); margin:0 0 14px 0; }\n"
         << ".toc-item { display:block; padding:7px 10px; color:var(--fg); text-decoration:none; border-radius:6px; font-size:14px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }\n"
         << ".toc-item:hover { background:rgba(128,128,128,0.12); color:var(--accent); }\n"
         << "main { flex:1; padding:24px 20px 80px; max-width:860px; margin:0 auto; }\n"
         << ".toolbar { position:sticky; top:12px; background:var(--card); border:1px solid var(--border); border-radius:10px; padding:8px 14px; display:flex; gap:8px; align-items:center; margin-bottom:28px; box-shadow:0 4px 14px rgba(0,0,0,.05); flex-wrap:wrap; }\n"
         << ".toolbar button { border:1px solid var(--border); background:var(--bg); color:var(--fg); border-radius:6px; padding:5px 11px; cursor:pointer; font-size:13px; }\n"
         << ".chapter { background:var(--card); border:1px solid var(--border); border-radius:12px; padding:32px 36px; margin-bottom:24px; }\n"
         << ".chapter h2 { margin-top:0; padding-bottom:12px; border-bottom:1px solid var(--border); font-size:calc(var(--fs) * 1.3); }\n"
         << ".chapter p { font-size:var(--fs); line-height:1.9; text-indent:" << indentCss << "; margin:0.65em 0; text-align:justify; }\n"
         << "@media (max-width: 768px) { aside { position:fixed; z-index:20; } .chapter { padding:20px 18px; } }\n"
         << "</style>\n</head>\n<body class=\"sepia\">\n"
         << "<aside id=\"sidebar\">\n  <div class=\"book-meta\">\n"
         << "    <h1>" << escTitle << "</h1>\n"
         << "    <p>作者：" << escAuthor << " · 共 " << doc.chapters.size() << " 章</p>\n"
         << "  </div>\n  " << tocLinks.str() << "\n</aside>\n"
         << "<main>\n  <div class=\"toolbar\">\n"
         << "    <button onclick=\"document.getElementById('sidebar').classList.toggle('hidden')\">☰ 目录开关</button>\n"
         << "    <button onclick=\"document.body.className=''\">纯净白</button>\n"
         << "    <button onclick=\"document.body.className='sepia'\">护眼羊皮纸</button>\n"
         << "    <button onclick=\"document.body.className='dark'\">夜间深色</button>\n"
         << "    <button onclick=\"chgFs(-2)\">A-</button>\n"
         << "    <button onclick=\"chgFs(2)\">A+</button>\n"
         << "  </div>\n  " << chapBlocks.str() << "\n</main>\n"
         << "<script>\nlet fs = 18;\nfunction chgFs(d) { fs = Math.min(30, Math.max(13, fs + d)); document.documentElement.style.setProperty('--fs', fs + 'px'); }\n</script>\n"
         << "</body>\n</html>";

    omni::infra::AtomicFileTx::writeStringAtomic(outPath, page.str());
}

void NovelEngine::writeBookToMd(const BookDocument& doc, const std::filesystem::path& outPath) {
    std::ostringstream ss;
    ss << "# " << doc.title << "\n\n> 作者：" << (doc.author.empty() ? "佚名" : doc.author) << "\n\n";
    for (const auto& ch : doc.chapters) {
        ss << "## " << ch.title << "\n\n";
        for (const auto& p : ch.paragraphs) {
            ss << p << "\n\n";
        }
    }
    omni::infra::AtomicFileTx::writeStringAtomic(outPath, ss.str());
}

void NovelEngine::writeBookToFb2(const BookDocument& doc, const std::filesystem::path& outPath) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
       << "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">\n"
       << "  <description>\n"
       << "    <title-info>\n"
       << "      <genre>prose</genre>\n"
       << "      <author><nickname>" << xmlEscape(doc.author.empty() ? "佚名" : doc.author) << "</nickname></author>\n"
       << "      <book-title>" << xmlEscape(doc.title) << "</book-title>\n"
       << "      <lang>zh</lang>\n"
       << "    </title-info>\n"
       << "  </description>\n"
       << "  <body>\n";
    for (const auto& ch : doc.chapters) {
        ss << "    <section>\n      <title><p>" << xmlEscape(ch.title) << "</p></title>\n";
        for (const auto& p : ch.paragraphs) {
            ss << "      <p>" << xmlEscape(p) << "</p>\n";
        }
        ss << "    </section>\n";
    }
    ss << "  </body>\n</FictionBook>\n";
    omni::infra::AtomicFileTx::writeStringAtomic(outPath, ss.str());
}

void NovelEngine::convertNovel(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    bool autoIndent,
    const std::string& customAuthor,
    const std::string& txtEncoding,
    const std::string& pdfPreset,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(10);

    BookDocument doc = parseNovelToDocument(inPath);
    const QString qCustom = QString::fromUtf8(customAuthor.c_str()).trimmed();
    if (!qCustom.isEmpty()) {
        doc.author = qCustom.toUtf8().toStdString();
    }

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(45);

    std::string fmt = targetFormat;
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (fmt == "epub") {
        writeBookToEpub(doc, outPath, autoIndent);
    } else if (fmt == "txt") {
        writeBookToTxt(doc, outPath, txtEncoding, autoIndent);
    } else if (fmt == "mobi") {
        writeBookToMobi(doc, outPath, autoIndent);
    } else if (fmt == "pdf") {
        writeBookToPdf(doc, outPath, pdfPreset, autoIndent, progressCb, isCancelled);
    } else if (fmt == "docx") {
        writeBookToDocx(doc, outPath, autoIndent);
    } else if (fmt == "html") {
        writeBookToHtml(doc, outPath, autoIndent);
    } else if (fmt == "md") {
        writeBookToMd(doc, outPath);
    } else if (fmt == "fb2") {
        writeBookToFb2(doc, outPath);
    } else {
        throw std::runtime_error("不支持的小说输出格式: " + targetFormat);
    }

    if (progressCb) progressCb(100);
}

} // namespace omni::engines
