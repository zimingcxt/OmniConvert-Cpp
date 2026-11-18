#include "omni/engines/document_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/engines/media_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/portable_env.hpp"
#include "omni/infra/text_encoding.hpp"

#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QString>

#ifdef OMNI_HAVE_QTPDF
#include <QPdfDocument>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace omni::engines {

namespace {

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

// Pure C++ cross-platform classic PDF object merger (preserves vector streams, fonts, and images)
bool mergeClassicPdfsCrossPlatform(
    const std::vector<std::filesystem::path>& pdfPaths,
    const std::filesystem::path& outPdfPath,
    const std::function<void(std::size_t index, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    struct PdfObjRecord {
        std::uint32_t newId = 0;
        std::string bodyBytes;
    };

    std::vector<PdfObjRecord> mergedObjs;
    std::vector<std::uint32_t> leafPageIds;
    std::uint32_t nextBaseId = 3; // 1 = Catalog, 2 = Root Pages

    static const QRegularExpression objHeaderRe(QStringLiteral(R"((?:^|[\r\n])\s*(\d+)\s+(\d+)\s+obj\b)"));
    static const QRegularExpression refRe(QStringLiteral(R"(\b(\d+)\s+(\d+)\s+R\b)"));
    static const QRegularExpression leafPageTypeRe(QStringLiteral(R"(/Type\s*/Page(?![A-Za-z]))"));
    static const QRegularExpression catalogTypeRe(QStringLiteral(R"(/Type\s*/Catalog\b)"));
    static const QRegularExpression pagesNodeRe(QStringLiteral(R"(/Type\s*/Pages\b)"));
    static const QRegularExpression mediaBoxRe(QStringLiteral(R"(/MediaBox\s*\[[^\]]+\])"));
    static const QRegularExpression parentRefRe(QStringLiteral(R"(/Parent\s+\d+\s+\d+\s+R)"));

    for (std::size_t fileIdx = 0; fileIdx < pdfPaths.size(); ++fileIdx) {
        if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");

        QFile f(omni::core::pathToQString(pdfPaths[fileIdx]));
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QByteArray raw = f.readAll();
        f.close();
        if (raw.size() < 32 || !raw.startsWith("%PDF-")) return false;

        const std::string bin(raw.constData(), static_cast<std::size_t>(raw.size()));

        struct RawObj {
            std::uint32_t oldId = 0;
            std::string dictPart;
            std::string streamPart; // includes "stream...endstream" if present
        };

        std::vector<RawObj> fileObjs;
        std::uint32_t maxOldId = 0;
        std::string inheritedMediaBox;

        // Scan top-level objects while skipping binary stream interiors
        std::size_t scanPos = 0;
        const QString latinView = QString::fromLatin1(bin.data(), static_cast<qsizetype>(bin.size()));

        while (scanPos < bin.size()) {
            const auto m = objHeaderRe.match(latinView, static_cast<qsizetype>(scanPos));
            if (!m.hasMatch()) break;

            const std::uint32_t oldId = m.captured(1).toUInt();
            const std::size_t bodyStart = static_cast<std::size_t>(m.capturedEnd());

            // Check if there is a "stream" keyword before the next "endobj"
            const std::size_t nextEndObj = bin.find("endobj", bodyStart);
            if (nextEndObj == std::string::npos) break;

            std::size_t streamPos = bin.find("stream", bodyStart);
            // Verify "stream" is an actual stream keyword (followed by \n or \r\n) and before nextEndObj
            while (streamPos != std::string::npos && streamPos < nextEndObj) {
                const bool validBefore = (streamPos == bodyStart) ||
                                         !std::isalpha(static_cast<unsigned char>(bin[streamPos - 1]));
                const std::size_t after = streamPos + 6;
                const bool validAfter = (after < bin.size()) &&
                                        (bin[after] == '\n' || (bin[after] == '\r' && after + 1 < bin.size() && bin[after + 1] == '\n'));
                if (validBefore && validAfter) break;
                streamPos = bin.find("stream", streamPos + 6);
            }

            RawObj ro;
            ro.oldId = oldId;
            maxOldId = std::max(maxOldId, oldId);

            if (streamPos != std::string::npos && streamPos < nextEndObj) {
                ro.dictPart = bin.substr(bodyStart, streamPos - bodyStart);
                // Find endstream followed by endobj
                std::size_t endStreamPos = bin.find("endstream", streamPos + 6);
                std::size_t realEndObj = nextEndObj;
                while (endStreamPos != std::string::npos) {
                    const std::size_t eo = bin.find("endobj", endStreamPos + 9);
                    if (eo == std::string::npos) break;
                    // Check only whitespace between endstream and endobj
                    bool onlyWs = true;
                    for (std::size_t k = endStreamPos + 9; k < eo; ++k) {
                        if (!std::isspace(static_cast<unsigned char>(bin[k]))) {
                            onlyWs = false;
                            break;
                        }
                    }
                    if (onlyWs) {
                        realEndObj = eo;
                        break;
                    }
                    endStreamPos = bin.find("endstream", endStreamPos + 9);
                }
                if (endStreamPos == std::string::npos || realEndObj <= streamPos) {
                    scanPos = nextEndObj + 6;
                    continue;
                }
                ro.streamPart = bin.substr(streamPos, realEndObj - streamPos);
                scanPos = realEndObj + 6;
            } else {
                ro.dictPart = bin.substr(bodyStart, nextEndObj - bodyStart);
                scanPos = nextEndObj + 6;
            }

            const QString qDict = QString::fromLatin1(ro.dictPart.data(), static_cast<qsizetype>(ro.dictPart.size()));
            if (pagesNodeRe.match(qDict).hasMatch() && inheritedMediaBox.empty()) {
                const auto mb = mediaBoxRe.match(qDict);
                if (mb.hasMatch()) {
                    inheritedMediaBox = mb.captured(0).toLatin1().toStdString();
                }
            }
            fileObjs.push_back(std::move(ro));
        }

        if (fileObjs.empty()) return false;

        std::size_t fileLeafCount = 0;
        for (const auto& ro : fileObjs) {
            if (ro.oldId == 0) continue;
            QString qDict = QString::fromLatin1(ro.dictPart.data(), static_cast<qsizetype>(ro.dictPart.size()));

            if (catalogTypeRe.match(qDict).hasMatch() || pagesNodeRe.match(qDict).hasMatch()) {
                continue;
            }

            // Rewrite all indirect references "X Y R" -> "(X + nextBaseId) 0 R" inside dictionary part
            QString shifted;
            shifted.reserve(qDict.size() + 32);
            qsizetype lastPos = 0;
            auto it = refRe.globalMatch(qDict);
            while (it.hasNext()) {
                const auto m = it.next();
                shifted += qDict.mid(lastPos, m.capturedStart() - lastPos);
                const std::uint32_t targetOld = m.captured(1).toUInt();
                shifted += QString::number(targetOld + nextBaseId) + QStringLiteral(" 0 R");
                lastPos = m.capturedEnd();
            }
            shifted += qDict.mid(lastPos);

            const std::uint32_t newObjId = ro.oldId + nextBaseId;
            if (leafPageTypeRe.match(shifted).hasMatch()) {
                shifted.replace(parentRefRe, QStringLiteral("/Parent 2 0 R"));
                if (!mediaBoxRe.match(shifted).hasMatch() && !inheritedMediaBox.empty()) {
                    const qsizetype typeIdx = shifted.indexOf(QStringLiteral("/Type"));
                    if (typeIdx >= 0) {
                        shifted.insert(typeIdx, QString::fromLatin1(inheritedMediaBox.c_str()) + QStringLiteral(" "));
                    }
                }
                leafPageIds.push_back(newObjId);
                ++fileLeafCount;
            }

            PdfObjRecord rec;
            rec.newId = newObjId;
            const QByteArray dictLatin = shifted.toLatin1();
            rec.bodyBytes.reserve(static_cast<std::size_t>(dictLatin.size()) + ro.streamPart.size() + 8);
            rec.bodyBytes.append(dictLatin.constData(), static_cast<std::size_t>(dictLatin.size()));
            rec.bodyBytes.append(ro.streamPart);
            mergedObjs.push_back(std::move(rec));
        }

        if (fileLeafCount == 0) return false;
        nextBaseId += maxOldId + 1;
        if (itemProgressCb) itemProgressCb(fileIdx, 90);
    }

    if (leafPageIds.empty()) return false;

    const std::uint32_t maxObjId = nextBaseId - 1;
    std::map<std::uint32_t, std::size_t> xrefOffsets;

    std::string outBin;
    outBin.reserve(65536);
    outBin += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

    // Object 1: Catalog
    xrefOffsets[1] = outBin.size();
    outBin += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    // Object 2: Master Pages tree
    xrefOffsets[2] = outBin.size();
    {
        std::ostringstream kids;
        kids << "2 0 obj\n<< /Type /Pages /Kids [ ";
        for (std::uint32_t pid : leafPageIds) {
            kids << pid << " 0 R ";
        }
        kids << "] /Count " << leafPageIds.size() << " >>\nendobj\n";
        outBin += kids.str();
    }

    for (const auto& obj : mergedObjs) {
        xrefOffsets[obj.newId] = outBin.size();
        outBin += std::to_string(obj.newId) + " 0 obj";
        if (obj.bodyBytes.empty() || (obj.bodyBytes[0] != '\n' && obj.bodyBytes[0] != '\r' && obj.bodyBytes[0] != ' ')) {
            outBin.push_back('\n');
        }
        outBin += obj.bodyBytes;
        if (outBin.back() != '\n' && outBin.back() != '\r') {
            outBin.push_back('\n');
        }
        outBin += "endobj\n";
    }

    const std::size_t xrefPos = outBin.size();
    outBin += "xref\n0 " + std::to_string(maxObjId + 1) + "\n";
    outBin += "0000000000 65535 f \n";
    for (std::uint32_t id = 1; id <= maxObjId; ++id) {
        char line[32];
        auto it = xrefOffsets.find(id);
        if (it != xrefOffsets.end()) {
            std::snprintf(line, sizeof(line), "%010zu 00000 n \n", it->second);
        } else {
            std::snprintf(line, sizeof(line), "0000000000 65535 f \n");
        }
        outBin += line;
    }

    outBin += "trailer\n<< /Size " + std::to_string(maxObjId + 1) + " /Root 1 0 R >>\n";
    outBin += "startxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";

    const std::vector<std::uint8_t> outVec(outBin.begin(), outBin.end());
    omni::infra::AtomicFileTx::writeBytesAtomic(outPdfPath, outVec);
    for (std::size_t i = 0; i < pdfPaths.size(); ++i) {
        if (itemProgressCb) itemProgressCb(i, 100);
    }
    return true;
}

// RFC-4180 CSV / TSV parser
std::vector<std::vector<std::string>> parseDelimitedTable(const std::string& utf8Str, char delim) {
    std::string s = utf8Str;
    // Strip UTF-8 BOM if present
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }

    // Auto-detect semicolon vs comma if delim == ','
    if (delim == ',') {
        const std::size_t firstNl = s.find('\n');
        const std::string firstLine = s.substr(0, firstNl);
        const auto commas = std::count(firstLine.begin(), firstLine.end(), ',');
        const auto semis = std::count(firstLine.begin(), firstLine.end(), ';');
        const auto tabs = std::count(firstLine.begin(), firstLine.end(), '\t');
        if (tabs > commas && tabs > semis) delim = '\t';
        else if (semis > commas && commas == 0) delim = ';';
    }

    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> curRow;
    std::string cell;
    bool inQuotes = false;

    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cell.push_back(c);
            }
        } else {
            if (c == '"' && cell.empty()) {
                inQuotes = true;
            } else if (c == delim) {
                curRow.push_back(std::move(cell));
                cell.clear();
            } else if (c == '\r') {
                if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
                curRow.push_back(std::move(cell));
                cell.clear();
                if (!(curRow.size() == 1 && curRow[0].empty())) {
                    rows.push_back(std::move(curRow));
                }
                curRow.clear();
            } else if (c == '\n') {
                curRow.push_back(std::move(cell));
                cell.clear();
                if (!(curRow.size() == 1 && curRow[0].empty())) {
                    rows.push_back(std::move(curRow));
                }
                curRow.clear();
            } else {
                cell.push_back(c);
            }
        }
    }

    if (!cell.empty() || !curRow.empty()) {
        curRow.push_back(std::move(cell));
        if (!(curRow.size() == 1 && curRow[0].empty())) {
            rows.push_back(std::move(curRow));
        }
    }
    return rows;
}

std::vector<std::vector<std::string>> parseJsonTable(const std::string& utf8Str) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(utf8Str), &err);
    if (err.error != QJsonParseError::NoError) {
        throw std::runtime_error("JSON 语法解析失败: " + err.errorString().toUtf8().toStdString());
    }

    auto valToString = [](const QJsonValue& v) -> std::string {
        if (v.isNull() || v.isUndefined()) return "";
        if (v.isString()) return v.toString().toUtf8().toStdString();
        if (v.isBool()) return v.toBool() ? "true" : "false";
        if (v.isDouble()) {
            const double d = v.toDouble();
            if (std::floor(d) == d && std::abs(d) < 1e15) {
                return std::to_string(static_cast<long long>(d));
            }
            return QString::number(d, 'g', 12).toUtf8().toStdString();
        }
        if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact).toStdString();
        if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).toStdString();
        return "";
    };

    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        // Check if root object wraps an array property (e.g. {"data": [...]})
        const QJsonObject rootObj = doc.object();
        for (auto it = rootObj.begin(); it != rootObj.end(); ++it) {
            if (it.value().isArray()) {
                arr = it.value().toArray();
                break;
            }
        }
        if (arr.isEmpty()) {
            arr.append(rootObj);
        }
    }

    std::vector<std::vector<std::string>> rows;
    if (arr.isEmpty()) return rows;

    if (arr.first().isObject()) {
        QStringList headers;
        for (const QJsonValue& item : arr) {
            if (!item.isObject()) continue;
            const QJsonObject obj = item.toObject();
            for (const QString& k : obj.keys()) {
                if (!headers.contains(k)) headers.append(k);
            }
        }
        std::vector<std::string> hdrRow;
        for (const QString& h : headers) hdrRow.push_back(h.toUtf8().toStdString());
        rows.push_back(std::move(hdrRow));

        for (const QJsonValue& item : arr) {
            if (!item.isObject()) continue;
            const QJsonObject obj = item.toObject();
            std::vector<std::string> r;
            r.reserve(static_cast<std::size_t>(headers.size()));
            for (const QString& h : headers) {
                r.push_back(valToString(obj.value(h)));
            }
            rows.push_back(std::move(r));
        }
    } else if (arr.first().isArray()) {
        for (const QJsonValue& item : arr) {
            if (!item.isArray()) continue;
            const QJsonArray sub = item.toArray();
            std::vector<std::string> r;
            for (const QJsonValue& cell : sub) {
                r.push_back(valToString(cell));
            }
            rows.push_back(std::move(r));
        }
    } else {
        rows.push_back({"value"});
        for (const QJsonValue& item : arr) {
            rows.push_back({valToString(item)});
        }
    }
    return rows;
}

std::vector<std::vector<std::string>> parseMarkdownTable(const std::string& utf8Str) {
    std::vector<std::vector<std::string>> rows;
    const QString qText = QString::fromUtf8(utf8Str.c_str());
    static const QRegularExpression sepLineRe(QStringLiteral(R"(^\s*\|?(?:\s*:?-{2,}:?\s*\|)+\s*:?-{2,}:?\s*\|?\s*$)"));

    for (const QString& rawLine : qText.split('\n')) {
        QString line = rawLine.trimmed();
        if (!line.contains('|') || sepLineRe.match(line).hasMatch()) continue;
        if (line.startsWith('|')) line.remove(0, 1);
        if (line.endsWith('|')) line.chop(1);

        std::vector<std::string> r;
        for (const QString& part : line.split('|')) {
            r.push_back(part.trimmed().toUtf8().toStdString());
        }
        if (!r.empty()) rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<std::vector<std::string>> parseHtmlTable(const std::string& utf8Str) {
    std::vector<std::vector<std::string>> rows;
    const QString html = QString::fromUtf8(utf8Str.c_str());
    static const QRegularExpression trRe(
        QStringLiteral(R"(<tr\b[^>]*>(.*?)</tr>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cellRe(
        QStringLiteral(R"(<t[hd]\b[^>]*>(.*?)</t[hd]>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression stripTagRe(QStringLiteral(R"(<[^>]+>)"));

    auto trIt = trRe.globalMatch(html);
    while (trIt.hasNext()) {
        const QString rowInner = trIt.next().captured(1);
        std::vector<std::string> r;
        auto cellIt = cellRe.globalMatch(rowInner);
        while (cellIt.hasNext()) {
            QString val = cellIt.next().captured(1);
            val.remove(stripTagRe);
            val.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
            val.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
            val.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
            val.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
            val.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
            val.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            r.push_back(val.simplified().toUtf8().toStdString());
        }
        if (!r.empty()) rows.push_back(std::move(r));
    }
    return rows;
}

std::string formatDelimitedCell(const std::string& s, char delim) {
    bool needQuote = false;
    for (char c : s) {
        if (c == delim || c == '"' || c == '\n' || c == '\r') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

} // namespace

void DocumentEngine::mergeImagesToPdf(
    const std::vector<std::filesystem::path>& imagePaths,
    const std::filesystem::path& outPdfPath,
    const std::string& pagePreset,
    const std::function<void(std::size_t index, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (imagePaths.empty()) {
        throw std::runtime_error("请先添加至少一张待合成的图片");
    }

    omni::infra::AtomicFileTx tx(outPdfPath);
    QPdfWriter pdfWriter(omni::core::pathToQString(tx.tempPath()));
    pdfWriter.setResolution(72); // 1 unit == 1 PDF point (1/72 inch)
    pdfWriter.setCreator(QStringLiteral("OmniConvert Document Workshop"));

    const std::string preset = lowerAscii(pagePreset);

    auto configurePageForImage = [&](const QImage& img) -> double {
        if (preset == "a4") {
            pdfWriter.setPageSize(QPageSize(QPageSize::A4));
            pdfWriter.setPageMargins(QMarginsF(20, 20, 20, 20), QPageLayout::Point);
            return 20.0;
        }
        if (preset == "b5") {
            pdfWriter.setPageSize(QPageSize(QPageSize::JisB5));
            pdfWriter.setPageMargins(QMarginsF(18, 18, 18, 18), QPageLayout::Point);
            return 18.0;
        }
        // "fit": exact aspect-ratio page sized cleanly around 72..595 pt width with zero border
        double wPt = std::clamp(img.width() * 0.75, 180.0, 842.0);
        double hPt = wPt * static_cast<double>(img.height()) / std::max(1, img.width());
        pdfWriter.setPageSize(QPageSize(QSizeF(wPt, hPt), QPageSize::Point));
        pdfWriter.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
        return 0.0;
    };

    // Load first image BEFORE creating QPainter so Page 1 uses the right page size
    const QImage firstImg = ImageEngine::loadAnyImage(imagePaths[0]);
    if (firstImg.isNull()) {
        tx.rollback();
        throw std::runtime_error("无法读取首张图片: " + omni::core::pathToUtf8(imagePaths[0].filename()));
    }
    configurePageForImage(firstImg);

    QPainter painter(&pdfWriter);
    if (!painter.isActive()) {
        tx.rollback();
        throw std::runtime_error("初始化 PDF 合成画布失败");
    }
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

    for (std::size_t i = 0; i < imagePaths.size(); ++i) {
        if (isCancelled && isCancelled()) {
            painter.end();
            tx.rollback();
            throw std::runtime_error("CANCELLED");
        }

        const QImage img = (i == 0) ? firstImg : ImageEngine::loadAnyImage(imagePaths[i]);
        if (img.isNull()) {
            painter.end();
            tx.rollback();
            throw std::runtime_error("无法解码图片: " + omni::core::pathToUtf8(imagePaths[i].filename()));
        }

        if (i > 0) {
            configurePageForImage(img);
            pdfWriter.newPage();
        }

        const QRectF vp = painter.viewport();
        QSizeF scaledSize = QSizeF(img.size()).scaled(vp.size(), Qt::KeepAspectRatio);
        const QRectF targetRect(
            vp.left() + (vp.width() - scaledSize.width()) / 2.0,
            vp.top() + (vp.height() - scaledSize.height()) / 2.0,
            scaledSize.width(),
            scaledSize.height());

        // Flatten onto white if transparent
        if (img.hasAlphaChannel()) {
            painter.fillRect(targetRect, Qt::white);
        }
        painter.drawImage(targetRect, img);

        if (itemProgressCb) {
            itemProgressCb(i, 96);
        }
    }

    painter.end();
    tx.commit();
    for (std::size_t i = 0; i < imagePaths.size(); ++i) {
        if (itemProgressCb) itemProgressCb(i, 100);
    }
}

void DocumentEngine::mergePdfs(
    const std::vector<std::filesystem::path>& pdfPaths,
    const std::filesystem::path& outPdfPath,
    const std::function<void(std::size_t index, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (pdfPaths.empty()) {
        throw std::runtime_error("请先添加待合并的 PDF 文档");
    }

#ifdef __APPLE__
    if (macos_pdf::mergePdfFilesNative(pdfPaths, outPdfPath, itemProgressCb, isCancelled)) {
        return;
    }
#endif

    if (mergeClassicPdfsCrossPlatform(pdfPaths, outPdfPath, itemProgressCb, isCancelled)) {
        return;
    }

#ifdef OMNI_HAVE_QTPDF
    {
        omni::infra::AtomicFileTx tx(outPdfPath);
        QPdfWriter writer(omni::core::pathToQString(tx.tempPath()));
        writer.setResolution(150);
        QPainter painter(&writer);
        bool first = true;
        for (std::size_t i = 0; i < pdfPaths.size(); ++i) {
            if (isCancelled && isCancelled()) {
                painter.end();
                tx.rollback();
                throw std::runtime_error("CANCELLED");
            }
            QPdfDocument doc;
            if (doc.load(omni::core::pathToQString(pdfPaths[i])) != QPdfDocument::Error::None) {
                painter.end();
                tx.rollback();
                throw std::runtime_error("无法打开 PDF 文件: " + omni::core::pathToUtf8(pdfPaths[i].filename()));
            }
            for (int p = 0; p < doc.pageCount(); ++p) {
                const QSizeF ptSize = doc.pagePointSize(p);
                writer.setPageSize(QPageSize(ptSize, QPageSize::Point));
                writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
                if (!first) writer.newPage();
                first = false;
                const QSize pxSize(std::max(16, static_cast<int>(ptSize.width() * 150.0 / 72.0)),
                                   std::max(16, static_cast<int>(ptSize.height() * 150.0 / 72.0)));
                const QImage rendered = doc.render(p, pxSize);
                painter.drawImage(painter.viewport(), rendered);
            }
            if (itemProgressCb) itemProgressCb(i, 95);
        }
        painter.end();
        tx.commit();
        for (std::size_t i = 0; i < pdfPaths.size(); ++i) {
            if (itemProgressCb) itemProgressCb(i, 100);
        }
        return;
    }
#endif

    throw std::runtime_error("合并 PDF 失败（输入 PDF 可能受密码加密或采用非标交叉引用流）");
}

std::filesystem::path DocumentEngine::exportPdfToImages(
    const std::filesystem::path& inPdfPath,
    const std::filesystem::path& customOutDir,
    const std::string& imgFormat,
    int dpi,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    const std::filesystem::path baseDir = customOutDir.empty() ? inPdfPath.parent_path() : customOutDir;
    const std::string stem = omni::core::pathToUtf8(inPdfPath.stem());
    std::filesystem::path outFolder = baseDir / omni::core::pathFromUtf8(stem + "_pages");
    int suffix = 1;
    std::error_code ec;
    while (std::filesystem::exists(outFolder, ec) && !std::filesystem::is_empty(outFolder, ec)) {
        outFolder = baseDir / omni::core::pathFromUtf8(stem + "_pages_" + std::to_string(suffix++));
    }
    std::filesystem::create_directories(outFolder, ec);

    std::string fmt = lowerAscii(imgFormat);
    if (fmt != "png" && fmt != "jpg" && fmt != "jpeg" && fmt != "webp") {
        fmt = "png";
    }

#ifdef __APPLE__
    if (macos_pdf::renderPdfPagesNative(inPdfPath, outFolder, stem, fmt, dpi, progressCb, isCancelled)) {
        return outFolder;
    }
#endif

#ifdef OMNI_HAVE_QTPDF
    {
        QPdfDocument doc;
        if (doc.load(omni::core::pathToQString(inPdfPath)) == QPdfDocument::Error::None && doc.pageCount() > 0) {
            const double scale = std::clamp(dpi, 72, 600) / 72.0;
            const int total = doc.pageCount();
            for (int p = 0; p < total; ++p) {
                if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
                const QSizeF pt = doc.pagePointSize(p);
                const QSize px(std::max(16, static_cast<int>(std::lround(pt.width() * scale))),
                               std::max(16, static_cast<int>(std::lround(pt.height() * scale))));
                QImage rendered(px, QImage::Format_RGB32);
                rendered.fill(Qt::white);
                QPainter ptr(&rendered);
                ptr.drawImage(0, 0, doc.render(p, px));
                ptr.end();

                char nameBuf[64];
                std::snprintf(nameBuf, sizeof(nameBuf), "_page_%04d.%s", p + 1, fmt.c_str());
                const auto pagePath = outFolder / omni::core::pathFromUtf8(stem + nameBuf);
                QImageWriter w(omni::core::pathToQString(pagePath), (fmt == "jpg") ? "jpeg" : fmt.c_str());
                w.setQuality(90);
                w.write(rendered);
                if (progressCb) progressCb(std::min(99, (p + 1) * 98 / total));
            }
            if (progressCb) progressCb(100);
            return outFolder;
        }
    }
#endif

    throw std::runtime_error("当前环境缺少离线 PDF 光栅化组件（macOS 使用原生 PDFKit；Windows 需安装 QtPdf 模块）");
}

void DocumentEngine::convertTable(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(15);

    const std::string utf8Raw = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
    const std::string inExt = lowerAscii(omni::core::pathToUtf8(inPath.extension()));

    std::vector<std::vector<std::string>> rows;
    if (inExt == ".json") {
        rows = parseJsonTable(utf8Raw);
    } else if (inExt == ".tsv") {
        rows = parseDelimitedTable(utf8Raw, '\t');
    } else if (inExt == ".md" || inExt == ".markdown") {
        rows = parseMarkdownTable(utf8Raw);
    } else if (inExt == ".html" || inExt == ".htm") {
        rows = parseHtmlTable(utf8Raw);
    } else {
        rows = parseDelimitedTable(utf8Raw, ',');
    }

    if (rows.empty()) {
        throw std::runtime_error("未能从源文件中解析出有效的数据表格行");
    }

    // Normalize rectangular column count
    std::size_t colCount = 0;
    for (const auto& r : rows) colCount = std::max(colCount, r.size());
    for (auto& r : rows) {
        if (r.size() < colCount) r.resize(colCount);
    }

    if (progressCb) progressCb(65);
    const std::string fmt = lowerAscii(targetFormat);
    std::ostringstream out;

    if (fmt == "csv" || fmt == "tsv") {
        const char delim = (fmt == "tsv") ? '\t' : ',';
        if (fmt == "csv") {
            out << "\xEF\xBB\xBF"; // UTF-8 BOM prevents Excel GBK garbling
        }
        for (const auto& r : rows) {
            for (std::size_t c = 0; c < r.size(); ++c) {
                if (c > 0) out << delim;
                out << formatDelimitedCell(r[c], delim);
            }
            out << "\r\n";
        }
    } else if (fmt == "json") {
        QJsonArray arr;
        if (rows.size() >= 2) {
            const auto& hdr = rows[0];
            for (std::size_t rIdx = 1; rIdx < rows.size(); ++rIdx) {
                QJsonObject obj;
                for (std::size_t c = 0; c < colCount; ++c) {
                    const std::string keyStr = hdr[c].empty() ? ("col_" + std::to_string(c + 1)) : hdr[c];
                    const QString qVal = QString::fromUtf8(rows[rIdx][c].c_str());
                    bool okInt = false;
                    const qint64 iv = qVal.toLongLong(&okInt);
                    // Preserve leading-zero identifiers (like ID numbers / phone / zip codes) as strings!
                    const bool hasLeadingZero = (qVal.size() > 1 && qVal.startsWith('0') && !qVal.startsWith("0."));
                    if (okInt && !hasLeadingZero && qVal.size() <= 15) {
                        obj.insert(QString::fromUtf8(keyStr.c_str()), iv);
                    } else {
                        obj.insert(QString::fromUtf8(keyStr.c_str()), qVal);
                    }
                }
                arr.append(obj);
            }
        } else {
            for (const auto& r : rows) {
                QJsonArray sub;
                for (const auto& cell : r) sub.append(QString::fromUtf8(cell.c_str()));
                arr.append(sub);
            }
        }
        out << QJsonDocument(arr).toJson(QJsonDocument::Indented).toStdString();
    } else if (fmt == "md") {
        auto escMd = [](std::string s) {
            QString q = QString::fromUtf8(s.c_str());
            q.replace('|', QStringLiteral("\\|"));
            q.replace('\r', ' ');
            q.replace('\n', QStringLiteral("<br>"));
            return q.trimmed().toUtf8().toStdString();
        };
        for (std::size_t rIdx = 0; rIdx < rows.size(); ++rIdx) {
            out << "|";
            for (std::size_t c = 0; c < colCount; ++c) {
                out << " " << escMd(rows[rIdx][c]) << " |";
            }
            out << "\n";
            if (rIdx == 0) {
                out << "|";
                for (std::size_t c = 0; c < colCount; ++c) {
                    out << " --- |";
                }
                out << "\n";
            }
        }
    } else if (fmt == "html") {
        const std::string docTitle = htmlEscape(omni::core::pathToUtf8(inPath.stem()));
        out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\"/>\n"
            << "<title>" << docTitle << "</title>\n<style>\n"
            << "body{font-family:-apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;background:#f8fafc;color:#0f172a;margin:24px;}\n"
            << ".bar{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;gap:12px;flex-wrap:wrap;}\n"
            << "input{padding:8px 14px;border:1px solid #cbd5e1;border-radius:8px;min-width:260px;font-size:14px;}\n"
            << ".wrap{overflow:auto;background:#fff;border:1px solid #e2e8f0;border-radius:10px;box-shadow:0 2px 8px rgba(15,23,42,.04);}\n"
            << "table{border-collapse:collapse;width:100%;font-size:13.5px;}\n"
            << "th,td{padding:10px 14px;border-bottom:1px solid #e2e8f0;text-align:left;}\n"
            << "th{background:#f1f5f9;font-weight:600;position:sticky;top:0;}\n"
            << "tr:hover td{background:#f8fafc;}\n"
            << "</style>\n</head>\n<body>\n"
            << "<div class=\"bar\"><h2>" << docTitle << " （共 " << (rows.size() > 1 ? rows.size() - 1 : rows.size())
            << " 条记录）</h2><input id=\"q\" placeholder=\"🔍 输入关键词实时筛选行...\" oninput=\"flt()\"/></div>\n"
            << "<div class=\"wrap\"><table id=\"tbl\">\n<thead><tr>";
        for (const auto& h : rows[0]) {
            out << "<th>" << htmlEscape(h) << "</th>";
        }
        out << "</tr></thead>\n<tbody>\n";
        for (std::size_t rIdx = 1; rIdx < rows.size(); ++rIdx) {
            out << "<tr>";
            for (const auto& cell : rows[rIdx]) {
                out << "<td>" << htmlEscape(cell) << "</td>";
            }
            out << "</tr>\n";
        }
        out << "</tbody></table></div>\n"
            << "<script>function flt(){const v=document.getElementById('q').value.toLowerCase();"
            << "for(const tr of document.querySelectorAll('#tbl tbody tr')){"
            << "tr.style.display=tr.textContent.toLowerCase().includes(v)?'':'none';}}</script>\n"
            << "</body></html>\n";
    } else {
        throw std::runtime_error("不支持的数据表格输出格式: " + targetFormat);
    }

    omni::infra::AtomicFileTx::writeStringAtomic(outPath, out.str());
    if (progressCb) progressCb(100);
}

} // namespace omni::engines
