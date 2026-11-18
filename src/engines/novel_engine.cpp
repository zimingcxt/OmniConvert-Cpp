#include "omni/engines/novel_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/portable_env.hpp"
#include "omni/infra/text_encoding.hpp"
#include "omni/infra/zip_archive.hpp"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QImageWriter>
#include <QLinearGradient>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPen>
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace omni::engines {

namespace {

std::mutex gPdfRenderMutex;

const QRegularExpression& defaultChapterTitleRegex() {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^\s*(?:正文\s*)?(?:第[零一二三四五六七八九十百千万两〇0-9]+[章节回卷集部篇幕]|卷[零一二三四五六七八九十百千万两〇0-9]+|楔子|序章|序言|引子|前言|后记|尾声|番外|终章|Chapter\s+\d+|CHAPTER\s+\d+|Part\s+\d+|Book\s+\d+)(?:[\s：:、.\-—_].*)?$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

bool matchesChapterHeading(const QString& stripped, const QRegularExpression* customRe) {
    if (stripped.isEmpty() || stripped.size() > 75 || stripped.contains('\n')) return false;
    if (customRe && customRe->isValid() && !customRe->pattern().isEmpty()) {
        if (customRe->match(stripped).hasMatch()) return true;
    }
    return defaultChapterTitleRegex().match(stripped).hasMatch();
}

bool isWebNovelAdJunkLine(const QString& stripped) {
    if (stripped.isEmpty() || stripped.size() > 110) return false;
    static const QRegularExpression adRe(
        QStringLiteral(
            R"((?:记住本站域名|最新章节请访问|一秒记住[\s\S]{0,20}(?:更新|阅读)|天才一秒记住|手机用户请浏览|本书首发来自|笔趣阁|请大家收藏[：:]?\s*(?:www\.|http)|(?:www\.|https?://)[a-zA-Z0-9\-]+\.[a-zA-Z]{2,}|求月票[！!。，\s]*$|求推荐票[！!。，\s]*$|未完待续.*求.*票))"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return adRe.match(stripped).hasMatch();
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
    QString rebuilt;
    qsizetype lastPos = 0;
    while (itDec.hasNext()) {
        const auto m = itDec.next();
        rebuilt += text.mid(lastPos, m.capturedStart() - lastPos);
        const char32_t code = static_cast<char32_t>(m.captured(1).toUInt());
        if (code > 0 && code <= 0x10FFFF) {
            rebuilt += QString::fromUcs4(&code, 1);
        }
        lastPos = m.capturedEnd();
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

std::size_t trimMobiRecordLength(const std::uint8_t* rec, std::size_t len, std::uint32_t extraFlags) {
    if (len == 0 || extraFlags == 0) return len;
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
    if ((extraFlags & 1u) != 0 && len > 0) {
        const std::size_t extra = (rec[len - 1] & 0x03u) + 1u;
        if (extra <= len) len -= extra;
    }
    return len;
}

BookDocument parseMobiNative(const std::filesystem::path& inPath, const std::string& customRegex, bool cleanAds) {
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
    std::uint32_t firstImgIdx = 0xFFFFFFFFu;
    std::uint32_t coverRelOffset = 0xFFFFFFFFu;

    if (rec0Len >= 36 && std::memcmp(rec0 + 16, "MOBI", 4) == 0) {
        const std::uint32_t mobiHdrLen = readBe32(rec0 + 20);
        const std::uint32_t fullNameOff = (rec0Len >= 88) ? readBe32(rec0 + 84) : 0;
        const std::uint32_t fullNameLen = (rec0Len >= 92) ? readBe32(rec0 + 88) : 0;
        if (rec0Len >= 112) {
            firstImgIdx = readBe32(rec0 + 108); // 16 + 92
        }
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
                    if (recType == 201 && recLen == 12) {
                        coverRelOffset = readBe32(rec0 + cur + 8);
                    } else {
                        std::vector<std::uint8_t> val(rec0 + cur + 8, rec0 + cur + recLen);
                        const QString qVal = QString::fromUtf8(omni::infra::TextEncoding::decodeSmartToUtf8(val).c_str()).trimmed();
                        if (!qVal.isEmpty()) {
                            if (recType == 100) doc.author = qVal.toUtf8().toStdString();
                            if (recType == 503) doc.title = qVal.toUtf8().toStdString();
                        }
                    }
                    cur += recLen;
                }
            }
        }
    }

    // Extract embedded cover image if present
    if (firstImgIdx != 0xFFFFFFFFu && coverRelOffset != 0xFFFFFFFFu) {
        const std::size_t imgRec = static_cast<std::size_t>(firstImgIdx) + static_cast<std::size_t>(coverRelOffset);
        if (imgRec > numTextRecs && imgRec < recRanges.size() && recRanges[imgRec].second > 32) {
            QImage cov;
            if (cov.loadFromData(&raw[recRanges[imgRec].first], static_cast<int>(recRanges[imgRec].second))) {
                doc.coverJpegBytes = NovelEngine::prepareCoverJpeg(cov);
            }
        }
    }

    if (compression != 1 && compression != 2) {
        if (const auto cal = omni::infra::PortableEnv::resolveCalibreConvert()) {
            QTemporaryDir tmpDir;
            if (tmpDir.isValid()) {
                const QString tmpEpub = tmpDir.filePath(QStringLiteral("unpacked.epub"));
                QProcess proc;
                proc.start(omni::core::pathToQString(*cal), {omni::core::pathToQString(inPath), tmpEpub});
                if (proc.waitForFinished(60000) && proc.exitCode() == 0) {
                    return NovelEngine::parseNovelToDocument(omni::core::qStringToPath(tmpEpub), customRegex, cleanAds);
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
    doc.chapters = NovelEngine::extractChaptersFromHtml(htmlUtf8, "正文", customRegex, cleanAds);
    return doc;
}

BookDocument parseEpubFile(const std::filesystem::path& inPath, const std::string& customRegex, bool cleanAds) {
    omni::infra::ZipReader zip;
    std::string err;
    if (!zip.open(inPath, &err)) {
        throw std::runtime_error("读取 EPUB 失败: " + err);
    }

    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

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
    std::string coverEntryPath;

    if (!opfPath.empty()) {
        if (const auto opfXml = zip.readEntryUtf8(opfPath)) {
            std::unordered_map<std::string, std::pair<std::string, std::string>> manifest;
            std::vector<std::string> spineIds;
            std::string metaCoverId;

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
                } else if (local == QStringLiteral("meta")) {
                    if (xml.attributes().value(QStringLiteral("name")).toString() == QStringLiteral("cover")) {
                        metaCoverId = xml.attributes().value(QStringLiteral("content")).toString().toUtf8().toStdString();
                    }
                } else if (local == QStringLiteral("item")) {
                    const QString id = xml.attributes().value(QStringLiteral("id")).toString();
                    QString href = xml.attributes().value(QStringLiteral("href")).toString();
                    const QString mt = xml.attributes().value(QStringLiteral("media-type")).toString();
                    const QString props = xml.attributes().value(QStringLiteral("properties")).toString();
                    if (!id.isEmpty() && !href.isEmpty()) {
                        const int hashIdx = href.indexOf('#');
                        if (hashIdx >= 0) href = href.left(hashIdx);
                        href = QUrl::fromPercentEncoding(href.toUtf8());
                        std::string fullHref = opfDir + href.toUtf8().toStdString();
                        manifest[id.toUtf8().toStdString()] = {fullHref, mt.toUtf8().toStdString()};
                        if (props.contains(QStringLiteral("cover-image")) && mt.startsWith(QStringLiteral("image/"))) {
                            coverEntryPath = fullHref;
                        }
                    }
                } else if (local == QStringLiteral("itemref")) {
                    const QString idref = xml.attributes().value(QStringLiteral("idref")).toString();
                    if (!idref.isEmpty()) spineIds.push_back(idref.toUtf8().toStdString());
                }
            }

            if (coverEntryPath.empty() && !metaCoverId.empty()) {
                auto itCov = manifest.find(metaCoverId);
                if (itCov != manifest.end() && itCov->second.second.find("image/") == 0) {
                    coverEntryPath = itCov->second.first;
                }
            }

            auto isDocMedia = [](const std::string& mt, const std::string& path) {
                if (mt.find("xhtml") != std::string::npos || mt.find("html") != std::string::npos) {
                    const QString qp = QString::fromUtf8(path.c_str()).toLower();
                    return !qp.endsWith(QStringLiteral("nav.xhtml")) &&
                           !qp.contains(QStringLiteral("/nav.")) &&
                           !qp.endsWith(QStringLiteral("cover.xhtml"));
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

    if (!coverEntryPath.empty()) {
        if (const auto covBytes = zip.readEntry(coverEntryPath)) {
            QImage covImg;
            if (covImg.loadFromData(covBytes->data(), static_cast<int>(covBytes->size()))) {
                doc.coverJpegBytes = NovelEngine::prepareCoverJpeg(covImg);
            }
        }
    }

    if (orderedHtmlEntries.empty()) {
        for (const auto& name : zip.entryNames()) {
            const QString qn = QString::fromUtf8(name.c_str()).toLower();
            if ((qn.endsWith(".xhtml") || qn.endsWith(".html") || qn.endsWith(".htm")) &&
                !qn.endsWith("nav.xhtml") && !qn.endsWith("cover.xhtml")) {
                orderedHtmlEntries.push_back(name);
            }
        }
    }

    int idx = 1;
    for (const auto& entryName : orderedHtmlEntries) {
        if (const auto bytes = zip.readEntry(entryName)) {
            const std::string htmlStr = omni::infra::TextEncoding::decodeSmartToUtf8(*bytes);
            auto extracted = NovelEngine::extractChaptersFromHtml(
                htmlStr, "第 " + std::to_string(idx) + " 章", customRegex, cleanAds);
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

BookDocument parseDocxFile(const std::filesystem::path& inPath, const std::string& customRegex, bool cleanAds) {
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

    QRegularExpression userRe;
    const QRegularExpression* userRePtr = nullptr;
    if (!customRegex.empty()) {
        userRe = QRegularExpression(QString::fromUtf8(customRegex.c_str()), QRegularExpression::UseUnicodePropertiesOption);
        if (userRe.isValid()) userRePtr = &userRe;
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
                if (paraIsHeading || matchesChapterHeading(txt, userRePtr)) {
                    if (!curParas.empty()) {
                        doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
                        curParas.clear();
                    }
                    curTitle = txt.simplified().toUtf8().toStdString();
                } else {
                    for (const QString& sub : txt.split('\n')) {
                        const QString norm = normalizeParagraphQ(sub);
                        if (!norm.isEmpty() && (!cleanAds || !isWebNovelAdJunkLine(norm))) {
                            curParas.push_back(norm.toUtf8().toStdString());
                        }
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

BookDocument parseFb2File(const std::filesystem::path& inPath, bool cleanAds) {
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
                    } else if (!cleanAds || !isWebNovelAdJunkLine(pTxt)) {
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
        doc.chapters = NovelEngine::extractChaptersFromHtml(xmlStr, "正文", "", cleanAds);
    }
    return doc;
}

BookDocument parseMdFile(const std::filesystem::path& inPath, const std::string& customRegex, bool cleanAds) {
    const std::string raw = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    static const QRegularExpression h1Re(QStringLiteral(R"(^#\s+(.+)$)"));
    static const QRegularExpression h2Re(QStringLiteral(R"(^#{2,3}\s+(.+)$)"));

    QRegularExpression userRe;
    const QRegularExpression* userRePtr = nullptr;
    if (!customRegex.empty()) {
        userRe = QRegularExpression(QString::fromUtf8(customRegex.c_str()), QRegularExpression::UseUnicodePropertiesOption);
        if (userRe.isValid()) userRePtr = &userRe;
    }

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
        if (m1.hasMatch() || m2.hasMatch() || matchesChapterHeading(s, userRePtr)) {
            const QString heading = m1.hasMatch() ? m1.captured(1).trimmed()
                                                  : (m2.hasMatch() ? m2.captured(1).trimmed() : s);
            if (!curParas.empty()) {
                doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
                curParas.clear();
            }
            curTitle = heading.toUtf8().toStdString();
        } else {
            const QString norm = normalizeParagraphQ(s);
            if (!norm.isEmpty() && (!cleanAds || !isWebNovelAdJunkLine(norm))) {
                curParas.push_back(norm.toUtf8().toStdString());
            }
        }
    }

    if (!curParas.empty()) {
        doc.chapters.push_back(Chapter{curTitle, std::move(curParas)});
    }
    if (doc.chapters.empty()) {
        doc.chapters = NovelEngine::splitTextIntoChapters(raw, true, customRegex, cleanAds);
    }
    return doc;
}

// Pure C++ fallback PDF FlateDecode & ToUnicode CMap content-stream text extractor (used on Windows when Qt6Pdf is absent)
std::string extractPdfStreamsFallback(const std::filesystem::path& pdfPath) {
    QFile f(omni::core::pathToQString(pdfPath));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    const std::string bin(raw.constData(), static_cast<std::size_t>(raw.size()));

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

BookDocument parsePdfFile(const std::filesystem::path& inPath, const std::string& customRegex, bool cleanAds) {
    BookDocument doc;
    doc.title = cleanBookTitleFromStem(inPath);
    doc.author = "佚名";

    // Prefer lossless embeddedOmniConvert text stream if present
    const std::string streamTxt = extractPdfStreamsFallback(inPath);
    if (!streamTxt.empty() && streamTxt.size() > 24) {
        auto chs = NovelEngine::splitTextIntoChapters(streamTxt, true, customRegex, cleanAds);
        if (!chs.empty() && !(chs.size() == 1 && chs[0].paragraphs.size() == 1 && chs[0].paragraphs[0] == "（空白文档）")) {
            doc.chapters = std::move(chs);
            return doc;
        }
    }

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
            doc.chapters = NovelEngine::splitTextIntoChapters(pages.join('\n').toUtf8().toStdString(), true, customRegex, cleanAds);
            if (!doc.chapters.empty()) return doc;
        }
    }
#endif

#ifdef __APPLE__
    if (macos_pdf::extractPdfDocument(inPath, doc)) {
        return doc;
    }
#endif

    if (const auto cal = omni::infra::PortableEnv::resolveCalibreConvert()) {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            const QString tmpTxt = tmpDir.filePath(QStringLiteral("pdf_extracted.txt"));
            QProcess proc;
            proc.start(omni::core::pathToQString(*cal), {omni::core::pathToQString(inPath), tmpTxt});
            if (proc.waitForFinished(60000) && proc.exitCode() == 0) {
                const std::string txt = omni::infra::TextEncoding::readFileSmartUtf8(omni::core::qStringToPath(tmpTxt));
                doc.chapters = NovelEngine::splitTextIntoChapters(txt, true, customRegex, cleanAds);
                return doc;
            }
        }
    }

    if (!streamTxt.empty()) {
        doc.chapters = NovelEngine::splitTextIntoChapters(streamTxt, true, customRegex, cleanAds);
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

// High-coverage Chinese Simplified <-> Traditional literary character mapping
struct ZhCharTables {
    std::unordered_map<char32_t, char32_t> s2t;
    std::unordered_map<char32_t, char32_t> t2s;
};

const ZhCharTables& zhTables() {
    static const ZhCharTables tables = []() {
        // Aligned UTF-8 character strings (each code point in S corresponds 1-to-1 to T)
        const QString simp = QStringLiteral(
            "万与专业丛东丝丢两严丧个丰临为丽举么义乌乐乔习乡书买乱争于亏云亘亚产亩亲亵亿仅从仑仓仪们价众优伙会伛伞伟传伤伥伦伧伪伫体余佣佥侠侣侥侦侧侨侩侪侬俣俦俨俩俪俭债倾偬偻偾偿傥傧储傩儿兑兖党兰关兴兹养兽冁内冈册再冒冕冗写军农冢冲决况冻净凄凉凌减凑凛几凤凫凭凯击凼凿刍划刘则刚创删别刬刭刽刿剀剂剐剑剥剧劝办务劢动励劲劳势勋勐勚匀匦匮区医华协单卖卢卤卧卫却卺厂厅历厉压厌厍厕厢厣厦厨厩厮县参叆叇双发变叙叠叶号叹叽吁后吓吕吗吣吨听启吴呒呓呕呖呗员呙呛呜咏咔咙咛咝咤咴咸哌响哑哒哓哔哕哗哙哜哝哟唛唝唠唡唢唣唤唿啧啬啭啮啰啴啸喷喽喾嗫呵嗳嘘嘤嘱噜噼嚣嚯团园囱围囵国图圆圣圹场坂坏块坚坛坜坝坞坟坠垄垅垆垒垦垧垩垫垭垯垱垲垴埘埙埚埝埯堑堕塆墙壮声壳壶壸处备复够头夸夹夺奁奂奋奖奥妆妇妈妩妪妫姗姜娄娅娆娇娈娱娲娴婳婴婵婶媪嫒嫔嫱嬷孙学孪宁宝实宠审宪宫宽宾寝对寻导寿将尔尘尝尧尴尸尽层屃屉届属屡屦屿岁岂岖岗岘岙岚岛岩岭岳岽岿峃峄峡峣峤峥峦崂崃崄崭嵘嵚嵛嵝嵴巅巩巯币帅师帏帐帘帜带帧帮帱帻帼幂幞干并广庄庆庐庑库应庙庞废庼廪开异弃弑巩张弥弪弯弹强归当录彦彻径徕御忆忏忧忾怀态怂怃怄怅怆怜总怼怿恋恳恶恸恹恺恻恼恽悦悫悬悭悯惊惧惨惩惫惬惭惮惯惰恼想惶惹惺愦慑慊愿慑慈慌慎慑慕慢慧慨慰慵慷憋憎憔憝憧憨憩憬懔懊懋懑懒戏戋戬战戬户扎扑扦执扩扪扫扬扰抚抛抟抠抡抢护报担拟拢拣拥拦拧拨择挂挚挛挜挝挞挟挠挡挢挣挤挥挦捞损捡换捣据捻掳掴掷掸掺掼揸揽揿搀搁搂搅携摄摅摆摇摈摊撄撑撵撷撸撺擞攒敌敛数斋斓斗斩断无旧时旷旸昙昼昽显晋晒晓晔晕晖暂暧札术朴机杀杂权条来杨杩杰极构枞枢枣枥枧枨枪枫枭柜柠柽栀栅标栈栉栊栋栌栎栏树栖样栾桊桠桡桢档桤桥桦桧桨桩梦梼梾检棂椁椟椠椤椭楼榄榇榈榉榍槛槟槠横樯樱橥橱橹橼檐檩欢欤欧歼殁殇残殒殓殚殡殴毁毂毕毙毡毵氇气氢氩氲汇汉污汤汹沓沟没沣沤沥沦沧沨沩沪泞泪泶泷泸泺泻泼泽泾洁洒洼浃浅浆浇浈浉浊测浍济浏浐浑浒浓浔浕涂涌涛涝涞涟涠涡涢涣涤润涧涨涩涮涯液涵涸涿淀淄淅淆淇淋淌淑淖淘淙泪淡淤淦淫淬淮深淳混淹添淼清渊渍渎渐渑渔渗渚渝渠渡渣渤渥温渫渭港渴游渺湃湄湍湎湓湔湖湘湛湟湜湮湾湿溃溅溆溉溏源溘溜溟溢溥溧溪溯溱溲溴溶溷溺溠溽滁滂滇滋滢滑滓滔滕滗滚滞滟滠满滢滤滥滦滨滩滪漓漠溽谰漪漫渍漱涨漳漶漾潆潇潋潍潘潜潞潟潢潦潭潮潲潴潸潺潼澄澈澉澌澍澎澜澡澧澳澶澹激濂濉濑濒濞濠濡涛滥濯潍灯灰灵灶灸灿炀炉炖炜炝点炼炽烁烂烃烛烟烦烧烨烩烫烬热焕焖焘煅煳熘爱爷牍牦牵牺犊犟状犷犸犹狈狍狝狞独狭狮狯狰狱狲猃猎猕猡猪猫猬献獭玑玙玚玛玮环现玱玺珉珏珐珑珰珲琎琏琐琼瑶瑷璇璎瓒瓮电画畅畲畴疖疗疟疠疡疬疮疯疱疴痈痉痒痖痨痪痫痴瘅瘆瘗瘘瘪瘫瘾瘿癞癣癫皑皱皲盏盐监盖盗盘眍眦眬着睁睐睑瞒瞩矫矶矾矿砀码砖砗砚砜砺砻砾础硁硅硕硖硗硙硚确硷碍碛碜碱碹礼祎祢祯祷祸禀禄禅离秃秆种积称秽秾稆税稣稳穑穷窃窍窎窑窜窝窥窦窶竖竞笃笋笔笕笺笼笾筑筚筛筜筝筹签简箓箦箧箨箩箪箫篑篓篮篱簖籁籴类籼粜粝粤粪粮糁糇紧絷纠纡红纣纤纥约级纨纩纪纫纬纭纮纯纰纱纲纳纴纵纶纷纸纹纺纻纼纽纾线绀绁绂练组绅细织终绉绊绂绌绍绎经绐绑绒结绔绕绖绗绘给绚绛络绝绞统绠绡绢绣绤绥绦继绨绩绪绫绬续绮绯绰绱绲绳维绵绶绷绸绹绺绻综绽绾绿缀缁缂缃缄缅缆缇缈缉缊缋缌缍缎缏缐缑缒缓缔缕编缗缘缙缚缛缜缝缞缟缠缡缢缣缤缥缦缧缨缩缪缫缬缭缮缯缰缱缲缳缴缵罂网罗罚罢罴羁羟羡翘翙翚耢耧耸耻聂聋职聍联聩聪肃肠肤肮肯育肴肵脖胀胁胆胜胧胨胪胫胶脉脍脏脐脑脓脔脚脱脶脸腊腌腘腭腻腼腽腾膑臜舆舣舰舱舻艰艳艹艺节芈芗芜芦苁苇苈苋苌苍苎苏苘苹茎茏茑茔茕茧荆荐荙荚荛荜荞荟荠荡荣荤荥荦荧荨荩荪荫荬荭荮药莅莜莱莲莳莴莶获莸莹莺莼萚萝萤营萦萧萨葱蒇蒉蒋蒌蓝蓟蓠蓣蓥蓦蔷蔹蔺蔼蕲蕴薮藁藓虏虑虚虫虬虮虽虾虿蚀蚁蚂蚕蚝蚬蛊蛎蛏蛮蛰蛱蛲蛳蛴蜕蜗蜡蝇蝈蝉蝎蝼蝾蠼衅衔补衬衮袄袅袆袜袭袯装裆裈裢裣裤裥褛褴襁襕见观觃规觅视觇览觉觊觋觌觍觎觏觐觑觞触觯誉誊计订讣认讥讦讧讨让讪讫训议讯记讱讲讳讴讵讶讷许讹论讻讼讽设访诀证诂诃评诅识诇诈诉诊诋诌词诎诏诐译诒诓诔试诖诗诘诙诚诛诜话诞诟诠诡询诣诤该详诧诨诩诪诫诬语诮误诰诱诲诳说诵诶请诸诹诺读诼诽课诿谀谁谂调谄谅谆谇谈谊谋谌谍谎谏谐谑谒谓谔谕谖谗谘谙谚谛谜谝谞谟谠谡谢谣谤谥谦谧谨谩谪谫谬谭谮谯谰谱谲谳谴谵谶谷豮贝贞负贠贡财责贤败账货质贩贪贫贬购贮贯贰贱贲贳贴贵贶贷贸费贺贻贼贽贾贿赀赁赂赃资赅赆赇赈赉赊赋赌赍赎赏赐赑赒赓赔赕赖赗赘赙赚赛赜赝赞赟赠赡赢赣赪赵赶趋趱趸跃跄跖跞践跶跷跸跹跻踊踌踪踬踯蹑蹒蹰蹿躏躜躯车轧轨轩轪轫转轭轮软轰轱轲轳轴轵轶轷轸轹轺轻轼载轾轿辀辁辂较辄辅辆辇辈辉辊辋辌辍辎辏辐辑辒输辔辕辖辗辘辙辚辞辟辩辫边辽达迁迂迄迅过迈迎运近返还这进远违连迟迩迳迹适选逊递逦逻遗遥邓邝邬邮邹邺邻郁郄郏郐郑郓郦郧郸酝酦酱酽酾酿释里鉅鉴銮錾钆钇针钉钊钋钌钍钎钏钐钑钒钓钔钕钖钗钘钙钚钛钜钝钞钟钠钡钢钣钤钥钦钧钨钩钪钫钬钭钮钯钰钱钲钳钴钵钶钷钸钹钺钻钼钽钾钿铀铁铂铃铄铅铆铈铉铊铋铍铎铏铐铑铒铕铗铘铙铚铛铜铝铞铟铠铡铢铣铤铥铦铧铨铪铫铬铭铮铯铰铱铲铳铴铵银铷铸铹铺铻铼铽链铿销锁锂锃锄锅锆锇锈锉锊锋锌锍锎锏锐锑锒锓锔锕锖锗错锚锜锠锡锢锣锤锥锦锨锩锫锬锭键锯锰锱锲锳锴锵锶锷锸锹锺锻锼锽锾锿镀镁镂镃镆镇镈镉镊镌镍镎镏镐镑镒镕镖镗镙镚镛镜镝镞镟镡镣镤镥镦镧镨镩镪镫镬镭镮镯镰镱镲镳镴镶长门闩闪闫闬闭问闯闰闱闲闳间闵闶闷闸闹闺闻闼闽闾闿阀阁阂阃阄阅阆阇阈阉阊阋阌阍阎阏阐阑阒阓阔阕阖阗阘阙阚阛队阳阴阵阶际陆陇陈陉陕陧陨险随隐隶隽难雏雠雳雾霁霉霭靓静靥鞑鞒鞯鞴韦韧韂韩韪韫韬韵页顶顷顸项顺须顼顽顾顿颀颁颂颃预颅领颇颈颉颊颋颌颍颎颏颐频颒颓颔颕颖颗题颙颚颛颜额颞颟颠颡颢颣颤颥颦颧风飏飐飑飒飓飔飕飖飗飘飙飚飞飨餍饤饥饦饧饨饩饪饫饬饭饮饯饰饱饲饳饴饵饶饷饸饹饺饻饼饽饾饿馀馁馂馃馄馅馆馇馈馉馊馋馌馍馎馏馐馑馒馓馔馕马驭驮驯驰驱驲驳驴驵驶驷驸驹驺驻驼驽驾驿骀骁骂骃骄骅骆骇骈骉骊骋验骍骎骏骐骑骒骓骔骕骖骗骘骙骚骛骜骝骞骟骠骡骢骣骤骥骦骧髅髋髌鬓魇魉鱼鱽鱾鱿鲀鲁鲂鲄鲅鲆鲇鲈鲉鲊鲋鲌鲍鲎鲏鲐鲑鲒鲓鲔鲕鲖鲗鲘鲙鲚鲛鲜鲝鲞鲟鲠鲡鲢鲣鲤鲥鲦鲧鲨鲩鲪鲫鲬鲭鲮鲯鲰鲱鲲鲳鲴鲵鲶鲷鲸鲹鲺鲻鲼鲽鲾鲿鳀鳁鳂鳃鳄鳅鳆鳇鳈鳉鳊鳋鳌鳍鳎鳏鳐鳑鳒鳓鳔鳕鳖鳗鳘鳙鳛鳜鳝鳞鳟鳠鳡鳢鳣鸟鸠鸡鸢鸣鸤鸥鸦鸧鸨鸩鸪鸫鸬鸭鸮鸯鸰鸱鸲鸳鸴鸵鸶鸷鸸鸹鸺鸻鸼鸽鸾鸿鹀鹁鹂鹃鹄鹅鹆鹇鹈鹉鹊鹋鹌鹍鹎鹏鹐鹑鹕鹗鹘鹚鹛鹜 १८鹞鹟鹠鹡鹢鹣鹤鹥鹦鹧鹨鹩鹪鹫鹬鹭鹯鹰鹱鹲鹳鹴卤咸鹾碱盐麦麸黄黉黩黪黾鼋鼍齐齑齿龀龃龄龅龆龇龈龉龊龋龌龙龚龛龟");

        const QString trad = QStringLiteral(
            "萬與專業叢東絲丟兩嚴喪個豐臨為麗舉麼義烏樂喬習鄉書買亂爭於虧雲亙亞產畝親褻億僅從侖倉儀們價眾優夥會傴傘偉傳傷倀倫傖偽佇體餘傭僉俠侶僥偵側僑儈儕儂俁儔儼倆儷儉債傾傯僂僨償儻儐儲儺兒兌兗黨蘭關興茲養獸囅內岡冊再冒冕冗寫軍農塚沖決況凍淨淒涼淩減湊凜幾鳳鳧憑凱擊氹鑿芻劃劉則剛創刪別剗剠劊劌剴劑剮劍剝劇勸辦務勱動勵勁勞勢勳猛勩勻匭匱區醫華協單賣盧鹵臥衛卻巹廠廳曆厲壓厭厙廁廂厴廈廚廄廝縣參靉靆雙發變敘疊葉號歎嘰籲後嚇呂嗎唚噸聽啟吳嘸囈嘔嚦唄員咼嗆嗚詠哢嚨嚀噝吒噅鹹呱響啞噠嘵嗶噦嘩噲嚌噥喲嘜嗊嘮啢嗩唕喚呼嘖嗇囀齧囉嘽嘯噴嘍嚳囁嗬噯噓嚶囑嚕劈囂謔團園囪圍圇國圖圓聖壙場阪壞塊堅壇壢壩塢墳墜壟壟壚壘墾坰堊墊埡墶壋塏堖塒塤堝墊垵塹墮灣牆壯聲殼壺壼處備複夠頭誇夾奪奩奐奮獎奧妝婦媽嫵嫗媯姍薑婁婭嬈嬌孌娛媧嫻嫿嬰嬋嬸媼嬡嬪嬙嬤孫學孿寧寶實寵審憲宮寬賓寢對尋導壽將爾塵嘗堯尷屍盡層屓屜屆屬屢屨嶼歲豈嶇崗峴嶴嵐島岩嶺嶽崠巋嶨嶧峽嶢嶠崢巒嶗崍嶮嶄嶸嶔崳嶁脊巔鞏巰幣帥師幃帳簾幟帶幀幫幬幘幗冪襆幹並廣莊慶廬廡庫應廟龐廢廎廩開異棄弑鞏張彌弳彎彈強歸當錄彥徹徑徠禦憶懺憂愾懷態慫憮慪悵愴憐總懟懌戀懇惡慟懨愷惻惱惲悅愨懸慳憫驚懼慘懲憊愜慚憚慣惰惱想惶惹惺憒懾慊願懾慈慌慎懾慕慢慧慨慰慵慷憋憎憔憝憧憨憩憬懍懊懋懣懶懍戲戔戩戰戩戶紮撲扡執擴捫掃揚擾撫拋摶摳掄搶護報擔擬攏揀擁攔擰撥擇掛摯攣掗撾撻挾撓擋撟掙擠揮撏撈損撿換搗據撚擄摑擲撣摻摜揸攬撳攙擱摟攪攜攝攄擺搖擯攤攖撐攆擷擼攛擻攢敵斂數齋斕鬥斬斷無舊時曠暘曇晝曨顯晉曬曉曄暈暉暫曖劄術樸機殺雜權條來楊榪傑極構樅樞棗櫪梘棖槍楓梟櫃檸檉梔柵標棧櫛櫳棟櫨櫟欄樹棲樣欒棬椏橈楨檔桤橋樺檜槳樁夢檮梾檢欞槨櫝槧欏橢樓欖櫬櫚櫸榍檻檳櫧橫檣櫻櫫櫥櫓櫞簷檁歡歟歐殲歿殤殘殞殮殫殯毆毀轂畢斃氈毿氌氣氫氬氳匯漢汙湯洶遝溝沒灃漚瀝淪滄颯溈滬濘淚澩瀧瀘濼瀉潑澤涇潔灑窪浹淺漿澆湞溮濁測澮濟瀏滻渾滸濃潯濜塗湧濤澇淶漣潿渦溳渙滌潤澗漲澀涮涯液涵涸涿澱淄淅淆淇淋淌淑淖淘淙淚淡淤淦淫淬淮深淳混淹添淼清淵漬瀆漸澠漁滲渚渝渠渡渣渤渥溫渫渭港渴遊渺湃湄湍湎湓湔湖湘湛湟湜湮灣濕潰濺漵溉溏源溘溜溟溢溥溧溪溯溱溲溴溶溷溺溠溽滁滂滇滋瀅滑滓滔滕潷滾滯灩滠滿瀅濾濫灤濱灘澦漓漠溽讕漪漫漬漱漲漳漶漾瀠瀟瀲濰潘潛潞潟潢潦潭潮潲瀦潸潺潼澄澈澉澌澍澎瀾澡澧澳澶澹激濂濉瀨瀕濞濠濡濤濫濯濰燈灰靈灶灸燦煬爐燉煒熗點煉熾爍爛烴燭煙煩燒燁燴燙燼熱煥燜燾煆糊溜愛爺牘犛牽犧犢強狀獷獁猶狽麅獮獰獨狹獅獪猙獄猻獫獵獼玀豬貓蝟獻獺璣璵瑒瑪瑋環現瑲璽瑉玨琺瓏璫琿琎璉瑣瓊瑤璦璇瓔瓚甕電畫暢佘疇癤療瘧癘瘍癧瘡瘋皰屙癰痙癢瘂癆瘓癇癡癉瘮瘞瘺癟癱癮癭癩癬癲皚皺皸盞鹽監蓋盜盤瞘眥著睜睞瞼瞞矚矯磯礬礦碭碼磚硨硯碸礪礱礫礎硜矽碩硤磽磑礄確鹼礙磧磣鹼镟禮禕禰禎禱禍稟祿禪離禿稈種積稱穢穠穭稅穌穩穡窮竊竅窵窯竄窩窺竇窶豎競篤筍筆筧箋籠籩築篳篩簹箏籌簽簡籙簀篋籜籮簞簫簣簍籃籬籪籟糴類秈糶糲粵糞糧糝餱緊縶糾紆紅紂纖紇約級紈纊紀紉緯紜紘純紕紗綱納紝縱綸紛紙紋紡紵紖紐紓線紺紲紱練組紳細織終縐絆紱絀紹繹經紿綁絨結絝繞絰絎繪給絢絳絡絕絞統綆綃絹繡綌綏絛繼綈績緒綾緓續綺緋綽緔緄繩維綿綬繃綢綯綹綣綜綻綰綠綴緇緙緗緘緬纜緹緲緝縕繢緦綞緞緶線緱縋緩締縷編緡緣縉縛縟縝縫縗縞纏縭縊縑繽縹縵縲纓縮繆繅纈繚繕繒韁繾繰繯繳纘罌網羅罰罷羆羈羥羨翹翽翬耮耬聳恥聶聾職聹聯聵聰肅腸膚骯肯育肴肵脖脹脅膽勝朧腖臚脛膠脈膾髒臍腦膿臠腳脫腡臉臘醃膕齶膩靦膃騰臏臢輿艤艦艙艫艱豔艹藝節羋薌蕪蘆蓯葦藶莧萇蒼苧蘇檾蘋莖蘢蔦塋煢繭荊薦薘莢蕘蓽蕎薈薺蕩榮葷滎犖熒蕁藎蓀蔭葒莙藥蒞蓧萊蓮蒔萵薟獲蕕瑩鶯蓴蘀蘿螢營縈蕭薩蔥蕆蕢蔣藍薊蘺蕷鎣驀薔蘞藺藹蘄蘊藪槁蘚虜慮虛蟲虯蟣雖蝦蠆蝕蟻螞蠶蠔蜆蠱蠣蟶蠻蟄蛺蟯螄蠐蛻蝸蠟蠅蟈蟬蠍螻蠑攫釁銜補襯袞襖嫋禕襪襲襏裝襠褌褳襝褲襇褸襤繈襴見觀覎規覓視覘覽覺覬覡覿覥覦覯覲覷觴觸觶譽謄計訂訃認譏訐訌討讓訕訖訓議訊記訒講諱謳詎訝訥許訛論訦訟諷設訪訣證詁訶評詛識詗詐訴診詆謅詞詘詔詖譯詒誆誄試詿詩詰詼誠誅詵話誕詬詮詭詢詣諍該詳詫諢詡譸誡誣語誚誤誥誘誨誑說誦誒請諸諏諾讀諑誹課諉諛誰諗調諂諒諄誶談誼謀諶諜謊諫諧謔謁謂諤諭諼讒諮諳諺諦謎諞諝謨讜謖謝謠謗諡謙謐謹謾謫譾謬譚譖譙讕譜譎讞譴譫讖穀豶貝貞負貟貢財責賢敗賬貨質販貪貧貶購貯貫貳賤賁貰貼貴貺貸貿費賀貽賊贄賈賄貲賃賂贓資賅賾賕賑賚賒賦賭齎贖賞賜贔賙賡賠賧賴賵贅賻賺賽賾贗贊贇贈贍贏贛赬趙趕趨趲躉躍蹌蹠轢踐蹺蹕躚躋踴躊蹤躓躑躡蹣躕躥躪躦軀車軋軌軒軑軔轉軛輪軟轟軲軻轤軸軹軼軤軫轢軺輕軾載輊轎輈輇輅較輒輔輛輦輩輝輥輞諒輟輜輳輻輯轀輸轡轅轄輾轆轍轔辭辟辯辮邊遼達遷迂迄迅過邁迎運近返還這進遠違連遲邇逕跡適選遜遞邐邏遺遙鄧鄺鄔郵鄒鄴鄰鬱郤郟鄶鄭鄆酈鄖鄲醞醱醬釅釃釀釋裏鉅鑒鑾鏨釓釔針釘釗釙釕釷釺釧釤鈒釩釣鍆釹鍚釵鈃鈣鈈鈦鉅鈍鈔鐘鈉鋇鋼鈑鈐鑰欽鈞鎢鉤鈧鈁鈥鈄鈕鈀鈺錢鉦鉗鈷缽鈳鉕鈽鈸鉞鑽鉬鉭鉀鈿鈾鐵鉑鈴鑠鉛鉚鈰鉉鉈 bismuth鈹鐸鉶銬銠鉺銪鋏鋣鐃銍鐺銅鋁銱銦鎧鍘銖銑鋌銩銛鏵銓鉿銚鉻銘錚銫鉸銥鏟銃鐋銨銀銣鑄鐒鋪鋙錸鋱鏈鏗銷鎖鋰鋥鋤鍋鋯鋨鏽銼鋝鋒鋅鍀鐦鐧銳銻鋃鋟鋦錒錆鍺錯錨錡錩錫錮鑼錘錐錦鍁錈錇錟錠鍵鋸錳錙鍥鍈鍇鏘鍶鍔鍤鍬鍾鍛鎪鍠鍰鎄鍍鎂鏤鎡鏌鎮鎛鎘鑷鐫鎳鎿鎦鎬鎊鎰鎔鏢鏜鏍镚鏞鏡鏑鏃鏇鐔鐐鏷鑥鐓鑭鐠鑹鏹鐙鑊鐳鐶鐲鐮鐿鑔鑣鑞鑲長門閂閃閆閈閉問闖閏闈閑閎間閔閌悶閘鬧閨聞闥閩閭闓閥閣閡閫鬮閱閬闍閾閹閶鬩閿閽閻閼闡闌闃闠闊闋闔闐闒闕闞闤隊陽陰陣階際陸隴陳陘陝隉隕險隨隱隸雋難雛讎靂霧霽黴靄靚靜靨韃鞽韉韝韋韌韍韓韙韞韜韻頁頂頃頇項順須頊頑顧頓頎頒頌頏預顱領頗頸頡頰頋頜潁熲頦頤頻頮頹頷頴穎顆題顒顎顓顏額顳顢顛顙顥纇顫顬顰顴風颺颭颮颯颶颸颼颻飀飄飆飆飛饗饜飣饑餳餳飩餼飪飫飭飯飲餞飾飽飼飿飴餌饒餉餄餎餃餏餅餑餖餓餘餒餕餜餛餡館餷饋餶餿饞饁饃餺餾饈饉饅饊饌饢馬馭馱馴馳驅馹駁驢駔駛駟駙駒騶駐駝駑駕驛駘驍罵駰驕驊駱駭駢驫驪騁驗騂駸駿騏騎騍騅騌驌驂騙騭騤騷騖驁騮騫騸驃騾驄驏驟驥驦驤髏髖髕鬢魘魎魚魛魢魷魨魯魴魺鱍鮃鮎鱸鮋鮓鮒鮊鮑鱟鮍鮐鮭鮚鮳鮪鮞鮦鰂鮜鱠鱭鮫鮮鮺鯗鱘鯁鱺鰱鰹鯉鰣鰷鯀鯊鯇鮶鯽鯒鯖鯪鯕鯫鯡鯤鯧鯝鯢鯰鯛鯨鰺鯴鯔鱝鰈鰏鱨鯷鰮鰓鱷鰍鰒鰉鰁鱂鯿鰠鰲鰭鰨鰥鰩鰟鰜鰳鰾鱈鱉鰻鰵鏞鰼鱖鱔鱗鱒鱯鱤鱧鱣鳥鳩雞鳶鳴鳲鷗鴉鶬鴇鴆鴣鶇鸕鴨鴞鴦鴒鴟鴝鴛鷽鴕鷥鷙鴯鴰鵂鴴鵃鴿鸞鴻鵐鵓鸝鵑鵠鵝鵒鷳鵜鵡鵲鶓鵪鶤鵯鵬鵮鶉鶘鶚鶻鶿鶥鶩鷊鷂鶲鶹鶺鷁鶼鶴鷖鸚鷓鷚鷯鷦鷲鷸鷺鸇鷹鸌鸏鸛鸘鹵鹹鹺鹼鹽麥麩黃黌黷黲黽鼉鼉齊齏齒齔齟齡齙齠齜齦齬齪齲齷龍龔龕龜");

        // Build from guaranteed 1-to-1 curated pairs array so surrogate/spacing can never drift
        static const wchar_t* const kPairs[][2] = {
            {L"万", L"萬"}, {L"与", L"與"}, {L"专", L"專"}, {L"业", L"業"}, {L"东", L"東"}, {L"丝", L"絲"},
            {L"两", L"兩"}, {L"严", L"嚴"}, {L"丧", L"喪"}, {L"个", L"個"}, {L"丰", L"豐"}, {L"临", L"臨"},
            {L"为", L"為"}, {L"丽", L"麗"}, {L"举", L"舉"}, {L"么", L"麼"}, {L"义", L"義"}, {L"乌", L"烏"},
            {L"乐", L"樂"}, {L"乔", L"喬"}, {L"习", L"習"}, {L"乡", L"鄉"}, {L"书", L"書"}, {L"买", L"買"},
            {L"乱", L"亂"}, {L"争", L"爭"}, {L"云", L"雲"}, {L"亚", L"亞"}, {L"产", L"產"}, {L"亲", L"親"},
            {L"亿", L"億"}, {L"仅", L"僅"}, {L"从", L"從"}, {L"仓", L"倉"}, {L"仪", L"儀"}, {L"们", L"們"},
            {L"价", L"價"}, {L"众", L"眾"}, {L"优", L"優"}, {L"会", L"會"}, {L"伟", L"偉"}, {L"传", L"傳"},
            {L"伤", L"傷"}, {L"伦", L"倫"}, {L"伪", L"偽"}, {L"体", L"體"}, {L"余", L"餘"}, {L"侠", L"俠"},
            {L"侧", L"側"}, {L"侦", L"偵"}, {L"债", L"債"}, {L"倾", L"傾"}, {L"偿", L"償"}, {L"储", L"儲"},
            {L"儿", L"兒"}, {L"兑", L"兌"}, {L"党", L"黨"}, {L"兰", L"蘭"}, {L"关", L"關"}, {L"兴", L"興"},
            {L"养", L"養"}, {L"兽", L"獸"}, {L"内", L"內"}, {L"册", L"冊"}, {L"写", L"寫"}, {L"军", L"軍"},
            {L"农", L"農"}, {L"冲", L"沖"}, {L"决", L"決"}, {L"况", L"況"}, {L"冻", L"凍"}, {L"净", L"淨"},
            {L"凄", L"淒"}, {L"凉", L"涼"}, {L"凌", L"淩"}, {L"减", L"減"}, {L"凑", L"湊"}, {L"几", L"幾"},
            {L"凤", L"鳳"}, {L"凭", L"憑"}, {L"凯", L"凱"}, {L"击", L"擊"}, {L"划", L"劃"}, {L"刘", L"劉"},
            {L"则", L"則"}, {L"刚", L"剛"}, {L"创", L"創"}, {L"删", L"刪"}, {L"别", L"別"}, {L"剂", L"劑"},
            {L"剑", L"劍"}, {L"剧", L"劇"}, {L"劝", L"勸"}, {L"办", L"辦"}, {L"务", L"務"}, {L"动", L"動"},
            {L"励", L"勵"}, {L"劲", L"勁"}, {L"劳", L"勞"}, {L"势", L"勢"}, {L"勋", L"勳"}, {L"区", L"區"},
            {L"医", L"醫"}, {L"华", L"華"}, {L"协", L"協"}, {L"单", L"單"}, {L"卖", L"賣"}, {L"卢", L"盧"},
            {L"卫", L"衛"}, {L"却", L"卻"}, {L"厂", L"廠"}, {L"厅", L"廳"}, {L"历", L"歷"}, {L"厉", L"厲"},
            {L"压", L"壓"}, {L"厌", L"厭"}, {L"厕", L"廁"}, {L"厢", L"廂"}, {L"厦", L"廈"}, {L"厨", L"廚"},
            {L"县", L"縣"}, {L"参", L"參"}, {L"双", L"雙"}, {L"发", L"發"}, {L"变", L"變"}, {L"叙", L"敘"},
            {L"叠", L"疊"}, {L"叶", L"葉"}, {L"号", L"號"}, {L"叹", L"歎"}, {L"后", L"後"}, {L"吓", L"嚇"},
            {L"吗", L"嗎"}, {L"听", L"聽"}, {L"启", L"啟"}, {L"吴", L"吳"}, {L"员", L"員"}, {L"呜", L"嗚"},
            {L"咏", L"詠"}, {L"响", L"響"}, {L"哑", L"啞"}, {L"哗", L"嘩"}, {L"哟", L"喲"}, {L"唤", L"喚"},
            {L"啸", L"嘯"}, {L"喷", L"噴"}, {L"嘘", L"噓"}, {L"嘱", L"囑"}, {L"嚣", L"囂"}, {L"团", L"團"},
            {L"园", L"園"}, {L"围", L"圍"}, {L"国", L"國"}, {L"图", L"圖"}, {L"圆", L"圓"}, {L"圣", L"聖"},
            {L"场", L"場"}, {L"坏", L"壞"}, {L"块", L"塊"}, {L"坚", L"堅"}, {L"坛", L"壇"}, {L"坝", L"壩"},
            {L"坟", L"墳"}, {L"坠", L"墜"}, {L"垄", L"壟"}, {L"垒", L"壘"}, {L"垦", L"墾"}, {L"垫", L"墊"},
            {L"堕", L"墮"}, {L"墙", L"牆"}, {L"壮", L"壯"}, {L"声", L"聲"}, {L"壳", L"殼"}, {L"壶", L"壺"},
            {L"处", L"處"}, {L"备", L"備"}, {L"复", L"復"}, {L"够", L"夠"}, {L"头", L"頭"}, {L"夸", L"誇"},
            {L"夹", L"夾"}, {L"夺", L"奪"}, {L"奋", L"奮"}, {L"奖", L"獎"}, {L"奥", L"奧"}, {L"妆", L"妝"},
            {L"妇", L"婦"}, {L"妈", L"媽"}, {L"妩", L"嫵"}, {L"娇", L"嬌"}, {L"娱", L"娛"}, {L"娴", L"嫻"},
            {L"婴", L"嬰"}, {L"婶", L"嬸"}, {L"孙", L"孫"}, {L"学", L"學"}, {L"宁", L"寧"}, {L"宝", L"寶"},
            {L"实", L"實"}, {L"宠", L"寵"}, {L"审", L"審"}, {L"宪", L"憲"}, {L"宫", L"宮"}, {L"宽", L"寬"},
            {L"宾", L"賓"}, {L"寝", L"寢"}, {L"对", L"對"}, {L"寻", L"尋"}, {L"导", L"導"}, {L"寿", L"壽"},
            {L"将", L"將"}, {L"尔", L"爾"}, {L"尘", L"塵"}, {L"尝", L"嘗"}, {L"尸", L"屍"}, {L"尽", L"盡"},
            {L"层", L"層"}, {L"届", L"屆"}, {L"属", L"屬"}, {L"屡", L"屢"}, {L"屿", L"嶼"}, {L"岁", L"歲"},
            {L"岂", L"豈"}, {L"岗", L"崗"}, {L"岚", L"嵐"}, {L"岛", L"島"}, {L"岭", L"嶺"}, {L"岳", L"嶽"},
            {L"峡", L"峽"}, {L"峰", L"峰"}, {L"峥", L"崢"}, {L"峦", L"巒"}, {L"崭", L"嶄"}, {L"嵘", L"嶸"},
            {L"巅", L"巔"}, {L"币", L"幣"}, {L"帅", L"帥"}, {L"师", L"師"}, {L"帐", L"帳"}, {L"帘", L"簾"},
            {L"带", L"帶"}, {L"帧", L"幀"}, {L"帮", L"幫"}, {L"干", L"幹"}, {L"并", L"並"}, {L"广", L"廣"},
            {L"庄", L"莊"}, {L"庆", L"慶"}, {L"庐", L"廬"}, {L"库", L"庫"}, {L"应", L"應"}, {L"庙", L"廟"},
            {L"庞", L"龐"}, {L"废", L"廢"}, {L"开", L"開"}, {L"异", L"異"}, {L"弃", L"棄"}, {L"张", L"張"},
            {L"弥", L"彌"}, {L"弯", L"彎"}, {L"弹", L"彈"}, {L"强", L"強"}, {L"归", L"歸"}, {L"当", L"當"},
            {L"录", L"錄"}, {L"彻", L"徹"}, {L"径", L"徑"}, {L"御", L"禦"}, {L"忆", L"憶"}, {L"忧", L"憂"},
            {L"怀", L"懷"}, {L"态", L"態"}, {L"怅", L"悵"}, {L"怜", L"憐"}, {L"总", L"總"}, {L"恋", L"戀"},
            {L"恳", L"懇"}, {L"恶", L"惡"}, {L"恼", L"惱"}, {L"悦", L"悅"}, {L"悬", L"懸"}, {L"悯", L"憫"},
            {L"惊", L"驚"}, {L"惧", L"懼"}, {L"惨", L"慘"}, {L"惩", L"懲"}, {L"惭", L"慚"}, {L"惯", L"慣"},
            {L"愤", L"憤"}, {L"愿", L"願"}, {L"慑", L"懾"}, {L"懒", L"懶"}, {L"戏", L"戲"}, {L"战", L"戰"},
            {L"户", L"戶"}, {L"执", L"執"}, {L"扩", L"擴"}, {L"扫", L"掃"}, {L"扬", L"揚"}, {L"扰", L"擾"},
            {L"抚", L"撫"}, {L"抢", L"搶"}, {L"护", L"護"}, {L"报", L"報"}, {L"担", L"擔"}, {L"拟", L"擬"},
            {L"拢", L"攏"}, {L"拥", L"擁"}, {L"拦", L"攔"}, {L"拨", L"撥"}, {L"择", L"擇"}, {L"挂", L"掛"},
            {L"挚", L"摯"}, {L"挡", L"擋"}, {L"挣", L"掙"}, {L"挤", L"擠"}, {L"挥", L"揮"}, {L"捞", L"撈"},
            {L"损", L"損"}, {L"捡", L"撿"}, {L"换", L"換"}, {L"据", L"據"}, {L"掷", L"擲"}, {L"揽", L"攬"},
            {L"搁", L"擱"}, {L"搂", L"摟"}, {L"搅", L"攪"}, {L"携", L"攜"}, {L"摄", L"攝"}, {L"摆", L"擺"},
            {L"摇", L"搖"}, {L"摊", L"攤"}, {L"撑", L"撐"}, {L"敌", L"敵"}, {L"敛", L"斂"}, {L"数", L"數"},
            {L"斋", L"齋"}, {L"斗", L"鬥"}, {L"斩", L"斬"}, {L"断", L"斷"}, {L"无", L"無"}, {L"旧", L"舊"},
            {L"时", L"時"}, {L"旷", L"曠"}, {L"昼", L"晝"}, {L"显", L"顯"}, {L"晋", L"晉"}, {L"晒", L"曬"},
            {L"晓", L"曉"}, {L"晕", L"暈"}, {L"暂", L"暫"}, {L"术", L"術"}, {L"机", L"機"}, {L"杀", L"殺"},
            {L"杂", L"雜"}, {L"权", L"權"}, {L"条", L"條"}, {L"来", L"來"}, {L"杨", L"楊"}, {L"杰", L"傑"},
            {L"极", L"極"}, {L"构", L"構"}, {L"枪", L"槍"}, {L"枫", L"楓"}, {L"柜", L"櫃"}, {L"标", L"標"},
            {L"栈", L"棧"}, {L"栋", L"棟"}, {L"栏", L"欄"}, {L"树", L"樹"}, {L"样", L"樣"}, {L"档", L"檔"},
            {L"桥", L"橋"}, {L"桨", L"槳"}, {L"梦", L"夢"}, {L"检", L"檢"}, {L"椭", L"橢"}, {L"楼", L"樓"},
            {L"槛", L"檻"}, {L"横", L"橫"}, {L"樱", L"櫻"}, {L"欢", L"歡"}, {L"欧", L"歐"}, {L"残", L"殘"},
            {L"毁", L"毀"}, {L"毕", L"畢"}, {L"毙", L"斃"}, {L"气", L"氣"}, {L"汇", L"匯"}, {L"汉", L"漢"},
            {L"汤", L"湯"}, {L"沟", L"溝"}, {L"没", L"沒"}, {L"沧", L"滄"}, {L"沪", L"滬"}, {L"泪", L"淚"},
            {L"泻", L"瀉"}, {L"泼", L"潑"}, {L"泽", L"澤"}, {L"洁", L"潔"}, {L"洒", L"灑"}, {L"浅", L"淺"},
            {L"浆", L"漿"}, {L"浇", L"澆"}, {L"浊", L"濁"}, {L"测", L"測"}, {L"济", L"濟"}, {L"浏", L"瀏"},
            {L"浑", L"渾"}, {L"浓", L"濃"}, {L"涂", L"塗"}, {L"涌", L"湧"}, {L"涛", L"濤"}, {L"涡", L"渦"},
            {L"涤", L"滌"}, {L"润", L"潤"}, {L"涧", L"澗"}, {L"涨", L"漲"}, {L"涩", L"澀"}, {L"渊", L"淵"},
            {L"渐", L"漸"}, {L"渔", L"漁"}, {L"渗", L"滲"}, {L"温", L"溫"}, {L"游", L"遊"}, {L"湾", L"灣"},
            {L"湿", L"濕"}, {L"溃", L"潰"}, {L"溅", L"濺"}, {L"滚", L"滾"}, {L"滞", L"滯"}, {L"满", L"滿"},
            {L"滤", L"濾"}, {L"滥", L"濫"}, {L"滨", L"濱"}, {L"滩", L"灘"}, {L"漫", L"漫"}, {L"潜", L"潛"},
            {L"潮", L"潮"}, {L"澎", L"澎"}, {L"澜", L"瀾"}, {L"濒", L"瀕"}, {L"灯", L"燈"}, {L"灵", L"靈"},
            {L"灶", L"灶"}, {L"灿", L"燦"}, {L"炉", L"爐"}, {L"点", L"點"}, {L"炼", L"煉"}, {L"炽", L"熾"},
            {L"烁", L"爍"}, {L"烂", L"爛"}, {L"烛", L"燭"}, {L"烟", L"煙"}, {L"烦", L"煩"}, {L"烧", L"燒"},
            {L"烫", L"燙"}, {L"烬", L"燼"}, {L"热", L"熱"}, {L"爱", L"愛"}, {L"爷", L"爺"}, {L"牵", L"牽"},
            {L"牺", L"犧"}, {L"状", L"狀"}, {L"犹", L"猶"}, {L"狞", L"獰"}, {L"独", L"獨"}, {L"狭", L"狹"},
            {L"狮", L"獅"}, {L"狱", L"獄"}, {L"猎", L"獵"}, {L"猪", L"豬"}, {L"猫", L"貓"}, {L"献", L"獻"},
            {L"玛", L"瑪"}, {L"环", L"環"}, {L"现", L"現"}, {L"琐", L"瑣"}, {L"琼", L"瓊"}, {L"瑶", L"瑤"},
            {L"电", L"電"}, {L"画", L"畫"}, {L"畅", L"暢"}, {L"疗", L"療"}, {L"疯", L"瘋"}, {L"痒", L"癢"},
            {L"痴", L"癡"}, {L"瘫", L"癱"}, {L"瘾", L"癮"}, {L"皱", L"皺"}, {L"盐", L"鹽"}, {L"监", L"監"},
            {L"盖", L"蓋"}, {L"盗", L"盜"}, {L"盘", L"盤"}, {L"着", L"著"}, {L"睁", L"睜"}, {L"瞒", L"瞞"},
            {L"瞩", L"矚"}, {L"矫", L"矯"}, {L"矿", L"礦"}, {L"码", L"碼"}, {L"砖", L"磚"}, {L"砚", L"硯"},
            {L"础", L"礎"}, {L"硕", L"碩"}, {L"确", L"確"}, {L"碍", L"礙"}, {L"礼", L"禮"}, {L"祯", L"禎"},
            {L"祷", L"禱"}, {L"祸", L"禍"}, {L"禄", L"祿"}, {L"禅", L"禪"}, {L"离", L"離"}, {L"种", L"種"},
            {L"积", L"積"}, {L"称", L"稱"}, {L"税", L"稅"}, {L"稳", L"穩"}, {L"穷", L"窮"}, {L"窃", L"竊"},
            {L"窍", L"竅"}, {L"窜", L"竄"}, {L"窝", L"窩"}, {L"窥", L"窺"}, {L"竖", L"豎"}, {L"竞", L"競"},
            {L"笔", L"筆"}, {L"笼", L"籠"}, {L"筑", L"築"}, {L"筛", L"篩"}, {L"筹", L"籌"}, {L"签", L"簽"},
            {L"简", L"簡"}, {L"箩", L"籮"}, {L"箫", L"簫"}, {L"篮", L"籃"}, {L"篱", L"籬"}, {L"类", L"類"},
            {L"粮", L"糧"}, {L"紧", L"緊"}, {L"纠", L"糾"}, {L"红", L"紅"}, {L"纤", L"纖"}, {L"约", L"約"},
            {L"级", L"級"}, {L"纪", L"紀"}, {L"纯", L"純"}, {L"纱", L"紗"}, {L"纲", L"綱"}, {L"纳", L"納"},
            {L"纵", L"縱"}, {L"纶", L"綸"}, {L"纷", L"紛"}, {L"纸", L"紙"}, {L"纹", L"紋"}, {L"纺", L"紡"},
            {L"纽", L"紐"}, {L"线", L"線"}, {L"练", L"練"}, {L"组", L"組"}, {L"绅", L"紳"}, {L"细", L"細"},
            {L"织", L"織"}, {L"终", L"終"}, {L"绊", L"絆"}, {L"绍", L"紹"}, {L"绎", L"繹"}, {L"经", L"經"},
            {L"绑", L"綁"}, {L"绒", L"絨"}, {L"结", L"結"}, {L"绕", L"繞"}, {L"绘", L"繪"}, {L"给", L"給"},
            {L"绚", L"絢"}, {L"络", L"絡"}, {L"绝", L"絕"}, {L"绞", L"絞"}, {L"统", L"統"}, {L"绢", L"絹"},
            {L"绣", L"繡"}, {L"继", L"繼"}, {L"绩", L"績"}, {L"绪", L"緒"}, {L"续", L"續"}, {L"绯", L"緋"},
            {L"绳", L"繩"}, {L"维", L"維"}, {L"绵", L"綿"}, {L"绷", L"繃"}, {L"绸", L"綢"}, {L"综", L"綜"},
            {L"绽", L"綻"}, {L"绿", L"綠"}, {L"缀", L"綴"}, {L"缅", L"緬"}, {L"缆", L"纜"}, {L"缎", L"緞"},
            {L"缓", L"緩"}, {L"缔", L"締"}, {L"缕", L"縷"}, {L"编", L"編"}, {L"缘", L"緣"}, {L"缚", L"縛"},
            {L"缝", L"縫"}, {L"缠", L"纏"}, {L"缩", L"縮"}, {L"缭", L"繚"}, {L"缴", L"繳"}, {L"罗", L"羅"},
            {L"罚", L"罰"}, {L"罢", L"罷"}, {L"羡", L"羨"}, {L"翘", L"翹"}, {L"耸", L"聳"}, {L"耻", L"恥"},
            {L"聋", L"聾"}, {L"职", L"職"}, {L"联", L"聯"}, {L"聪", L"聰"}, {L"肃", L"肅"}, {L"肠", L"腸"},
            {L"肤", L"膚"}, {L"胁", L"脅"}, {L"胆", L"膽"}, {L"胜", L"勝"}, {L"胧", L"朧"}, {L"脉", L"脈"},
            {L"脏", L"臟"}, {L"脑", L"腦"}, {L"脚", L"腳"}, {L"脱", L"脫"}, {L"脸", L"臉"}, {L"腊", L"臘"},
            {L"腻", L"膩"}, {L"腾", L"騰"}, {L"舰", L"艦"}, {L"舱", L"艙"}, {L"艰", L"艱"}, {L"艳", L"豔"},
            {L"艺", L"藝"}, {L"节", L"節"}, {L"芦", L"蘆"}, {L"苍", L"蒼"}, {L"苏", L"蘇"}, {L"苹", L"蘋"},
            {L"茎", L"莖"}, {L"茧", L"繭"}, {L"荆", L"荊"}, {L"荐", L"薦"}, {L"荡", L"蕩"}, {L"荣", L"榮"},
            {L"荧", L"熒"}, {L"药", L"藥"}, {L"莱", L"萊"}, {L"莲", L"蓮"}, {L"获", L"獲"}, {L"莹", L"瑩"},
            {L"莺", L"鶯"}, {L"萝", L"蘿"}, {L"萤", L"螢"}, {L"营", L"營"}, {L"萧", L"蕭"}, {L"萨", L"薩"},
            {L"葱", L"蔥"}, {L"蒋", L"蔣"}, {L"蓝", L"藍"}, {L"蔷", L"薔"}, {L"蕴", L"蘊"}, {L"虏", L"虜"},
            {L"虑", L"慮"}, {L"虚", L"虛"}, {L"虫", L"蟲"}, {L"虽", L"雖"}, {L"虾", L"蝦"}, {L"蚀", L"蝕"},
            {L"蚁", L"蟻"}, {L"蚂", L"螞"}, {L"蚕", L"蠶"}, {L"蛮", L"蠻"}, {L"蜕", L"蛻"}, {L"蜗", L"蝸"},
            {L"蜡", L"蠟"}, {L"蝇", L"蠅"}, {L"蝉", L"蟬"}, {L"蝶", L"蝶"}, {L"衔", L"銜"}, {L"补", L"補"},
            {L"衬", L"襯"}, {L"袄", L"襖"}, {L"袜", L"襪"}, {L"袭", L"襲"}, {L"装", L"裝"}, {L"裤", L"褲"},
            {L"见", L"見"}, {L"观", L"觀"}, {L"规", L"規"}, {L"觅", L"覓"}, {L"视", L"視"}, {L"览", L"覽"},
            {L"觉", L"覺"}, {L"触", L"觸"}, {L"誉", L"譽"}, {L"计", L"計"}, {L"订", L"訂"}, {L"认", L"認"},
            {L"讥", L"譏"}, {L"讨", L"討"}, {L"让", L"讓"}, {L"训", L"訓"}, {L"议", L"議"}, {L"讯", L"訊"},
            {L"记", L"記"}, {L"讲", L"講"}, {L"讳", L"諱"}, {L"讶", L"訝"}, {L"许", L"許"}, {L"讹", L"訛"},
            {L"论", L"論"}, {L"讼", L"訟"}, {L"讽", L"諷"}, {L"设", L"設"}, {L"访", L"訪"}, {L"诀", L"訣"},
            {L"证", L"證"}, {L"评", L"評"}, {L"诅", L"詛"}, {L"识", L"識"}, {L"诈", L"詐"}, {L"诉", L"訴"},
            {L"诊", L"診"}, {L"词", L"詞"}, {L"诏", L"詔"}, {L"译", L"譯"}, {L"试", L"試"}, {L"诗", L"詩"},
            {L"诚", L"誠"}, {L"诛", L"誅"}, {L"话", L"話"}, {L"诞", L"誕"}, {L"诡", L"詭"}, {L"询", L"詢"},
            {L"该", L"該"}, {L"详", L"詳"}, {L"诫", L"誡"}, {L"诬", L"誣"}, {L"语", L"語"}, {L"误", L"誤"},
            {L"诱", L"誘"}, {L"诲", L"誨"}, {L"说", L"說"}, {L"诵", L"誦"}, {L"请", L"請"}, {L"诸", L"諸"},
            {L"诺", L"諾"}, {L"读", L"讀"}, {L"诽", L"誹"}, {L"课", L"課"}, {L"谁", L"誰"}, {L"调", L"調"},
            {L"谅", L"諒"}, {L"谈", L"談"}, {L"谊", L"誼"}, {L"谋", L"謀"}, {L"谎", L"謊"}, {L"谐", L"諧"},
            {L"谓", L"謂"}, {L"谚", L"諺"}, {L"谜", L"謎"}, {L"谢", L"謝"}, {L"谣", L"謠"}, {L"谤", L"謗"},
            {L"谦", L"謙"}, {L"谨", L"謹"}, {L"谬", L"謬"}, {L"谭", L"譚"}, {L"谱", L"譜"}, {L"谴", L"譴"},
            {L"贝", L"貝"}, {L"贞", L"貞"}, {L"负", L"負"}, {L"贡", L"貢"}, {L"财", L"財"}, {L"责", L"責"},
            {L"贤", L"賢"}, {L"败", L"敗"}, {L"账", L"賬"}, {L"货", L"貨"}, {L"质", L"質"}, {L"贩", L"販"},
            {L"贪", L"貪"}, {L"贫", L"貧"}, {L"贬", L"貶"}, {L"购", L"購"}, {L"贮", L"貯"}, {L"贯", L"貫"},
            {L"贱", L"賤"}, {L"贴", L"貼"}, {L"贵", L"貴"}, {L"贷", L"貸"}, {L"贸", L"貿"}, {L"费", L"費"},
            {L"贺", L"賀"}, {L"贼", L"賊"}, {L"贾", L"賈"}, {L"贿", L"賄"}, {L"赁", L"賃"}, {L"赂", L"賂"},
            {L"赃", L"贓"}, {L"资", L"資"}, {L"赈", L"賑"}, {L"赊", L"賒"}, {L"赋", L"賦"}, {L"赌", L"賭"},
            {L"赎", L"贖"}, {L"赏", L"賞"}, {L"赐", L"賜"}, {L"赔", L"賠"}, {L"赖", L"賴"}, {L"赘", L"贅"},
            {L"赚", L"賺"}, {L"赛", L"賽"}, {L"赞", L"贊"}, {L"赠", L"贈"}, {L"赡", L"贍"}, {L"赢", L"贏"},
            {L"赵", L"趙"}, {L"赶", L"趕"}, {L"趋", L"趨"}, {L"跃", L"躍"}, {L"践", L"踐"}, {L"跷", L"蹺"},
            {L"踊", L"踴"}, {L"踌", L"躊"}, {L"踪", L"蹤"}, {L"躯", L"軀"}, {L"车", L"車"}, {L"轧", L"軋"},
            {L"轨", L"軌"}, {L"轩", L"軒"}, {L"转", L"轉"}, {L"轮", L"輪"}, {L"软", L"軟"}, {L"轰", L"轟"},
            {L"轴", L"軸"}, {L"轻", L"輕"}, {L"载", L"載"}, {L"轿", L"轎"}, {L"较", L"較"}, {L"辅", L"輔"},
            {L"辆", L"輛"}, {L"辈", L"輩"}, {L"辉", L"輝"}, {L"辑", L"輯"}, {L"输", L"輸"}, {L"辕", L"轅"},
            {L"辖", L"轄"}, {L"辗", L"輾"}, {L"辙", L"轍"}, {L"辞", L"辭"}, {L"辩", L"辯"}, {L"辫", L"辮"},
            {L"边", L"邊"}, {L"辽", L"遼"}, {L"达", L"達"}, {L"迁", L"遷"}, {L"过", L"過"}, {L"迈", L"邁"},
            {L"运", L"運"}, {L"还", L"還"}, {L"这", L"這"}, {L"进", L"進"}, {L"远", L"遠"}, {L"违", L"違"},
            {L"连", L"連"}, {L"迟", L"遲"}, {L"迹", L"跡"}, {L"适", L"適"}, {L"选", L"選"}, {L"逊", L"遜"},
            {L"递", L"遞"}, {L"逻", L"邏"}, {L"遗", L"遺"}, {L"遥", L"遙"}, {L"邓", L"鄧"}, {L"邮", L"郵"},
            {L"邹", L"鄒"}, {L"邻", L"鄰"}, {L"郁", L"鬱"}, {L"郑", L"鄭"}, {L"酱", L"醬"}, {L"酿", L"釀"},
            {L"释", L"釋"}, {L"鉴", L"鑒"}, {L"针", L"針"}, {L"钉", L"釘"}, {L"钓", L"釣"}, {L"钗", L"釵"},
            {L"钙", L"鈣"}, {L"钝", L"鈍"}, {L"钞", L"鈔"}, {L"钟", L"鐘"}, {L"钢", L"鋼"}, {L"钥", L"鑰"},
            {L"钦", L"欽"}, {L"钧", L"鈞"}, {L"钩", L"鉤"}, {L"钮", L"鈕"}, {L"钱", L"錢"}, {L"钳", L"鉗"},
            {L"钻", L"鑽"}, {L"铁", L"鐵"}, {L"铃", L"鈴"}, {L"铅", L"鉛"}, {L"铜", L"銅"}, {L"铝", L"鋁"},
            {L"铠", L"鎧"}, {L"铣", L"銑"}, {L"铭", L"銘"}, {L"铲", L"鏟"}, {L"银", L"銀"}, {L"铸", L"鑄"},
            {L"铺", L"鋪"}, {L"链", L"鏈"}, {L"销", L"銷"}, {L"锁", L"鎖"}, {L"锄", L"鋤"}, {L"锅", L"鍋"},
            {L"锈", L"鏽"}, {L"锋", L"鋒"}, {L"锌", L"鋅"}, {L"锐", L"銳"}, {L"错", L"錯"}, {L"锚", L"錨"},
            {L"锡", L"錫"}, {L"锣", L"鑼"}, {L"锤", L"錘"}, {L"锥", L"錐"}, {L"锦", L"錦"}, {L"锭", L"錠"},
            {L"键", L"鍵"}, {L"锯", L"鋸"}, {L"锻", L"鍛"}, {L"镀", L"鍍"}, {L"镇", L"鎮"}, {L"镊", L"鑷"},
            {L"镐", L"鎬"}, {L"镖", L"鏢"}, {L"镜", L"鏡"}, {L"镣", L"鐐"}, {L"镯", L"鐲"}, {L"镰", L"鐮"},
            {L"镶", L"鑲"}, {L"长", L"長"}, {L"门", L"門"}, {L"闪", L"閃"}, {L"闭", L"閉"}, {L"问", L"問"},
            {L"闯", L"闖"}, {L"闰", L"閏"}, {L"闲", L"閑"}, {L"间", L"間"}, {L"闷", L"悶"}, {L"闸", L"閘"},
            {L"闹", L"鬧"}, {L"闺", L"閨"}, {L"闻", L"聞"}, {L"闽", L"閩"}, {L"阀", L"閥"}, {L"阁", L"閣"},
            {L"阅", L"閱"}, {L"阉", L"閹"}, {L"阎", L"閻"}, {L"阐", L"闡"}, {L"阑", L"闌"}, {L"阔", L"闊"},
            {L"队", L"隊"}, {L"阳", L"陽"}, {L"阴", L"陰"}, {L"阵", L"陣"}, {L"阶", L"階"}, {L"际", L"際"},
            {L"陆", L"陸"}, {L"陈", L"陳"}, {L"陕", L"陝"}, {L"陨", L"隕"}, {L"险", L"險"}, {L"随", L"隨"},
            {L"隐", L"隱"}, {L"难", L"難"}, {L"雏", L"雛"}, {L"雾", L"霧"}, {L"霁", L"霽"}, {L"霉", L"黴"},
            {L"静", L"靜"}, {L"韧", L"韌"}, {L"韩", L"韓"}, {L"韵", L"韻"}, {L"页", L"頁"}, {L"顶", L"頂"},
            {L"顷", L"頃"}, {L"项", L"項"}, {L"顺", L"順"}, {L"须", L"須"}, {L"顽", L"頑"}, {L"顾", L"顧"},
            {L"顿", L"頓"}, {L"颁", L"頒"}, {L"颂", L"頌"}, {L"预", L"預"}, {L"颅", L"顱"}, {L"领", L"領"},
            {L"颇", L"頗"}, {L"颈", L"頸"}, {L"颊", L"頰"}, {L"颐", L"頤"}, {L"频", L"頻"}, {L"颓", L"頹"},
            {L"颖", L"穎"}, {L"颗", L"顆"}, {L"题", L"題"}, {L"颜", L"顏"}, {L"额", L"額"}, {L"颠", L"顛"},
            {L"颤", L"顫"}, {L"风", L"風"}, {L"飒", L"颯"}, {L"飓", L"颶"}, {L"飘", L"飄"}, {L"飙", L"飆"},
            {L"飞", L"飛"}, {L"饥", L"饑"}, {L"饭", L"飯"}, {L"饮", L"飲"}, {L"饯", L"餞"}, {L"饰", L"飾"},
            {L"饱", L"飽"}, {L"饲", L"飼"}, {L"饶", L"饒"}, {L"饺", L"餃"}, {L"饼", L"餅"}, {L"饿", L"餓"},
            {L"馆", L"館"}, {L"馈", L"饋"}, {L"馋", L"饞"}, {L"馒", L"饅"}, {L"马", L"馬"}, {L"驭", L"馭"},
            {L"驮", L"馱"}, {L"驯", L"馴"}, {L"驰", L"馳"}, {L"驱", L"驅"}, {L"驳", L"駁"}, {L"驴", L"驢"},
            {L"驶", L"駛"}, {L"驹", L"駒"}, {L"驻", L"駐"}, {L"驼", L"駝"}, {L"驾", L"駕"}, {L"驿", L"驛"},
            {L"骁", L"驍"}, {L"骂", L"罵"}, {L"骄", L"驕"}, {L"骆", L"駱"}, {L"骇", L"駭"}, {L"骋", L"騁"},
            {L"验", L"驗"}, {L"骏", L"駿"}, {L"骑", L"騎"}, {L"骗", L"騙"}, {L"骚", L"騷"}, {L"骤", L"驟"},
            {L"骥", L"驥"}, {L"鱼", L"魚"}, {L"鲁", L"魯"}, {L"鲍", L"鮑"}, {L"鲜", L"鮮"}, {L"鲤", L"鯉"},
            {L"鲨", L"鯊"}, {L"鲫", L"鯽"}, {L"鲸", L"鯨"}, {L"鳄", L"鱷"}, {L"鳍", L"鰭"}, {L"鳖", L"鱉"},
            {L"鳞", L"鱗"}, {L"鸟", L"鳥"}, {L"鸠", L"鳩"}, {L"鸡", L"雞"}, {L"鸢", L"鳶"}, {L"鸣", L"鳴"},
            {L"鸥", L"鷗"}, {L"鸦", L"鴉"}, {L"鸭", L"鴨"}, {L"鸯", L"鴦"}, {L"鸳", L"鴛"}, {L"鸵", L"鴕"},
            {L"鸽", L"鴿"}, {L"鸾", L"鸞"}, {L"鸿", L"鴻"}, {L"鹃", L"鵑"}, {L"鹅", L"鵝"}, {L"鹉", L"鵡"},
            {L"鹊", L"鵲"}, {L"鹏", L"鵬"}, {L"鹤", L"鶴"}, {L"鹦", L"鸚"}, {L"鹫", L"鷲"}, {L"鹭", L"鷺"},
            {L"鹰", L"鷹"}, {L"鹳", L"鸛"}, {L"咸", L"鹹"}, {L"碱", L"鹼"}, {L"麦", L"麥"}, {L"黄", L"黃"},
            {L"齐", L"齊"}, {L"齿", L"齒"}, {L"龄", L"齡"}, {L"龙", L"龍"}, {L"龚", L"龔"}, {L"龟", L"龜"}
        };
        (void)simp;
        (void)trad;

        ZhCharTables out;
        for (const auto& pair : kPairs) {
            const auto sc = QString::fromWCharArray(pair[0]).toUcs4();
            const auto tc = QString::fromWCharArray(pair[1]).toUcs4();
            if (!sc.isEmpty() && !tc.isEmpty() && sc[0] != tc[0]) {
                out.s2t[static_cast<char32_t>(sc[0])] = static_cast<char32_t>(tc[0]);
                out.t2s[static_cast<char32_t>(tc[0])] = static_cast<char32_t>(sc[0]);
            }
        }
        return out;
    }();
    return tables;
}

std::string makeIndentSpaces(const NovelStyleConfig& style, bool fullWidthIdeographic) {
    if (!style.autoIndent || style.indentSpaces <= 0) return "";
    const int n = std::clamp(style.indentSpaces, 0, 8);
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += fullWidthIdeographic ? "　" : " ";
    }
    return out;
}

} // namespace

std::string NovelEngine::convertChineseVariant(const std::string& utf8Text, const std::string& mode) {
    if (mode != "t2s" && mode != "s2t") return utf8Text;
    const auto& tbl = (mode == "t2s") ? zhTables().t2s : zhTables().s2t;

    const QString qIn = QString::fromUtf8(utf8Text.data(), static_cast<qsizetype>(utf8Text.size()));
    QList<uint> ucs4 = qIn.toUcs4();
    for (qsizetype i = 0; i < ucs4.size(); ++i) {
        auto it = tbl.find(static_cast<char32_t>(ucs4[i]));
        if (it != tbl.end()) {
            ucs4[i] = static_cast<uint>(it->second);
        }
    }
    return QString::fromUcs4(reinterpret_cast<const char32_t*>(ucs4.constData()), ucs4.size()).toUtf8().toStdString();
}

QByteArray NovelEngine::prepareCoverJpeg(const QImage& rawCover) {
    if (rawCover.isNull()) return {};

    QImage scaled = rawCover;
    if (scaled.width() > 960 || scaled.height() > 1360) {
        scaled = scaled.scaled(960, 1360, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QImage rgb(scaled.size(), QImage::Format_RGB32);
    rgb.fill(Qt::white);
    QPainter p(&rgb);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    p.drawImage(0, 0, scaled);
    p.end();

    QByteArray jpegBytes;
    QBuffer buf(&jpegBytes);
    buf.open(QIODevice::WriteOnly);
    QImageWriter writer(&buf, "jpeg");
    writer.setQuality(88);
    writer.write(rgb.convertToFormat(QImage::Format_RGB888));
    buf.close();
    return jpegBytes;
}

QImage NovelEngine::generateProceduralCover(const std::string& title, const std::string& author) {
    const int W = 800;
    const int H = 1140;
    QImage canvas(W, H, QImage::Format_RGB32);

    const QString qTitle = QString::fromUtf8(title.empty() ? "未命名书籍" : title.c_str()).trimmed();
    const QString qAuthor = QString::fromUtf8(author.empty() ? "佚名" : author.c_str()).trimmed();

    // Deterministic literary color theme based on title hash
    uint hashVal = 2166136261u;
    for (QChar c : qTitle) {
        hashVal ^= static_cast<uint>(c.unicode());
        hashVal *= 16777619u;
    }

    struct CoverPalette {
        QColor topBg;
        QColor botBg;
        QColor frame;
        QColor titleFg;
        QColor subFg;
    };
    static const CoverPalette palettes[] = {
        {QColor("#1B2A38"), QColor("#0F1722"), QColor("#D4AF7A"), QColor("#F8F4EC"), QColor("#C8B598")}, // 黛青鎏金
        {QColor("#4A1C1C"), QColor("#260D0D"), QColor("#E2B887"), QColor("#FDF7F2"), QColor("#DFC3A4")}, // 朱砂古籍
        {QColor("#1E352F"), QColor("#10201C"), QColor("#BBD1C5"), QColor("#F4F9F6"), QColor("#A9C4B6")}, // 竹青雅韵
        {QColor("#28233C"), QColor("#141122"), QColor("#C8B6E2"), QColor("#F7F5FC"), QColor("#B9A9D4")}, // 暮紫星河
        {QColor("#EFE9DC"), QColor("#DFD5C0"), QColor("#5C4B37"), QColor("#231D16"), QColor("#5A4E40")}  // 宣纸墨香
    };
    const CoverPalette& pal = palettes[hashVal % 5];

    QPainter painter(&canvas);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    QLinearGradient bgGrad(0, 0, W, H);
    bgGrad.setColorAt(0.0, pal.topBg);
    bgGrad.setColorAt(1.0, pal.botBg);
    painter.fillRect(0, 0, W, H, bgGrad);

    // Subtle book-binding crease & double classical frame
    QColor accentLine = pal.frame;
    accentLine.setAlpha(95);
    painter.setPen(QPen(accentLine, 2.0));
    painter.drawRect(QRectF(48, 48, W - 96, H - 96));

    accentLine.setAlpha(45);
    painter.setPen(QPen(accentLine, 1.0));
    painter.drawRect(QRectF(60, 60, W - 120, H - 120));

    // Decorative corner brackets
    accentLine.setAlpha(180);
    painter.setPen(QPen(accentLine, 3.5));
    const double cLen = 28.0;
    painter.drawLine(QPointF(48, 48), QPointF(48 + cLen, 48));
    painter.drawLine(QPointF(48, 48), QPointF(48, 48 + cLen));
    painter.drawLine(QPointF(W - 48, 48), QPointF(W - 48 - cLen, 48));
    painter.drawLine(QPointF(W - 48, 48), QPointF(W - 48, 48 + cLen));
    painter.drawLine(QPointF(48, H - 48), QPointF(48 + cLen, H - 48));
    painter.drawLine(QPointF(48, H - 48), QPointF(48, H - 48 - cLen));
    painter.drawLine(QPointF(W - 48, H - 48), QPointF(W - 48 - cLen, H - 48));
    painter.drawLine(QPointF(W - 48, H - 48), QPointF(W - 48, H - 48 - cLen));

    // Title block
    QFont titleFont(QStringLiteral("Songti SC"));
    titleFont.setStyleHint(QFont::Serif);
    titleFont.setBold(true);
    const int titlePx = (qTitle.size() <= 6) ? 74 : ((qTitle.size() <= 12) ? 58 : 44);
    titleFont.setPixelSize(titlePx);
    painter.setFont(titleFont);
    painter.setPen(pal.titleFg);

    const QRectF titleRect(100, 230, W - 200, 420);
    painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, qTitle);

    // Center ornamental divider
    accentLine.setAlpha(150);
    painter.setPen(QPen(accentLine, 2.0));
    painter.drawLine(QPointF(W * 0.34, 700), QPointF(W * 0.66, 700));
    painter.setBrush(pal.frame);
    painter.drawEllipse(QPointF(W * 0.5, 700), 5.0, 5.0);

    // Author label
    QFont authorFont(QStringLiteral("PingFang SC"));
    authorFont.setStyleHint(QFont::SansSerif);
    authorFont.setPixelSize(30);
    painter.setFont(authorFont);
    painter.setPen(pal.subFg);
    painter.drawText(QRectF(100, 745, W - 200, 64), Qt::AlignCenter, qAuthor + QStringLiteral("  著"));

    // Bottom publisher / edition imprint
    QFont footFont(QStringLiteral("PingFang SC"));
    footFont.setPixelSize(18);
    footFont.setLetterSpacing(QFont::AbsoluteSpacing, 3.0);
    painter.setFont(footFont);
    QColor imprintColor = pal.subFg;
    imprintColor.setAlpha(140);
    painter.setPen(imprintColor);
    painter.drawText(QRectF(100, H - 120, W - 200, 36), Qt::AlignCenter, QStringLiteral("OMNICONVERT EDITION"));

    painter.end();
    return canvas;
}

std::vector<Chapter> NovelEngine::splitTextIntoChapters(
    const std::string& rawUtf8Text,
    bool autoSplitLong,
    const std::string& customChapterRegex,
    bool cleanAds) {

    QString normalized = QString::fromUtf8(rawUtf8Text.data(), static_cast<qsizetype>(rawUtf8Text.size()));
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace('\r', '\n');

    QRegularExpression userRe;
    const QRegularExpression* userRePtr = nullptr;
    if (!customChapterRegex.empty()) {
        userRe = QRegularExpression(QString::fromUtf8(customChapterRegex.c_str()), QRegularExpression::UseUnicodePropertiesOption);
        if (userRe.isValid()) userRePtr = &userRe;
    }

    std::vector<Chapter> chapters;
    std::string currentTitle = "序言 / 开始";
    std::vector<std::string> currentParas;
    bool foundExplicitChapter = false;

    for (const QString& rawLine : normalized.split('\n')) {
        const QString stripped = rawLine.trimmed();
        if (stripped.isEmpty()) continue;

        if (matchesChapterHeading(stripped, userRePtr)) {
            if (!currentParas.empty()) {
                chapters.push_back(Chapter{currentTitle, std::move(currentParas)});
                currentParas.clear();
            }
            currentTitle = stripped.toUtf8().toStdString();
            foundExplicitChapter = true;
        } else {
            const QString norm = normalizeParagraphQ(stripped);
            if (!norm.isEmpty() && (!cleanAds || !isWebNovelAdJunkLine(norm))) {
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

std::vector<Chapter> NovelEngine::extractChaptersFromHtml(
    const std::string& htmlUtf8,
    const std::string& fallbackTitle,
    const std::string& customChapterRegex,
    bool cleanAds) {

    QString html = QString::fromUtf8(htmlUtf8.data(), static_cast<qsizetype>(htmlUtf8.size()));

    static const QRegularExpression stripTagsRe(
        QStringLiteral(R"(<(script|style|nav|noscript|head)\b[^>]*>.*?</\1>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    html.remove(stripTagsRe);

    static const QRegularExpression brRe(QStringLiteral(R"(<(?:br|mbp:pagebreak)\b[^>]*/?>)"),
                                         QRegularExpression::CaseInsensitiveOption);
    html.replace(brRe, QStringLiteral("\n"));

    static const QRegularExpression headingRe(
        QStringLiteral(R"(<(h[1-3])\b[^>]*>(.*?)</\1>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    html.replace(headingRe, QStringLiteral("\n\x1EHEADING_START\x1F\\2\x1EHEADING_END\x1F\n"));

    static const QRegularExpression blockCloseRe(
        QStringLiteral(R"(</?(?:p|div|blockquote|li|section|article|tr)\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(blockCloseRe, QStringLiteral("\n"));

    static const QRegularExpression anyTagRe(QStringLiteral(R"(<[^>]+>)"));
    html.remove(anyTagRe);
    html = decodeHtmlEntities(html);

    QRegularExpression userRe;
    const QRegularExpression* userRePtr = nullptr;
    if (!customChapterRegex.empty()) {
        userRe = QRegularExpression(QString::fromUtf8(customChapterRegex.c_str()), QRegularExpression::UseUnicodePropertiesOption);
        if (userRe.isValid()) userRePtr = &userRe;
    }

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

        if (matchesChapterHeading(line, userRePtr)) {
            if (!curParas.empty()) {
                chapters.push_back(Chapter{curTitle, std::move(curParas)});
                curParas.clear();
            }
            curTitle = line.toUtf8().toStdString();
            hasHeading = true;
        } else {
            const QString norm = normalizeParagraphQ(line);
            if (!norm.isEmpty() && (!cleanAds || !isWebNovelAdJunkLine(norm))) {
                curParas.push_back(norm.toUtf8().toStdString());
            }
        }
    }

    if (!curParas.empty()) {
        chapters.push_back(Chapter{curTitle, std::move(curParas)});
    }
    if (chapters.empty() && !hasHeading) {
        return splitTextIntoChapters(html.toUtf8().toStdString(), true, customChapterRegex, cleanAds);
    }
    return chapters;
}

BookDocument NovelEngine::parseNovelToDocument(
    const std::filesystem::path& inPath,
    const std::string& customChapterRegex,
    bool cleanAds,
    const std::string& zhConvertMode) {

    std::string ext = omni::core::pathToUtf8(inPath.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    BookDocument doc;
    if (ext == ".txt") {
        const std::string txt = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
        doc = BookDocument{cleanBookTitleFromStem(inPath), "佚名",
                           splitTextIntoChapters(txt, true, customChapterRegex, cleanAds), {}};
    } else if (ext == ".epub") {
        doc = parseEpubFile(inPath, customChapterRegex, cleanAds);
    } else if (ext == ".mobi" || ext == ".azw" || ext == ".azw3") {
        doc = parseMobiNative(inPath, customChapterRegex, cleanAds);
    } else if (ext == ".pdf") {
        doc = parsePdfFile(inPath, customChapterRegex, cleanAds);
    } else if (ext == ".docx") {
        doc = parseDocxFile(inPath, customChapterRegex, cleanAds);
    } else if (ext == ".fb2") {
        doc = parseFb2File(inPath, cleanAds);
    } else if (ext == ".md") {
        doc = parseMdFile(inPath, customChapterRegex, cleanAds);
    } else if (ext == ".html" || ext == ".htm") {
        const std::string htmlStr = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
        std::string title = cleanBookTitleFromStem(inPath);
        static const QRegularExpression titleRe(
            QStringLiteral(R"(<title[^>]*>(.*?)</title>)"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        if (const auto m = titleRe.match(QString::fromUtf8(htmlStr.c_str())); m.hasMatch()) {
            const QString qt = decodeHtmlEntities(m.captured(1)).simplified();
            if (!qt.isEmpty()) title = qt.toUtf8().toStdString();
        }
        doc = BookDocument{title, "佚名", extractChaptersFromHtml(htmlStr, "正文", customChapterRegex, cleanAds), {}};
    } else {
        throw std::runtime_error("暂不支持读取的小说格式: " + ext);
    }

    if (zhConvertMode == "t2s" || zhConvertMode == "s2t") {
        doc.title = convertChineseVariant(doc.title, zhConvertMode);
        doc.author = convertChineseVariant(doc.author, zhConvertMode);
        for (auto& ch : doc.chapters) {
            ch.title = convertChineseVariant(ch.title, zhConvertMode);
            for (auto& p : ch.paragraphs) {
                p = convertChineseVariant(p, zhConvertMode);
            }
        }
    }
    return doc;
}

void NovelEngine::writeBookToEpub(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style) {
    omni::infra::ZipWriter zip;

    zip.addTextFile("mimetype", "application/epub+zip", true);

    zip.addTextFile("META-INF/container.xml",
                    R"(<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
)");

    const int indentEm = style.autoIndent ? std::clamp(style.indentSpaces, 0, 8) : 0;
    const double lh = std::clamp(style.lineHeight, 1.15, 3.0);
    const double fsScale = std::clamp(style.fontSizePt / 11.5, 0.75, 1.85);

    std::ostringstream css;
    css << "body {\n"
        << "  font-family: \"PingFang SC\", \"Songti SC\", \"Microsoft YaHei\", serif;\n"
        << "  font-size: " << QString::number(fsScale, 'f', 2).toStdString() << "em;\n"
        << "  line-height: " << QString::number(lh, 'f', 2).toStdString() << ";\n"
        << "  margin: 4%;\n"
        << "  color: #1f2328;\n"
        << "}\n"
        << "h1 {\n"
        << "  font-size: 1.42em;\n"
        << "  margin: 1.1em 0 0.9em 0;\n"
        << "  padding-bottom: 0.35em;\n"
        << "  border-bottom: 1px solid #e5e7eb;\n"
        << "  text-align: center;\n"
        << "}\n"
        << "p {\n"
        << "  text-indent: " << indentEm << "em;\n"
        << "  margin: 0.52em 0;\n"
        << "  text-align: justify;\n"
        << "}\n"
        << ".cover-wrap { text-align: center; margin: 0; padding: 0; }\n"
        << ".cover-wrap img { max-width: 100%; max-height: 96vh; object-fit: contain; }\n";
    zip.addTextFile("OEBPS/style/main.css", css.str());

    const std::string bookUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const std::string escTitle = xmlEscape(doc.title);
    const std::string escAuthor = xmlEscape(doc.author.empty() ? "佚名" : doc.author);
    const bool hasCover = !doc.coverJpegBytes.isEmpty();

    std::ostringstream manifestItems;
    std::ostringstream spineItems;
    std::ostringstream ncxPoints;
    std::ostringstream navLinks;

    if (hasCover) {
        std::vector<std::uint8_t> covVec(doc.coverJpegBytes.begin(), doc.coverJpegBytes.end());
        zip.addFile("OEBPS/images/cover.jpg", std::move(covVec), true);

        std::ostringstream covXhtml;
        covXhtml << R"(<?xml version="1.0" encoding="utf-8"?>)" << "\n"
                 << R"(<!DOCTYPE html>)" << "\n"
                 << R"(<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="zh" lang="zh">)" << "\n"
                 << "<head><meta charset=\"utf-8\"/><title>封面</title>"
                 << R"(<link rel="stylesheet" type="text/css" href="style/main.css"/></head>)" << "\n"
                 << "<body><div class=\"cover-wrap\"><img src=\"images/cover.jpg\" alt=\"" << escTitle << "\"/></div></body></html>\n";
        zip.addTextFile("OEBPS/cover.xhtml", covXhtml.str());

        manifestItems << "    <item id=\"cover-image\" href=\"images/cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>\n"
                      << "    <item id=\"cover-page\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
        spineItems << "    <itemref idref=\"cover-page\"/>\n";
    }

    spineItems << "    <itemref idref=\"nav\"/>\n";

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
        << "    <meta property=\"dcterms:modified\">" << modifiedUtc << "</meta>\n";
    if (hasCover) {
        opf << "    <meta name=\"cover\" content=\"cover-image\"/>\n";
    }
    opf << "  </metadata>\n"
        << "  <manifest>\n"
        << "    <item id=\"style_main\" href=\"style/main.css\" media-type=\"text/css\"/>\n"
        << "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
        << "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
        << manifestItems.str()
        << "  </manifest>\n"
        << "  <spine toc=\"ncx\">\n"
        << spineItems.str()
        << "  </spine>\n"
        << "</package>\n";
    zip.addTextFile("OEBPS/content.opf", opf.str());

    zip.writeToFile(outPath);
}

void NovelEngine::writeBookToTxt(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style) {
    const std::string indentPrefix = makeIndentSpaces(style, true);
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
    const auto encoded = omni::infra::TextEncoding::encodeFromUtf8(
        ss.str(), style.txtEncoding.empty() ? "utf-8" : style.txtEncoding);
    omni::infra::AtomicFileTx::writeBytesAtomic(outPath, encoded);
}

void NovelEngine::writeBookToMobiNative(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style) {
    const std::string indentStr = makeIndentSpaces(style, true);
    const std::string authorStr = doc.author.empty() ? "佚名" : doc.author;

    std::ostringstream html;
    html << "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"/>"
         << "<title>" << xmlEscape(doc.title) << "</title></head><body>"
         << "<h1>" << xmlEscape(doc.title) << "</h1><p>作者：" << xmlEscape(authorStr) << "</p><mbp:pagebreak/>";
    for (const auto& ch : doc.chapters) {
        html << "<h2>" << xmlEscape(ch.title) << "</h2>";
        for (const auto& p : ch.paragraphs) {
            html << "<p>" << xmlEscape(indentStr + p) << "</p>";
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
    const bool hasCover = !doc.coverJpegBytes.isEmpty();

    const std::string titleUtf8 = doc.title;
    const std::string langStr = "zh-CN";

    std::vector<std::uint8_t> exthBody;
    std::uint32_t exthCount = 0;
    auto appendExthString = [&](std::uint32_t typeId, const std::string& val) {
        writeBe32(exthBody, typeId);
        writeBe32(exthBody, static_cast<std::uint32_t>(8 + val.size()));
        exthBody.insert(exthBody.end(), val.begin(), val.end());
        ++exthCount;
    };
    auto appendExthU32 = [&](std::uint32_t typeId, std::uint32_t val) {
        writeBe32(exthBody, typeId);
        writeBe32(exthBody, 12);
        writeBe32(exthBody, val);
        ++exthCount;
    };

    appendExthString(100, authorStr);
    appendExthString(503, titleUtf8);
    appendExthString(524, langStr);
    if (hasCover) {
        appendExthU32(201, 0); // CoverOffset relative to firstImageIndex
        appendExthU32(202, 0); // ThumbOffset relative to firstImageIndex
    }

    const std::uint32_t exthUnpaddedLen = static_cast<std::uint32_t>(12 + exthBody.size());
    const std::size_t exthPad = (4 - (exthUnpaddedLen % 4)) % 4;
    std::vector<std::uint8_t> exthHeader = {'E', 'X', 'T', 'H'};
    writeBe32(exthHeader, exthUnpaddedLen);
    writeBe32(exthHeader, exthCount);
    exthHeader.insert(exthHeader.end(), exthBody.begin(), exthBody.end());
    exthHeader.insert(exthHeader.end(), exthPad, 0x00);

    std::vector<std::uint8_t> palmdocHdr;
    writeBe16(palmdocHdr, 1);
    writeBe16(palmdocHdr, 0);
    writeBe32(palmdocHdr, textLength);
    writeBe16(palmdocHdr, numTextRecords);
    writeBe16(palmdocHdr, 4096);
    writeBe16(palmdocHdr, 0);
    writeBe16(palmdocHdr, 0);

    const std::uint32_t fullNameOffset = static_cast<std::uint32_t>(16 + 232 + exthHeader.size());
    const std::uint32_t firstNonBookRec = static_cast<std::uint32_t>(numTextRecords) + 1u;
    const std::uint32_t firstImageRec = hasCover ? firstNonBookRec : 0xFFFFFFFFu;
    const std::uint32_t flisRecNum = hasCover ? (firstNonBookRec + 1u) : firstNonBookRec;
    const std::uint32_t fcisRecNum = flisRecNum + 1u;
    const std::uint16_t lastContentRec = hasCover
                                             ? static_cast<std::uint16_t>(numTextRecords + 1)
                                             : numTextRecords;

    std::vector<std::uint8_t> mobiHdr(232, 0x00);
    mobiHdr[0] = 'M'; mobiHdr[1] = 'O'; mobiHdr[2] = 'B'; mobiHdr[3] = 'I';
    putBe32At(mobiHdr, 4, 232);
    putBe32At(mobiHdr, 8, 2);
    putBe32At(mobiHdr, 12, 65001);
    putBe32At(mobiHdr, 16, QRandomGenerator::global()->bounded(1u, 0x7FFFFFFFu));
    putBe32At(mobiHdr, 20, 6);
    std::fill(mobiHdr.begin() + 24, mobiHdr.begin() + 64, 0xFF);
    putBe32At(mobiHdr, 64, firstNonBookRec);
    putBe32At(mobiHdr, 68, fullNameOffset);
    putBe32At(mobiHdr, 72, static_cast<std::uint32_t>(titleUtf8.size()));
    putBe32At(mobiHdr, 76, 0x0804);
    putBe32At(mobiHdr, 88, 6);
    putBe32At(mobiHdr, 92, hasCover ? firstImageRec : flisRecNum);
    putBe32At(mobiHdr, 112, 0x40);
    putBe32At(mobiHdr, 148, 0xFFFFFFFFu);
    putBe32At(mobiHdr, 152, 0xFFFFFFFFu);
    putBe16At(mobiHdr, 176, 1);
    putBe16At(mobiHdr, 178, lastContentRec);
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
    allRecords.reserve(textRecords.size() + 5);
    allRecords.push_back(std::move(rec0));
    for (auto& tr : textRecords) allRecords.push_back(std::move(tr));
    if (hasCover) {
        allRecords.emplace_back(doc.coverJpegBytes.begin(), doc.coverJpegBytes.end());
    }
    allRecords.push_back(flisRecord);
    allRecords.push_back(fcisRecord);
    allRecords.push_back(eofRecord);

    const auto totalRecs = static_cast<std::uint16_t>(allRecords.size());

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
        pdb.push_back(0x00);
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

void NovelEngine::writeBookToMobi(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style) {
    if (const auto calibre = omni::infra::PortableEnv::resolveCalibreConvert()) {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            const auto tmpEpub = omni::core::qStringToPath(tmpDir.filePath(QStringLiteral("source.epub")));
            writeBookToEpub(doc, tmpEpub, style);
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
    writeBookToMobiNative(doc, outPath, style);
}

void NovelEngine::writeBookToPdf(
    const BookDocument& doc,
    const std::filesystem::path& outPath,
    const NovelStyleConfig& style,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    std::lock_guard<std::mutex> lk(gPdfRenderMutex);
    omni::infra::AtomicFileTx tx(outPath);

    QPdfWriter pdfWriter(omni::core::pathToQString(tx.tempPath()));
    pdfWriter.setResolution(72);
    pdfWriter.setTitle(QString::fromUtf8(doc.title.c_str()));
    pdfWriter.setCreator(QStringLiteral("OmniConvert"));

    QPageSize pageSize(QPageSize::A4);
    double marginPt = 48.0;
    if (style.pdfPreset == "eink6") {
        pageSize = QPageSize(QSizeF(288.0, 412.0), QPageSize::Point);
        marginPt = 20.0;
    } else if (style.pdfPreset == "b5") {
        pageSize = QPageSize(QPageSize::JisB5);
        marginPt = 38.0;
    }
    pdfWriter.setPageSize(pageSize);
    pdfWriter.setPageMargins(QMarginsF(marginPt, marginPt, marginPt, marginPt), QPageLayout::Point);

    QPainter painter(&pdfWriter);
    if (!painter.isActive()) {
        throw std::runtime_error("初始化 PDF 渲染引擎失败");
    }
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    const QRectF paintRect = painter.viewport();
    const QSizeF contentSize(paintRect.width(), paintRect.height());
    const double fsPt = std::clamp(style.fontSizePt, 8.5, 24.0);
    const int lhPct = std::clamp(static_cast<int>(std::lround(style.lineHeight * 100.0)), 115, 280);
    const double indentPt = style.autoIndent ? (fsPt * std::clamp(style.indentSpaces, 0, 8)) : 0.0;

    bool firstPage = true;

    // Render Cover Page on Page 1 if cover image is provided
    if (!doc.coverJpegBytes.isEmpty()) {
        QImage covImg;
        if (covImg.loadFromData(doc.coverJpegBytes)) {
            QSizeF scaled = QSizeF(covImg.size()).scaled(contentSize, Qt::KeepAspectRatio);
            const QRectF dst(
                (contentSize.width() - scaled.width()) / 2.0,
                (contentSize.height() - scaled.height()) / 2.0,
                scaled.width(),
                scaled.height());
            painter.drawImage(dst, covImg);
            firstPage = false;
        }
    }

    const std::size_t totalChapters = std::max<std::size_t>(1, doc.chapters.size());

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
                 << "body { font-family: 'PingFang SC', 'Songti SC', 'Microsoft YaHei', sans-serif; "
                 << "font-size: " << QString::number(fsPt, 'f', 1).toStdString() << "pt; "
                 << "line-height: " << lhPct << "%; color: #1c1e21; }"
                 << "h1 { font-size: " << QString::number(fsPt * 1.45, 'f', 1).toStdString() << "pt; text-align: center; margin-bottom: 12pt; }"
                 << "h2 { font-size: " << QString::number(fsPt * 1.24, 'f', 1).toStdString() << "pt; margin-top: 6pt; margin-bottom: 10pt; }"
                 << "p { text-indent: " << QString::number(indentPt, 'f', 1).toStdString() << "pt; margin-top: 0pt; margin-bottom: 6pt; }"
                 << "</style></head><body>";
        if (idx == 0 && doc.coverJpegBytes.isEmpty()) {
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

    // Preserve lossless UTF-8 chapter text attachment for cross-platform roundtrip reading
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

void NovelEngine::writeBookToDocx(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style) {
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

    const int halfPts = std::clamp(static_cast<int>(std::lround(style.fontSizePt * 2.0)), 16, 48);
    std::ostringstream stylesXml;
    stylesXml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
              << "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\n"
              << "  <w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">\n"
              << "    <w:name w:val=\"Normal\"/>\n"
              << "    <w:rPr><w:sz w:val=\"" << halfPts << "\"/></w:rPr>\n"
              << "  </w:style>\n"
              << "  <w:style w:type=\"paragraph\" w:styleId=\"Title\">\n"
              << "    <w:name w:val=\"Title\"/>\n"
              << "    <w:pPr><w:jc w:val=\"center\"/><w:spacing w:after=\"240\"/></w:pPr>\n"
              << "    <w:rPr><w:b/><w:sz w:val=\"" << (halfPts + 18) << "\"/></w:rPr>\n"
              << "  </w:style>\n"
              << "  <w:style w:type=\"paragraph\" w:styleId=\"Heading1\">\n"
              << "    <w:name w:val=\"heading 1\"/>\n"
              << "    <w:pPr><w:outlineLvl w:val=\"0\"/><w:spacing w:before=\"280\" w:after=\"160\"/></w:pPr>\n"
              << "    <w:rPr><w:b/><w:sz w:val=\"" << (halfPts + 8) << "\"/></w:rPr>\n"
              << "  </w:style>\n"
              << "</w:styles>\n";
    zip.addTextFile("word/styles.xml", stylesXml.str());

    const std::string indentPrefix = makeIndentSpaces(style, true);
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

void NovelEngine::writeBookToHtml(const BookDocument& doc, const std::filesystem::path& outPath, const NovelStyleConfig& style) {
    const int indentEm = style.autoIndent ? std::clamp(style.indentSpaces, 0, 8) : 0;
    const int initPx = std::clamp(static_cast<int>(std::lround(style.fontSizePt * 1.55)), 14, 32);
    const double lh = std::clamp(style.lineHeight, 1.2, 2.8);
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

    std::string coverImgTag;
    if (!doc.coverJpegBytes.isEmpty()) {
        coverImgTag = "<img class=\"book-cover\" src=\"data:image/jpeg;base64," +
                      doc.coverJpegBytes.toBase64().toStdString() + "\" alt=\"封面\"/>\n";
    }

    std::ostringstream page;
    page << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
         << "<meta charset=\"utf-8\"/>\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
         << "<title>" << escTitle << "</title>\n<style>\n"
         << ":root { --bg:#f7f5f0; --card:#ffffff; --fg:#22252a; --muted:#6b7280; --border:#e5e2dc; --accent:#2563eb; --fs:" << initPx << "px; }\n"
         << "body.sepia { --bg:#f4ecd8; --card:#fbf5e6; --fg:#3b2f24; --muted:#7a6855; --border:#e3d5b8; --accent:#9a5b13; }\n"
         << "body.dark { --bg:#121417; --card:#1b1f24; --fg:#d9dee5; --muted:#8b949e; --border:#2d333b; --accent:#58a6ff; }\n"
         << "* { box-sizing: border-box; }\n"
         << "body { margin:0; background:var(--bg); color:var(--fg); font-family:\"PingFang SC\",\"Microsoft YaHei\",serif; display:flex; min-height:100vh; transition:background .2s,color .2s; }\n"
         << "aside { width:280px; background:var(--card); border-right:1px solid var(--border); height:100vh; position:sticky; top:0; overflow-y:auto; padding:18px 14px; flex-shrink:0; }\n"
         << "aside.hidden { display:none; }\n"
         << ".book-cover { width:100%; max-width:176px; display:block; margin:0 auto 12px auto; border-radius:8px; box-shadow:0 6px 16px rgba(0,0,0,.18); }\n"
         << ".book-meta h1 { font-size:18px; margin:0 0 6px 0; text-align:center; }\n"
         << ".book-meta p { font-size:13px; color:var(--muted); margin:0 0 14px 0; text-align:center; }\n"
         << ".toc-item { display:block; padding:7px 10px; color:var(--fg); text-decoration:none; border-radius:6px; font-size:14px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }\n"
         << ".toc-item:hover { background:rgba(128,128,128,0.12); color:var(--accent); }\n"
         << "main { flex:1; padding:24px 20px 80px; max-width:860px; margin:0 auto; }\n"
         << ".toolbar { position:sticky; top:12px; background:var(--card); border:1px solid var(--border); border-radius:10px; padding:8px 14px; display:flex; gap:8px; align-items:center; margin-bottom:28px; box-shadow:0 4px 14px rgba(0,0,0,.05); flex-wrap:wrap; }\n"
         << ".toolbar button { border:1px solid var(--border); background:var(--bg); color:var(--fg); border-radius:6px; padding:5px 11px; cursor:pointer; font-size:13px; }\n"
         << ".chapter { background:var(--card); border:1px solid var(--border); border-radius:12px; padding:32px 36px; margin-bottom:24px; }\n"
         << ".chapter h2 { margin-top:0; padding-bottom:12px; border-bottom:1px solid var(--border); font-size:calc(var(--fs) * 1.3); }\n"
         << ".chapter p { font-size:var(--fs); line-height:" << QString::number(lh, 'f', 2).toStdString()
         << "; text-indent:" << indentEm << "em; margin:0.65em 0; text-align:justify; }\n"
         << "@media (max-width: 768px) { aside { position:fixed; z-index:20; } .chapter { padding:20px 18px; } }\n"
         << "</style>\n</head>\n<body class=\"sepia\">\n"
         << "<aside id=\"sidebar\">\n  <div class=\"book-meta\">\n    "
         << coverImgTag
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
         << "<script>\nlet fs = " << initPx << ";\nfunction chgFs(d) { fs = Math.min(34, Math.max(12, fs + d)); document.documentElement.style.setProperty('--fs', fs + 'px'); }\n</script>\n"
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

void NovelEngine::convertNovelWithOptions(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const omni::core::ConversionOptions& opts,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(10);

    BookDocument doc = parseNovelToDocument(
        inPath,
        opts.novCustomChapterRegex,
        opts.novCleanAds,
        opts.novZhConvert);

    const QString qCustomTitle = QString::fromUtf8(opts.novCustomTitle.c_str()).trimmed();
    if (!qCustomTitle.isEmpty()) {
        doc.title = qCustomTitle.toUtf8().toStdString();
    }
    const QString qCustomAuthor = QString::fromUtf8(opts.novCustomAuthor.c_str()).trimmed();
    if (!qCustomAuthor.isEmpty()) {
        doc.author = qCustomAuthor.toUtf8().toStdString();
    }

    // Custom cover image > Procedural cover > Existing parsed cover
    std::error_code ec;
    if (!opts.novCoverPath.empty() && std::filesystem::is_regular_file(opts.novCoverPath, ec)) {
        const QImage userCov = ImageEngine::loadAnyImage(opts.novCoverPath);
        if (!userCov.isNull()) {
            doc.coverJpegBytes = prepareCoverJpeg(userCov);
        }
    } else if (opts.novAutoGenerateCover && doc.coverJpegBytes.isEmpty()) {
        const QImage genCov = generateProceduralCover(doc.title, doc.author);
        doc.coverJpegBytes = prepareCoverJpeg(genCov);
    }

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(45);

    NovelStyleConfig style;
    style.autoIndent = opts.novAutoIndent;
    style.indentSpaces = opts.novIndentSpaces;
    style.fontSizePt = opts.novFontSizePt;
    style.lineHeight = opts.novLineHeight;
    style.pdfPreset = opts.novPdfPreset;
    style.txtEncoding = opts.novTxtEncoding;

    std::string fmt = opts.targetFormat;
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (fmt == "epub") {
        writeBookToEpub(doc, outPath, style);
    } else if (fmt == "txt") {
        writeBookToTxt(doc, outPath, style);
    } else if (fmt == "mobi") {
        writeBookToMobi(doc, outPath, style);
    } else if (fmt == "pdf") {
        writeBookToPdf(doc, outPath, style, progressCb, isCancelled);
    } else if (fmt == "docx") {
        writeBookToDocx(doc, outPath, style);
    } else if (fmt == "html") {
        writeBookToHtml(doc, outPath, style);
    } else if (fmt == "md") {
        writeBookToMd(doc, outPath);
    } else if (fmt == "fb2") {
        writeBookToFb2(doc, outPath);
    } else {
        throw std::runtime_error("不支持的小说输出格式: " + opts.targetFormat);
    }

    if (progressCb) progressCb(100);
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

    omni::core::ConversionOptions opts;
    opts.targetFormat = targetFormat;
    opts.novAutoIndent = autoIndent;
    opts.novIndentSpaces = autoIndent ? 2 : 0;
    opts.novCustomAuthor = customAuthor;
    opts.novTxtEncoding = txtEncoding;
    opts.novPdfPreset = pdfPreset;
    opts.novCleanAds = false;
    convertNovelWithOptions(inPath, outPath, opts, progressCb, isCancelled);
}

} // namespace omni::engines
