#include "omni/engines/toolbox_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/text_encoding.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace omni::engines {

namespace {

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

QByteArray encodeImageToBuffer(const QImage& img, const std::string& fmtLower, int quality) {
    QByteArray outBytes;
    QBuffer buffer(&outBytes);
    buffer.open(QIODevice::WriteOnly);

    QByteArray qtFmt = "jpeg";
    if (fmtLower == "webp" && QImageWriter::supportedImageFormats().contains("webp")) {
        qtFmt = "webp";
    } else if (fmtLower == "png") {
        qtFmt = "png";
    }

    QImage prepared = img;
    if (qtFmt == "jpeg" && prepared.hasAlphaChannel()) {
        QImage flat(prepared.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, prepared);
        p.end();
        prepared = flat.convertToFormat(QImage::Format_RGB888);
    }

    QImageWriter writer(&buffer, qtFmt);
    writer.setQuality(std::clamp(quality, 1, 100));
    writer.write(prepared);
    buffer.close();
    return outBytes;
}

struct SubtitleCue {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    QString text;
};

std::int64_t parseSrtVttTimestampMs(const QString& hh, const QString& mm, const QString& ss, const QString& ms) {
    const std::int64_t h = hh.isEmpty() ? 0 : hh.toLongLong();
    const std::int64_t m = mm.toLongLong();
    const std::int64_t s = ss.toLongLong();
    QString msNorm = ms;
    while (msNorm.size() < 3) msNorm.append('0');
    if (msNorm.size() > 3) msNorm = msNorm.left(3);
    return ((h * 3600 + m * 60 + s) * 1000) + msNorm.toLongLong();
}

std::int64_t parseAssTimestampMs(const QString& rawTs) {
    static const QRegularExpression assTsRe(QStringLiteral(R"(^\s*(\d+):(\d{1,2}):(\d{1,2})[.:](\d{1,3})\s*$)"));
    const auto m = assTsRe.match(rawTs);
    if (!m.hasMatch()) return -1;
    const std::int64_t h = m.captured(1).toLongLong();
    const std::int64_t min = m.captured(2).toLongLong();
    const std::int64_t sec = m.captured(3).toLongLong();
    const QString frac = m.captured(4);
    std::int64_t ms = frac.toLongLong();
    if (frac.size() == 1) ms *= 100;
    else if (frac.size() == 2) ms *= 10;
    return ((h * 3600 + min * 60 + sec) * 1000) + ms;
}

std::string formatSrtTime(std::int64_t totalMs, char decimalSep) {
    totalMs = std::max<std::int64_t>(0, totalMs);
    const int ms = static_cast<int>(totalMs % 1000);
    const std::int64_t totalSec = totalMs / 1000;
    const int s = static_cast<int>(totalSec % 60);
    const int m = static_cast<int>((totalSec / 60) % 60);
    const int h = static_cast<int>(totalSec / 3600);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%03d", h, m, s, decimalSep, ms);
    return std::string(buf);
}

std::string formatAssTime(std::int64_t totalMs) {
    totalMs = std::max<std::int64_t>(0, totalMs);
    const int cs = static_cast<int>((totalMs % 1000) / 10);
    const std::int64_t totalSec = totalMs / 1000;
    const int s = static_cast<int>(totalSec % 60);
    const int m = static_cast<int>((totalSec / 60) % 60);
    const int h = static_cast<int>(totalSec / 3600);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", h, m, s, cs);
    return std::string(buf);
}

std::vector<SubtitleCue> parseSubtitles(const std::string& utf8Raw, const std::string& extLower) {
    QString content = QString::fromUtf8(utf8Raw.data(), static_cast<qsizetype>(utf8Raw.size()));
    if (!content.isEmpty() && content[0] == QChar(0xFEFF)) {
        content.remove(0, 1);
    }
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    content.replace('\r', '\n');

    std::vector<SubtitleCue> cues;

    if (extLower == ".ass" || extLower == ".ssa" || content.contains(QStringLiteral("\nDialogue:"), Qt::CaseInsensitive)) {
        static const QRegularExpression assTagRe(QStringLiteral(R"(\{[^}]*\})"));
        for (const QString& line : content.split('\n')) {
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith(QStringLiteral("Dialogue:"), Qt::CaseInsensitive)) continue;
            const QString rest = trimmed.mid(9).trimmed();
            // Split into at most 10 fields separated by first 9 commas
            int commaPositions[9];
            int found = 0;
            for (int i = 0; i < rest.size() && found < 9; ++i) {
                if (rest[i] == ',') {
                    commaPositions[found++] = i;
                }
            }
            if (found < 9) continue;
            const QString startStr = rest.mid(commaPositions[0] + 1, commaPositions[1] - commaPositions[0] - 1);
            const QString endStr = rest.mid(commaPositions[1] + 1, commaPositions[2] - commaPositions[1] - 1);
            QString text = rest.mid(commaPositions[8] + 1);
            text.remove(assTagRe);
            text.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
            text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            text.replace(QStringLiteral("\\h"), QStringLiteral(" "));
            text = text.trimmed();

            const std::int64_t sMs = parseAssTimestampMs(startStr);
            const std::int64_t eMs = parseAssTimestampMs(endStr);
            if (sMs >= 0 && eMs >= sMs && !text.isEmpty()) {
                cues.push_back(SubtitleCue{sMs, eMs, text});
            }
        }
        if (!cues.empty()) return cues;
    }

    // SRT / WebVTT parser
    static const QRegularExpression timeLineRe(
        QStringLiteral(R"(^(?:(\d{1,2}):)?(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(?:(\d{1,2}):)?(\d{2}):(\d{2})[,.](\d{1,3}))"));
    static const QRegularExpression htmlTagRe(QStringLiteral(R"(<[^>]+>)"));

    const QStringList lines = content.split('\n');
    int i = 0;
    while (i < lines.size()) {
        const QString line = lines[i].trimmed();
        const auto m = timeLineRe.match(line);
        if (m.hasMatch()) {
            const std::int64_t sMs = parseSrtVttTimestampMs(m.captured(1), m.captured(2), m.captured(3), m.captured(4));
            const std::int64_t eMs = parseSrtVttTimestampMs(m.captured(5), m.captured(6), m.captured(7), m.captured(8));
            ++i;
            QStringList textLines;
            while (i < lines.size() && !lines[i].trimmed().isEmpty()) {
                if (timeLineRe.match(lines[i].trimmed()).hasMatch()) break;
                QString cleaned = lines[i];
                cleaned.remove(htmlTagRe);
                textLines << cleaned.trimmed();
                ++i;
            }
            const QString cueText = textLines.join('\n').trimmed();
            if (!cueText.isEmpty()) {
                cues.push_back(SubtitleCue{sMs, std::max(sMs, eMs), cueText});
            }
        } else {
            ++i;
        }
    }
    return cues;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string csvEscape(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

} // namespace

void ToolboxEngine::compressImageToExactKb(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    int targetKb,
    const std::string& targetFormat,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(10);

    const QImage src = ImageEngine::loadAnyImage(inPath);
    if (src.isNull()) {
        throw std::runtime_error("无法解码源图片文件");
    }

    const std::size_t maxBytes = static_cast<std::size_t>(std::clamp(targetKb, 5, 51200)) * 1024u;
    const std::string fmt = (lowerAscii(targetFormat) == "webp") ? "webp" : "jpg";

    QByteArray bestBytes;
    const double scales[] = {1.0, 0.90, 0.80, 0.70, 0.60, 0.50, 0.40, 0.32, 0.25, 0.18, 0.12, 0.08};
    const std::size_t numScales = sizeof(scales) / sizeof(scales[0]);

    for (std::size_t sIdx = 0; sIdx < numScales; ++sIdx) {
        if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");

        const double scale = scales[sIdx];
        QImage candidate = src;
        if (scale < 0.999) {
            const int nw = std::max(16, static_cast<int>(std::lround(src.width() * scale)));
            const int nh = std::max(16, static_cast<int>(std::lround(src.height() * scale)));
            candidate = src.scaled(nw, nh, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }

        // Quick check at min quality first so we don't waste binary-search steps if resolution is still too large
        const QByteArray minQBytes = encodeImageToBuffer(candidate, fmt, 6);
        if (minQBytes.isEmpty()) continue;

        if (static_cast<std::size_t>(minQBytes.size()) > maxBytes) {
            bestBytes = minQBytes; // Keep smallest seen so far in case cap is tiny
            if (progressCb) progressCb(15 + static_cast<int>((sIdx + 1) * 65 / numScales));
            continue;
        }

        // Binary search highest quality in [6..96] that fits <= maxBytes
        int lo = 6;
        int hi = 96;
        QByteArray bestAtScale = minQBytes;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const QByteArray trial = encodeImageToBuffer(candidate, fmt, mid);
            if (!trial.isEmpty() && static_cast<std::size_t>(trial.size()) <= maxBytes) {
                bestAtScale = trial;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        bestBytes = bestAtScale;
        break;
    }

    if (bestBytes.isEmpty()) {
        throw std::runtime_error("目标体积过小，无法完成编码");
    }

    if (progressCb) progressCb(92);
    const std::vector<std::uint8_t> rawVec(bestBytes.begin(), bestBytes.end());
    omni::infra::AtomicFileTx::writeBytesAtomic(outPath, rawVec);
    if (progressCb) progressCb(100);
}

void ToolboxEngine::applyIdSecurityWatermark(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& watermarkText,
    int opacityPercent,
    const std::string& targetFormat,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(15);

    QImage src = ImageEngine::loadAnyImage(inPath);
    if (src.isNull()) {
        throw std::runtime_error("无法读取证件源图片");
    }

    QImage canvas = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = canvas.width();
    const int h = canvas.height();

    QString wmText = QString::fromUtf8(watermarkText.c_str()).trimmed();
    if (wmText.isEmpty()) {
        wmText = QStringLiteral("仅供本次业务办理使用 他用无效");
    }

    const int alpha = std::clamp(opacityPercent, 5, 90) * 255 / 100;
    const int fontPx = std::clamp(std::min(w, h) / 20, 14, 96);

    QPainter painter(&canvas);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    QFont font(QStringLiteral("PingFang SC"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(fontPx);
    font.setBold(true);
    painter.setFont(font);

    const QFontMetricsF fm(font);
    const double textW = std::max(40.0, fm.horizontalAdvance(wmText));
    const double stepX = textW + fontPx * 3.0;
    const double stepY = fontPx * 3.4;
    const double diag = std::hypot(static_cast<double>(w), static_cast<double>(h));

    painter.save();
    painter.translate(w / 2.0, h / 2.0);
    painter.rotate(-28.0);

    int rowIdx = 0;
    for (double y = -diag; y <= diag; y += stepY, ++rowIdx) {
        const double rowOffset = (rowIdx % 2 != 0) ? (stepX * 0.5) : 0.0;
        for (double x = -diag + rowOffset; x <= diag; x += stepX) {
            // Subtle dark shadow + crisp light security text so watermark is tamper-resistant on both dark & white areas
            painter.setPen(QColor(20, 24, 30, std::min(255, static_cast<int>(alpha * 0.85))));
            painter.drawText(QPointF(x + 1.5, y + 1.5), wmText);

            painter.setPen(QColor(245, 247, 250, alpha));
            painter.drawText(QPointF(x, y), wmText);
        }
    }
    painter.restore();
    painter.end();

    if (progressCb) progressCb(85);

    const std::string fmt = lowerAscii(targetFormat);
    const QByteArray encoded = encodeImageToBuffer(canvas, fmt.empty() ? "jpg" : fmt, 90);
    if (encoded.isEmpty()) {
        throw std::runtime_error("保存防盗水印图片失败");
    }
    const std::vector<std::uint8_t> rawVec(encoded.begin(), encoded.end());
    omni::infra::AtomicFileTx::writeBytesAtomic(outPath, rawVec);
    if (progressCb) progressCb(100);
}

void ToolboxEngine::convertAndShiftSubtitle(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    int shiftMilliseconds,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(15);

    const std::string utf8Raw = omni::infra::TextEncoding::readFileSmartUtf8(inPath);
    const std::string inExt = lowerAscii(omni::core::pathToUtf8(inPath.extension()));
    auto cues = parseSubtitles(utf8Raw, inExt);
    if (cues.empty()) {
        throw std::runtime_error("未能在字幕文件中识别到有效时间轴条目");
    }

    for (auto& cue : cues) {
        cue.startMs = std::max<std::int64_t>(0, cue.startMs + shiftMilliseconds);
        cue.endMs = std::max<std::int64_t>(cue.startMs + 50, cue.endMs + shiftMilliseconds);
    }

    if (progressCb) progressCb(65);

    const std::string fmt = lowerAscii(targetFormat);
    std::ostringstream ss;

    if (fmt == "srt") {
        for (std::size_t i = 0; i < cues.size(); ++i) {
            ss << (i + 1) << "\n"
               << formatSrtTime(cues[i].startMs, ',') << " --> " << formatSrtTime(cues[i].endMs, ',') << "\n"
               << cues[i].text.toUtf8().toStdString() << "\n\n";
        }
    } else if (fmt == "vtt") {
        ss << "WEBVTT\n\n";
        for (std::size_t i = 0; i < cues.size(); ++i) {
            ss << (i + 1) << "\n"
               << formatSrtTime(cues[i].startMs, '.') << " --> " << formatSrtTime(cues[i].endMs, '.') << "\n"
               << cues[i].text.toUtf8().toStdString() << "\n\n";
        }
    } else if (fmt == "ass") {
        ss << "[Script Info]\n"
           << "ScriptType: v4.00+\n"
           << "PlayResX: 1920\n"
           << "PlayResY: 1080\n"
           << "WrapStyle: 0\n"
           << "ScaledBorderAndShadow: yes\n\n"
           << "[V4+ Styles]\n"
           << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
              "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
              "Alignment, MarginL, MarginR, MarginV, Encoding\n"
           << "Style: Default,PingFang SC,54,&H00FFFFFF,&H000000FF,&H00181818,&H64000000,"
              "0,0,0,0,100,100,0,0,1,2.4,1.2,2,30,30,36,1\n\n"
           << "[Events]\n"
           << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        for (const auto& c : cues) {
            QString assTxt = c.text;
            assTxt.replace('\n', QStringLiteral("\\N"));
            ss << "Dialogue: 0," << formatAssTime(c.startMs) << "," << formatAssTime(c.endMs)
               << ",Default,,0,0,0,," << assTxt.toUtf8().toStdString() << "\n";
        }
    } else if (fmt == "txt") {
        QString prevLine;
        for (const auto& c : cues) {
            const QString flat = c.text.simplified();
            if (!flat.isEmpty() && flat != prevLine) {
                ss << flat.toUtf8().toStdString() << "\n";
                prevLine = flat;
            }
        }
    } else {
        throw std::runtime_error("不支持的字幕导出格式: " + targetFormat);
    }

    omni::infra::AtomicFileTx::writeStringAtomic(outPath, ss.str());
    if (progressCb) progressCb(100);
}

void ToolboxEngine::generateHashManifest(
    const std::vector<std::filesystem::path>& inPaths,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    const std::function<void(std::size_t index, int pct)>& itemProgressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (inPaths.empty()) {
        throw std::runtime_error("没有可校验的文件");
    }

    struct HashEntry {
        std::string fileName;
        std::string fullPath;
        std::uintmax_t byteSize = 0;
        std::string humanSize;
        std::string sha256Hex;
        std::string md5Hex;
    };

    std::vector<HashEntry> entries;
    entries.reserve(inPaths.size());

    for (std::size_t idx = 0; idx < inPaths.size(); ++idx) {
        if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");

        const auto& p = inPaths[idx];
        QFile f(omni::core::pathToQString(p));
        if (!f.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("无法读取待校验文件: " + omni::core::pathToUtf8(p.filename()));
        }

        const qint64 totalBytes = std::max<qint64>(1, f.size());
        qint64 processed = 0;

        QCryptographicHash sha256(QCryptographicHash::Sha256);
        QCryptographicHash md5(QCryptographicHash::Md5);

        QByteArray chunk(256 * 1024, Qt::Uninitialized);
        while (!f.atEnd()) {
            if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
            const qint64 n = f.read(chunk.data(), chunk.size());
            if (n <= 0) break;
            const QByteArrayView view(chunk.constData(), n);
            sha256.addData(view);
            md5.addData(view);
            processed += n;
            if (itemProgressCb) {
                itemProgressCb(idx, std::min(96, static_cast<int>(processed * 95 / totalBytes)));
            }
        }
        f.close();

        std::error_code ec;
        const std::uintmax_t fsize = std::filesystem::file_size(p, ec);

        HashEntry e;
        e.fileName = omni::core::pathToUtf8(p.filename());
        e.fullPath = omni::core::pathToUtf8(p);
        e.byteSize = fsize;
        e.humanSize = omni::core::formatByteSize(fsize);
        e.sha256Hex = sha256.result().toHex().toStdString();
        e.md5Hex = md5.result().toHex().toStdString();
        entries.push_back(std::move(e));

        if (itemProgressCb) itemProgressCb(idx, 98);
    }

    const std::string fmt = lowerAscii(targetFormat);
    std::ostringstream out;

    if (fmt == "csv") {
        // UTF-8 BOM for direct Microsoft Excel compatibility
        out << "\xEF\xBB\xBF";
        out << "文件名,大小(字节),可读大小,SHA-256,MD5,文件路径\r\n";
        for (const auto& e : entries) {
            out << csvEscape(e.fileName) << ","
                << e.byteSize << ","
                << csvEscape(e.humanSize) << ","
                << e.sha256Hex << ","
                << e.md5Hex << ","
                << csvEscape(e.fullPath) << "\r\n";
        }
    } else if (fmt == "json") {
        out << "[\n";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            out << "  {\n"
                << "    \"file_name\": \"" << jsonEscape(e.fileName) << "\",\n"
                << "    \"size_bytes\": " << e.byteSize << ",\n"
                << "    \"size_human\": \"" << jsonEscape(e.humanSize) << "\",\n"
                << "    \"sha256\": \"" << e.sha256Hex << "\",\n"
                << "    \"md5\": \"" << e.md5Hex << "\",\n"
                << "    \"path\": \"" << jsonEscape(e.fullPath) << "\"\n"
                << "  }" << (i + 1 < entries.size() ? "," : "") << "\n";
        }
        out << "]\n";
    } else {
        out << "# OmniConvert 离线文件完整性哈希校验清单\n"
            << "# 共计文件数: " << entries.size() << "\n\n";
        for (const auto& e : entries) {
            out << "文件: " << e.fileName << " (" << e.humanSize << " / " << e.byteSize << " bytes)\n"
                << "SHA-256: " << e.sha256Hex << "\n"
                << "MD5:     " << e.md5Hex << "\n\n";
        }
        out << "# 标准 sha256sum 兼容行:\n";
        for (const auto& e : entries) {
            out << e.sha256Hex << "  " << e.fileName << "\n";
        }
    }

    omni::infra::AtomicFileTx::writeStringAtomic(outPath, out.str());
    for (std::size_t i = 0; i < inPaths.size(); ++i) {
        if (itemProgressCb) itemProgressCb(i, 100);
    }
}

} // namespace omni::engines
