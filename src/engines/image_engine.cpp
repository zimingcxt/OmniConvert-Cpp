#include "omni/engines/image_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/portable_env.hpp"

#include <QBuffer>
#include <QColor>
#include <QColorSpace>
#include <QFile>
#include <QFont>
#include <QImageReader>
#include <QImageWriter>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace omni::engines {

namespace {

std::string toUpperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

bool qtSupportsWrite(const char* fmtLower) {
    static std::mutex mu;
    static std::unordered_map<std::string, bool> cache;
    const std::string key(fmtLower ? fmtLower : "");
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    bool listed = false;
    const auto formats = QImageWriter::supportedImageFormats();
    for (const auto& f : formats) {
        if (f.compare(fmtLower, Qt::CaseInsensitive) == 0) {
            listed = true;
            break;
        }
    }
    bool ok = listed;
    if (listed && (key == "heic" || key == "heif" || key == "avif")) {
        QImage probe(256, 192, QImage::Format_RGB32);
        probe.fill(QColor(64, 128, 220));
        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, key.c_str());
        ok = w.write(probe) && !out.isEmpty();
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        cache[key] = ok;
    }
    return ok;
}

std::optional<std::filesystem::path> resolveHeifEnc() {
#ifdef _WIN32
    const std::string exe = "heif-enc.exe";
#else
    const std::string exe = "heif-enc";
#endif
    const auto root = omni::infra::PortableEnv::appRootDir();
    const auto exeDir = omni::infra::PortableEnv::executableDir();
    std::vector<std::filesystem::path> cands = {
        root / "bin" / exe,
        exeDir / "bin" / exe,
#ifdef __APPLE__
        "/opt/homebrew/bin/heif-enc",
        "/usr/local/bin/heif-enc",
#endif
    };
    for (const auto& c : cands) {
        if (omni::infra::PortableEnv::ensureExecutable(c)) return c;
    }
    const QString sys = QStandardPaths::findExecutable(QString::fromStdString(exe));
    if (!sys.isEmpty()) return omni::core::qStringToPath(sys);
    return std::nullopt;
}

bool encodeViaHeifEnc(const QImage& img, const std::filesystem::path& outPath, int quality, bool lossless, std::string* errOut) {
    const auto heifEnc = resolveHeifEnc();
    if (!heifEnc) return false;
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) return false;
    const QString srcPng = tmpDir.filePath(QStringLiteral("input.png"));
    if (!img.save(srcPng, "PNG")) return false;

    QStringList args;
    if (lossless) {
        args << QStringLiteral("-L");
    } else {
        args << QStringLiteral("-q") << QString::number(std::clamp(quality, 1, 100));
    }
    args << srcPng << QStringLiteral("-o") << omni::core::pathToQString(outPath);

    QProcess proc;
    proc.start(omni::core::pathToQString(*heifEnc), args);
    if (!proc.waitForFinished(45000) || proc.exitCode() != 0) {
        if (errOut) *errOut = proc.readAllStandardError().trimmed().toStdString();
        return false;
    }
    return std::filesystem::exists(outPath);
}

bool decodeViaFfmpeg(const std::filesystem::path& inPath, QImage& outImg) {
    const auto ff = omni::infra::PortableEnv::resolveFfmpeg();
    if (!ff) return false;

    QProcess proc;
    proc.start(omni::core::pathToQString(*ff),
               {QStringLiteral("-v"), QStringLiteral("error"),
                QStringLiteral("-i"), omni::core::pathToQString(inPath),
                QStringLiteral("-frames:v"), QStringLiteral("1"),
                QStringLiteral("-f"), QStringLiteral("image2pipe"),
                QStringLiteral("-vcodec"), QStringLiteral("png"),
                QStringLiteral("pipe:1")});
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0) {
        return false;
    }
    const QByteArray pngBytes = proc.readAllStandardOutput();
    return !pngBytes.isEmpty() && outImg.loadFromData(pngBytes, "PNG");
}

bool encodeViaFfmpeg(
    const QImage& img,
    const std::filesystem::path& outPath,
    const std::string& fmtUpper,
    int quality,
    bool lossless,
    std::string* errOut) {

    const auto ff = omni::infra::PortableEnv::resolveFfmpeg();
    if (!ff) {
        if (errOut) *errOut = "未找到 FFmpeg 内核";
        return false;
    }

    QByteArray pngInput;
    {
        QBuffer buf(&pngInput);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG", 90);
    }

    auto runFfmpegWithCodec = [&](const QStringList& codecArgs) -> bool {
        QStringList args = {
            QStringLiteral("-y"),
            QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("png_pipe"),
            QStringLiteral("-i"), QStringLiteral("pipe:0"),
            QStringLiteral("-frames:v"), QStringLiteral("1")
        };
        args << codecArgs << omni::core::pathToQString(outPath);

        QProcess proc;
        proc.start(omni::core::pathToQString(*ff), args);
        if (!proc.waitForStarted(5000)) {
            if (errOut) *errOut = "无法启动 FFmpeg";
            return false;
        }
        proc.write(pngInput);
        proc.closeWriteChannel();
        if (!proc.waitForFinished(60000) || proc.exitCode() != 0) {
            if (errOut) *errOut = proc.readAllStandardError().trimmed().toStdString();
            return false;
        }
        return true;
    };

    if (fmtUpper == "AVIF") {
        const int crf = lossless ? 0 : std::clamp(63 - (quality * 55 / 100), 6, 56);
        if (runFfmpegWithCodec({QStringLiteral("-c:v"), QStringLiteral("libsvtav1"),
                                QStringLiteral("-crf"), QString::number(crf),
                                QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                                QStringLiteral("-f"), QStringLiteral("avif")})) {
            return true;
        }
        return runFfmpegWithCodec({QStringLiteral("-c:v"), QStringLiteral("libaom-av1"),
                                   QStringLiteral("-cpu-used"), QStringLiteral("6"),
                                   QStringLiteral("-crf"), QString::number(crf),
                                   QStringLiteral("-b:v"), QStringLiteral("0"),
                                   QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                                   QStringLiteral("-f"), QStringLiteral("avif")});
    } else if (fmtUpper == "WEBP") {
        return runFfmpegWithCodec({QStringLiteral("-c:v"), QStringLiteral("libwebp"),
                                   QStringLiteral("-lossless"), (lossless ? QStringLiteral("1") : QStringLiteral("0")),
                                   QStringLiteral("-quality"), QString::number(quality)});
    }
    return runFfmpegWithCodec({});
}

struct ColorBox {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint8_t rMin = 255, rMax = 0;
    std::uint8_t gMin = 255, gMax = 0;
    std::uint8_t bMin = 255, bMax = 0;
    std::uint8_t aMin = 255, aMax = 0;

    void recomputeBounds(const std::vector<std::pair<QRgb, std::uint32_t>>& hist) {
        rMin = gMin = bMin = aMin = 255;
        rMax = gMax = bMax = aMax = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const QRgb c = hist[i].first;
            const auto r = static_cast<std::uint8_t>(qRed(c));
            const auto g = static_cast<std::uint8_t>(qGreen(c));
            const auto b = static_cast<std::uint8_t>(qBlue(c));
            const auto a = static_cast<std::uint8_t>(qAlpha(c));
            rMin = std::min(rMin, r); rMax = std::max(rMax, r);
            gMin = std::min(gMin, g); gMax = std::max(gMax, g);
            bMin = std::min(bMin, b); bMax = std::max(bMax, b);
            aMin = std::min(aMin, a); aMax = std::max(aMax, a);
        }
    }

    int maxRange() const {
        return std::max({rMax - rMin, gMax - gMin, bMax - bMin, aMax - aMin});
    }
};

void writeGif89a(const QImage& indexedImg, const std::filesystem::path& outPath) {
    QImage idxImg = indexedImg;
    if (idxImg.format() != QImage::Format_Indexed8) {
        idxImg = ImageEngine::quantizeColors(indexedImg, 256, indexedImg.hasAlphaChannel());
    }

    const int width = idxImg.width();
    const int height = idxImg.height();
    QList<QRgb> table = idxImg.colorTable();
    if (table.isEmpty()) table.append(qRgb(0, 0, 0));

    int colorBits = 1;
    while ((1 << colorBits) < table.size() && colorBits < 8) ++colorBits;
    const int paletteSize = 1 << colorBits;

    int transparentIdx = -1;
    for (int i = 0; i < table.size(); ++i) {
        if (qAlpha(table[i]) < 128) { transparentIdx = i; break; }
    }

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(width * height / 2 + 1024));
    auto put16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    };

    const char hdr[6] = {'G', 'I', 'F', '8', '9', 'a'};
    out.insert(out.end(), hdr, hdr + 6);
    put16(static_cast<std::uint16_t>(width));
    put16(static_cast<std::uint16_t>(height));
    const std::uint8_t gctFlag = static_cast<std::uint8_t>(0x80 | ((colorBits - 1) << 4) | (colorBits - 1));
    out.push_back(gctFlag);
    out.push_back(0);
    out.push_back(0);

    for (int i = 0; i < paletteSize; ++i) {
        const QRgb c = (i < table.size()) ? table[i] : qRgb(0, 0, 0);
        out.push_back(static_cast<std::uint8_t>(qRed(c)));
        out.push_back(static_cast<std::uint8_t>(qGreen(c)));
        out.push_back(static_cast<std::uint8_t>(qBlue(c)));
    }

    if (transparentIdx >= 0) {
        out.insert(out.end(), {0x21, 0xF9, 0x04, 0x05, 0x00, 0x00, static_cast<std::uint8_t>(transparentIdx), 0x00});
    }

    out.push_back(0x2C);
    put16(0); put16(0);
    put16(static_cast<std::uint16_t>(width));
    put16(static_cast<std::uint16_t>(height));
    out.push_back(0x00);

    const int minCodeSize = std::max(2, colorBits);
    out.push_back(static_cast<std::uint8_t>(minCodeSize));

    const int clearCode = 1 << minCodeSize;
    const int eoiCode = clearCode + 1;
    int nextCode = eoiCode + 1;
    int codeSize = minCodeSize + 1;

    std::vector<int> dict(4096 * 256, -1);
    auto resetDict = [&]() {
        std::fill(dict.begin(), dict.end(), -1);
        nextCode = eoiCode + 1;
        codeSize = minCodeSize + 1;
    };

    std::vector<std::uint8_t> subBlock;
    subBlock.reserve(255);
    std::uint32_t bitBuf = 0;
    int bitCount = 0;

    auto flushSubBlock = [&]() {
        if (!subBlock.empty()) {
            out.push_back(static_cast<std::uint8_t>(subBlock.size()));
            out.insert(out.end(), subBlock.begin(), subBlock.end());
            subBlock.clear();
        }
    };
    auto emitCode = [&](int code) {
        bitBuf |= (static_cast<std::uint32_t>(code) << bitCount);
        bitCount += codeSize;
        while (bitCount >= 8) {
            subBlock.push_back(static_cast<std::uint8_t>(bitBuf & 0xFF));
            if (subBlock.size() == 255) flushSubBlock();
            bitBuf >>= 8;
            bitCount -= 8;
        }
    };

    emitCode(clearCode);
    int prefix = -1;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* scan = idxImg.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            const int k = scan[x] & 0xFF;
            if (prefix < 0) { prefix = k; continue; }
            const int key = (prefix << 8) | k;
            const int found = dict[static_cast<std::size_t>(key)];
            if (found >= 0) {
                prefix = found;
            } else {
                emitCode(prefix);
                if (nextCode < 4096) {
                    dict[static_cast<std::size_t>(key)] = nextCode;
                    if (nextCode == (1 << codeSize) && codeSize < 12) ++codeSize;
                    ++nextCode;
                } else {
                    emitCode(clearCode);
                    resetDict();
                }
                prefix = k;
            }
        }
    }

    if (prefix >= 0) emitCode(prefix);
    emitCode(eoiCode);
    if (bitCount > 0) subBlock.push_back(static_cast<std::uint8_t>(bitBuf & 0xFF));
    flushSubBlock();
    out.push_back(0x00);
    out.push_back(0x3B);

    QFile f(omni::core::pathToQString(outPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("无法写入 GIF 文件");
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<qint64>(out.size()));
    f.close();
}

void applyCornerWatermark(QImage& img, const std::string& wmTextUtf8) {
    const QString text = QString::fromUtf8(wmTextUtf8.c_str()).trimmed();
    if (text.isEmpty() || img.isNull()) return;

    img = img.convertToFormat(QImage::Format_RGBA8888);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    const int minSide = std::min(img.width(), img.height());
    const int pxSize = std::clamp(minSide / 24, 12, 72);
    QFont font(QStringLiteral("PingFang SC"));
    font.setPixelSize(pxSize);
    font.setBold(true);
    p.setFont(font);

    const QFontMetrics fm(font);
    const QRect textRect = fm.boundingRect(text);
    const int padX = std::max(10, pxSize / 2);
    const int padY = std::max(6, pxSize / 3);
    const int margin = std::max(12, minSide / 32);

    const QRect pillRect(
        img.width() - textRect.width() - padX * 2 - margin,
        img.height() - textRect.height() - padY * 2 - margin,
        textRect.width() + padX * 2,
        textRect.height() + padY * 2);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 23, 42, 125));
    p.drawRoundedRect(pillRect, padY, padY);

    p.setPen(QColor(255, 255, 255, 230));
    p.drawText(pillRect, Qt::AlignCenter, text);
}

void writeSingleImagePdf(const QImage& img, const std::filesystem::path& outPath) {
    QPdfWriter writer(omni::core::pathToQString(outPath));
    writer.setResolution(72);
    const QSizeF ptSize(std::max(72.0, static_cast<double>(img.width())),
                        std::max(72.0, static_cast<double>(img.height())));
    writer.setPageSize(QPageSize(ptSize, QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
    QPainter painter(&writer);
    if (!painter.isActive()) {
        throw std::runtime_error("初始化单图 PDF 写入器失败");
    }
    painter.drawImage(painter.viewport(), img);
    painter.end();
}

void writeTruevisionTga(const QImage& src, const std::filesystem::path& outPath) {
    const bool hasAlpha = src.hasAlphaChannel();
    const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
    const int w = std::min(65535, rgba.width());
    const int h = std::min(65535, rgba.height());
    const int bpp = hasAlpha ? 4 : 3;

    std::vector<std::uint8_t> tga;
    tga.reserve(18u + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * bpp + 26u);

    std::uint8_t hdr[18] = {};
    hdr[2] = 2; // Uncompressed True-Color
    hdr[12] = static_cast<std::uint8_t>(w & 0xFF);
    hdr[13] = static_cast<std::uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<std::uint8_t>(h & 0xFF);
    hdr[15] = static_cast<std::uint8_t>((h >> 8) & 0xFF);
    hdr[16] = static_cast<std::uint8_t>(bpp * 8);
    hdr[17] = static_cast<std::uint8_t>(0x20 | (hasAlpha ? 8 : 0)); // bit 5 = top-to-bottom
    tga.insert(tga.end(), hdr, hdr + 18);

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = rgba.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            tga.push_back(row[x * 4 + 2]); // B
            tga.push_back(row[x * 4 + 1]); // G
            tga.push_back(row[x * 4 + 0]); // R
            if (hasAlpha) tga.push_back(row[x * 4 + 3]); // A
        }
    }

    std::uint8_t footer[26] = {};
    std::memcpy(footer + 8, "TRUEVISION-XFILE.", 17);
    footer[25] = 0;
    tga.insert(tga.end(), footer, footer + 26);

    QFile f(omni::core::pathToQString(outPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("无法写入 TGA 文件");
    }
    f.write(reinterpret_cast<const char*>(tga.data()), static_cast<qint64>(tga.size()));
    f.close();
}

void writeAppleIcns(const QImage& src, const std::filesystem::path& outPath) {
    const QImage base = src.convertToFormat(QImage::Format_RGBA8888);
    struct IcnsSlot { const char tag[5]; int size; };
    static const IcnsSlot slotsDef[] = {
        {"icp4", 16}, {"icp5", 32}, {"icp6", 64}, {"ic07", 128}, {"ic08", 256}, {"ic09", 512}
    };

    struct Chunk { char tag[4]; QByteArray png; };
    std::vector<Chunk> chunks;
    const int maxDim = std::max(base.width(), base.height());
    for (const auto& sl : slotsDef) {
        if (sl.size > maxDim && sl.size > 128 && !chunks.empty()) continue;
        QImage canvas(sl.size, sl.size, QImage::Format_RGBA8888);
        canvas.fill(Qt::transparent);
        const QImage scaled = base.scaled(sl.size, sl.size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&canvas);
        p.drawImage((sl.size - scaled.width()) / 2, (sl.size - scaled.height()) / 2, scaled);
        p.end();

        QByteArray pngBytes;
        QBuffer buf(&pngBytes);
        buf.open(QIODevice::WriteOnly);
        canvas.save(&buf, "PNG");
        if (!pngBytes.isEmpty()) {
            Chunk c{};
            std::memcpy(c.tag, sl.tag, 4);
            c.png = std::move(pngBytes);
            chunks.push_back(std::move(c));
        }
    }

    std::uint32_t totalLen = 8;
    for (const auto& c : chunks) totalLen += static_cast<std::uint32_t>(8 + c.png.size());

    std::vector<std::uint8_t> out;
    out.reserve(totalLen);
    auto putBe32 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };

    out.insert(out.end(), {'i', 'c', 'n', 's'});
    putBe32(totalLen);
    for (const auto& c : chunks) {
        out.insert(out.end(), c.tag, c.tag + 4);
        putBe32(static_cast<std::uint32_t>(8 + c.png.size()));
        out.insert(out.end(), c.png.begin(), c.png.end());
    }

    QFile f(omni::core::pathToQString(outPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("无法写入 ICNS 图标文件");
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<qint64>(out.size()));
    f.close();
}

} // namespace

bool ImageEngine::supportsAvif() {
    if (qtSupportsWrite("avif")) return true;
#ifdef __APPLE__
    if (macos_imageio::supportsUtiWrite("avif")) return true;
#endif
    return omni::infra::PortableEnv::resolveFfmpeg().has_value();
}

bool ImageEngine::supportsHeic() {
    if (qtSupportsWrite("heic") || qtSupportsWrite("heif")) return true;
#ifdef __APPLE__
    if (macos_imageio::supportsUtiWrite("heic")) return true;
#endif
    return resolveHeifEnc().has_value();
}

QImage ImageEngine::loadAnyImage(const std::filesystem::path& inPath) {
    std::string inExt = omni::core::pathToUtf8(inPath.extension());
    std::transform(inExt.begin(), inExt.end(), inExt.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    QImage img;
    bool loaded = false;

#ifdef __APPLE__
    if (inExt == ".heic" || inExt == ".heif" || inExt == ".avif") {
        loaded = macos_imageio::readImageViaImageIO(inPath, img);
    }
#endif

    if (!loaded) {
        QImageReader reader(omni::core::pathToQString(inPath));
        reader.setAutoTransform(true);
        reader.setAllocationLimit(2048); // Support ultra-high-res 8K/16K assets up to 2GB
        if (inExt == ".svg") {
            const QSize defSz = reader.size();
            if (defSz.isValid() && std::max(defSz.width(), defSz.height()) < 1024) {
                reader.setScaledSize(defSz.scaled(1024, 1024, Qt::KeepAspectRatio));
            }
        }
        img = reader.read();
        loaded = !img.isNull();
    }

#ifdef __APPLE__
    if (!loaded) {
        loaded = macos_imageio::readImageViaImageIO(inPath, img);
    }
#endif

    if (!loaded) {
        loaded = decodeViaFfmpeg(inPath, img);
    }

    if (!loaded || img.isNull()) {
        throw std::runtime_error("无法解码输入图片格式: " + inExt);
    }
    return img;
}

QImage ImageEngine::quantizeColors(const QImage& src, int maxColors, bool preserveAlpha) {
    maxColors = std::clamp(maxColors, 2, 256);
    QImage img = src.convertToFormat(preserveAlpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    const int w = img.width();
    const int h = img.height();

    std::unordered_map<std::uint32_t, std::pair<QRgb, std::uint32_t>> bins;
    bins.reserve(16384);

    for (int y = 0; y < h; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            const std::uint8_t a = preserveAlpha ? static_cast<std::uint8_t>(qAlpha(px)) : 255;
            if (a < 8) {
                auto& entry = bins[0];
                entry.first = qRgba(0, 0, 0, 0);
                ++entry.second;
                continue;
            }
            const std::uint32_t key =
                (static_cast<std::uint32_t>(a >> 4) << 16) |
                (static_cast<std::uint32_t>(qRed(px) >> 3) << 11) |
                (static_cast<std::uint32_t>(qGreen(px) >> 2) << 5) |
                static_cast<std::uint32_t>(qBlue(px) >> 3);
            auto& entry = bins[key];
            if (entry.second == 0) entry.first = qRgba(qRed(px), qGreen(px), qBlue(px), a);
            ++entry.second;
        }
    }

    std::vector<std::pair<QRgb, std::uint32_t>> hist;
    hist.reserve(bins.size());
    for (const auto& kv : bins) hist.push_back(kv.second);

    if (hist.empty()) {
        QImage empty(w, h, QImage::Format_Indexed8);
        empty.setColorTable({qRgba(0, 0, 0, 0)});
        empty.fill(0);
        return empty;
    }

    std::vector<ColorBox> boxes;
    boxes.reserve(static_cast<std::size_t>(maxColors));
    ColorBox root{0, hist.size()};
    root.recomputeBounds(hist);
    boxes.push_back(root);

    while (static_cast<int>(boxes.size()) < maxColors) {
        int bestIdx = -1;
        int bestRange = 0;
        for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
            if (boxes[i].end - boxes[i].begin > 1) {
                const int r = boxes[i].maxRange();
                if (r > bestRange) { bestRange = r; bestIdx = i; }
            }
        }
        if (bestIdx < 0 || bestRange == 0) break;

        ColorBox box = boxes[bestIdx];
        const int rSpan = box.rMax - box.rMin;
        const int gSpan = box.gMax - box.gMin;
        const int bSpan = box.bMax - box.bMin;
        const int aSpan = box.aMax - box.aMin;

        auto cmp = [&](const auto& lhs, const auto& rhs) {
            if (aSpan >= rSpan && aSpan >= gSpan && aSpan >= bSpan) return qAlpha(lhs.first) < qAlpha(rhs.first);
            if (gSpan >= rSpan && gSpan >= bSpan) return qGreen(lhs.first) < qGreen(rhs.first);
            if (rSpan >= bSpan) return qRed(lhs.first) < qRed(rhs.first);
            return qBlue(lhs.first) < qBlue(rhs.first);
        };
        std::sort(hist.begin() + static_cast<std::ptrdiff_t>(box.begin),
                  hist.begin() + static_cast<std::ptrdiff_t>(box.end),
                  cmp);

        std::uint64_t totalWeight = 0;
        for (std::size_t i = box.begin; i < box.end; ++i) totalWeight += hist[i].second;

        std::uint64_t acc = 0;
        std::size_t mid = box.begin + 1;
        for (std::size_t i = box.begin; i + 1 < box.end; ++i) {
            acc += hist[i].second;
            if (acc * 2 >= totalWeight) { mid = i + 1; break; }
        }

        ColorBox left{box.begin, mid};
        ColorBox right{mid, box.end};
        left.recomputeBounds(hist);
        right.recomputeBounds(hist);
        boxes[bestIdx] = left;
        boxes.push_back(right);
    }

    QList<QRgb> palette;
    palette.reserve(static_cast<qsizetype>(boxes.size()));
    for (const auto& b : boxes) {
        std::uint64_t sr = 0, sg = 0, sb = 0, sa = 0, sw = 0;
        for (std::size_t i = b.begin; i < b.end; ++i) {
            const QRgb c = hist[i].first;
            const std::uint64_t wt = hist[i].second;
            sr += static_cast<std::uint64_t>(qRed(c)) * wt;
            sg += static_cast<std::uint64_t>(qGreen(c)) * wt;
            sb += static_cast<std::uint64_t>(qBlue(c)) * wt;
            sa += static_cast<std::uint64_t>(qAlpha(c)) * wt;
            sw += wt;
        }
        if (sw == 0) sw = 1;
        palette.append(qRgba(static_cast<int>(sr / sw),
                             static_cast<int>(sg / sw),
                             static_cast<int>(sb / sw),
                             preserveAlpha ? static_cast<int>(sa / sw) : 255));
    }

    std::unordered_map<std::uint32_t, std::uint8_t> nearestCache;
    nearestCache.reserve(16384);
    auto findNearest = [&](QRgb px) -> std::uint8_t {
        const std::uint8_t a = preserveAlpha ? static_cast<std::uint8_t>(qAlpha(px)) : 255;
        const std::uint32_t key =
            (static_cast<std::uint32_t>(a >> 3) << 15) |
            (static_cast<std::uint32_t>(qRed(px) >> 3) << 10) |
            (static_cast<std::uint32_t>(qGreen(px) >> 3) << 5) |
            static_cast<std::uint32_t>(qBlue(px) >> 3);
        auto it = nearestCache.find(key);
        if (it != nearestCache.end()) return it->second;

        int best = 0;
        int bestDist = 0x7FFFFFFF;
        const int r = qRed(px), g = qGreen(px), b = qBlue(px);
        for (int i = 0; i < palette.size(); ++i) {
            const QRgb pc = palette[i];
            const int dr = r - qRed(pc), dg = g - qGreen(pc), db = b - qBlue(pc), da = a - qAlpha(pc);
            const int dist = dr * dr * 2 + dg * dg * 4 + db * db + da * da * 4;
            if (dist < bestDist) { bestDist = dist; best = i; }
        }
        const auto idx = static_cast<std::uint8_t>(best);
        nearestCache.emplace(key, idx);
        return idx;
    };

    QImage indexed(w, h, QImage::Format_Indexed8);
    indexed.setColorTable(palette);
    for (int y = 0; y < h; ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        std::uint8_t* dstLine = indexed.scanLine(y);
        for (int x = 0; x < w; ++x) dstLine[x] = findNearest(srcLine[x]);
    }
    return indexed;
}

void ImageEngine::writeMultiSizeIco(const QImage& src, const std::filesystem::path& outPath) {
    QImage base = src.convertToFormat(QImage::Format_RGBA8888);
    if (std::max(base.width(), base.height()) > 256) {
        base = base.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const int maxSide = std::max(base.width(), base.height());
    std::vector<int> sizes;
    for (int s : {256, 128, 64, 48, 32, 16}) {
        if (s <= maxSide) sizes.push_back(s);
    }
    if (sizes.empty()) sizes.push_back(32);

    struct LayerData { int w = 0; int h = 0; QByteArray pngBytes; };
    std::vector<LayerData> layers;
    layers.reserve(sizes.size());

    for (int s : sizes) {
        QImage scaled = (std::max(base.width(), base.height()) == s)
                            ? base
                            : base.scaled(s, s, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QImage canvas(s, s, QImage::Format_RGBA8888);
        canvas.fill(Qt::transparent);
        {
            QPainter p(&canvas);
            p.drawImage((s - scaled.width()) / 2, (s - scaled.height()) / 2, scaled);
        }
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        QImageWriter writer(&buf, "png");
        writer.setQuality(0);
        writer.write(canvas);
        layers.push_back(LayerData{s, s, std::move(bytes)});
    }

    std::vector<std::uint8_t> ico;
    auto put16 = [&](std::uint16_t v) {
        ico.push_back(static_cast<std::uint8_t>(v & 0xFF));
        ico.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    };
    auto put32 = [&](std::uint32_t v) {
        ico.push_back(static_cast<std::uint8_t>(v & 0xFF));
        ico.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        ico.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        ico.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    };

    put16(0); put16(1); put16(static_cast<std::uint16_t>(layers.size()));
    std::uint32_t offset = static_cast<std::uint32_t>(6 + layers.size() * 16);
    for (const auto& layer : layers) {
        ico.push_back(layer.w >= 256 ? 0 : static_cast<std::uint8_t>(layer.w));
        ico.push_back(layer.h >= 256 ? 0 : static_cast<std::uint8_t>(layer.h));
        ico.push_back(0); ico.push_back(0);
        put16(1); put16(32);
        put32(static_cast<std::uint32_t>(layer.pngBytes.size()));
        put32(offset);
        offset += static_cast<std::uint32_t>(layer.pngBytes.size());
    }
    for (const auto& layer : layers) ico.insert(ico.end(), layer.pngBytes.begin(), layer.pngBytes.end());

    QFile f(omni::core::pathToQString(outPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("无法写入 ICO 文件");
    f.write(reinterpret_cast<const char*>(ico.data()), static_cast<qint64>(ico.size()));
    f.close();
}

void ImageEngine::convertImageWithOptions(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const omni::core::ConversionOptions& opts,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(12);

    const std::string fmtUpper = toUpperAscii(opts.targetFormat);
    QImage img = loadAnyImage(inPath);

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(32);

    // 1. Geometric transforms: rotation & horizontal mirror
    if (opts.imgRotateDeg != 0 || opts.imgFlipHorizontal) {
        QTransform tf;
        if (opts.imgFlipHorizontal) tf.scale(-1.0, 1.0);
        if (opts.imgRotateDeg != 0) tf.rotate(opts.imgRotateDeg);
        img = img.transformed(tf, Qt::SmoothTransformation);
    }

    // 2. Resolution scaling: maxDim cap OR percentage scale
    if (opts.imgMaxDim > 0 && std::max(img.width(), img.height()) > opts.imgMaxDim) {
        img = img.scaled(opts.imgMaxDim, opts.imgMaxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else if (opts.imgMaxDim == 0 && opts.imgScalePercent > 0 && opts.imgScalePercent != 100) {
        const int nw = std::max(1, img.width() * opts.imgScalePercent / 100);
        const int nh = std::max(1, img.height() * opts.imgScalePercent / 100);
        img = img.scaled(nw, nh, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // 3. Grayscale conversion if requested
    if (opts.imgGrayscale) {
        const bool hadAlpha = img.hasAlphaChannel();
        if (hadAlpha) {
            QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < rgba.height(); ++y) {
                std::uint8_t* p = rgba.scanLine(y);
                for (int x = 0; x < rgba.width(); ++x, p += 4) {
                    const auto g = static_cast<std::uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
                    p[0] = p[1] = p[2] = g;
                }
            }
            img = std::move(rgba);
        } else {
            img = img.convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_RGB888);
        }
    }

    // 4. Optional corner watermark overlay
    if (!opts.imgWatermarkText.empty()) {
        applyCornerWatermark(img, opts.imgWatermarkText);
    }

    // 5. Strip EXIF/text metadata if requested
    if (opts.imgStripExif) {
        QImage clean(img.size(), img.format());
        clean.setColorTable(img.colorTable());
        clean.setColorSpace(img.colorSpace());
        for (int y = 0; y < img.height(); ++y) {
            std::memcpy(clean.scanLine(y), img.constScanLine(y), static_cast<std::size_t>(img.bytesPerLine()));
        }
        img = std::move(clean);
    }

    int quality = 75;
    switch (opts.compLevel) {
        case omni::core::CompressionLevel::Lossless: quality = 96; break;
        case omni::core::CompressionLevel::Light:    quality = 85; break;
        case omni::core::CompressionLevel::Balanced: quality = 72; break;
        case omni::core::CompressionLevel::Extreme:  quality = 50; break;
        case omni::core::CompressionLevel::Custom:   quality = std::clamp(opts.imgCustomQuality, 1, 100); break;
    }
    const bool isLossless = (opts.compLevel == omni::core::CompressionLevel::Lossless) ||
                            (opts.compLevel == omni::core::CompressionLevel::Custom && quality == 100);

    omni::infra::AtomicFileTx tx(outPath);

    if (fmtUpper == "ICO") {
        if (progressCb) progressCb(75);
        writeMultiSizeIco(img, tx.tempPath());
        tx.commit();
        if (progressCb) progressCb(100);
        return;
    }

    if (fmtUpper == "PDF") {
        if (progressCb) progressCb(75);
        writeSingleImagePdf(img, tx.tempPath());
        tx.commit();
        if (progressCb) progressCb(100);
        return;
    }

    const bool hasAlpha = img.hasAlphaChannel();
    if (fmtUpper == "JPG" || fmtUpper == "JPEG" || fmtUpper == "BMP" || fmtUpper == "PPM") {
        if (hasAlpha) {
            QColor bgColor(QString::fromUtf8(opts.imgBgColor.c_str()));
            if (!bgColor.isValid()) bgColor = Qt::white;
            QImage flat(img.size(), QImage::Format_RGB32);
            flat.setColorSpace(img.colorSpace());
            flat.fill(bgColor);
            {
                QPainter painter(&flat);
                painter.drawImage(0, 0, img);
            }
            img = flat.convertToFormat(QImage::Format_RGB888);
        } else if (img.format() != QImage::Format_RGB888) {
            img = img.convertToFormat(QImage::Format_RGB888);
        }
    }

    if (isCancelled && isCancelled()) throw std::runtime_error("CANCELLED");
    if (progressCb) progressCb(68);

    if (fmtUpper == "JPG" || fmtUpper == "JPEG") {
        QImageWriter writer(omni::core::pathToQString(tx.tempPath()), "jpeg");
        writer.setQuality(quality);
        writer.setOptimizedWrite(true);
        if (!writer.write(img)) {
            throw std::runtime_error("JPEG 编码失败: " + writer.errorString().toStdString());
        }
    } else if (fmtUpper == "PNG") {
        QImage outImg = img;
        if (opts.compLevel == omni::core::CompressionLevel::Custom) {
            if (opts.imgPngColors >= 2 && opts.imgPngColors <= 256 && !isLossless) {
                outImg = quantizeColors(img, opts.imgPngColors, hasAlpha);
            }
        } else if (opts.compLevel == omni::core::CompressionLevel::Balanced ||
                   opts.compLevel == omni::core::CompressionLevel::Extreme) {
            const int colors = (opts.compLevel == omni::core::CompressionLevel::Balanced) ? 256 : 128;
            outImg = quantizeColors(img, colors, hasAlpha);
        }
        QImageWriter writer(omni::core::pathToQString(tx.tempPath()), "png");
        writer.setQuality(0); // zlib level 9
        if (!writer.write(outImg)) {
            throw std::runtime_error("PNG 编码失败: " + writer.errorString().toStdString());
        }
    } else if (fmtUpper == "WEBP") {
        bool ok = false;
        if (qtSupportsWrite("webp")) {
            QImageWriter writer(omni::core::pathToQString(tx.tempPath()), "webp");
            writer.setQuality(isLossless ? 100 : quality);
            ok = writer.write(img);
        }
        if (!ok) {
            std::string err;
            ok = encodeViaFfmpeg(img, tx.tempPath(), "WEBP", quality, isLossless, &err);
            if (!ok) throw std::runtime_error("WEBP 编码失败: " + err);
        }
    } else if (fmtUpper == "AVIF") {
        const int avifQ = isLossless ? 95 : quality;
        bool ok = false;
        std::string err;
        if (qtSupportsWrite("avif")) {
            QImageWriter writer(omni::core::pathToQString(tx.tempPath()), "avif");
            writer.setQuality(avifQ);
            ok = writer.write(img);
        }
#ifdef __APPLE__
        if (!ok && macos_imageio::supportsUtiWrite("avif")) {
            ok = macos_imageio::writeImageViaImageIO(img, tx.tempPath(), "AVIF", avifQ, isLossless, &err);
        }
#endif
        if (!ok) {
            ok = encodeViaFfmpeg(img, tx.tempPath(), "AVIF", avifQ, isLossless, &err);
        }
        if (!ok) throw std::runtime_error("AVIF 编码失败: " + err);
    } else if (fmtUpper == "HEIC" || fmtUpper == "HEIF") {
        bool ok = false;
        std::string err;
        if (qtSupportsWrite("heic")) {
            QImageWriter writer(omni::core::pathToQString(tx.tempPath()), "heic");
            writer.setQuality(isLossless ? 100 : quality);
            ok = writer.write(img);
        }
#ifdef __APPLE__
        if (!ok && macos_imageio::supportsUtiWrite("heic")) {
            ok = macos_imageio::writeImageViaImageIO(img, tx.tempPath(), "HEIC", quality, isLossless, &err);
        }
#endif
        if (!ok) ok = encodeViaHeifEnc(img, tx.tempPath(), quality, isLossless, &err);
        if (!ok) throw std::runtime_error("当前环境不支持编码导出 HEIC: " + err);
    } else if (fmtUpper == "TIFF" || fmtUpper == "TIF") {
        QImageWriter writer(omni::core::pathToQString(tx.tempPath()), "tiff");
        writer.setCompression(1);
        if (!writer.write(img)) {
#ifdef __APPLE__
            std::string err;
            if (!macos_imageio::writeImageViaImageIO(img, tx.tempPath(), "TIFF", quality, true, &err)) {
                throw std::runtime_error("TIFF 编码失败: " + err);
            }
#else
            throw std::runtime_error("TIFF 编码失败: " + writer.errorString().toStdString());
#endif
        }
    } else if (fmtUpper == "GIF") {
        int colors = 128;
        if (opts.compLevel == omni::core::CompressionLevel::Custom) {
            colors = std::clamp(opts.imgPngColors, 2, 256);
        } else if (opts.compLevel == omni::core::CompressionLevel::Lossless ||
                   opts.compLevel == omni::core::CompressionLevel::Light) {
            colors = 256;
        } else if (opts.compLevel == omni::core::CompressionLevel::Extreme) {
            colors = 64;
        }
        QImage qImg = quantizeColors(img, colors, hasAlpha);
        writeGif89a(qImg, tx.tempPath());
    } else if (fmtUpper == "TGA") {
        writeTruevisionTga(img, tx.tempPath());
    } else if (fmtUpper == "ICNS") {
        writeAppleIcns(img, tx.tempPath());
    } else if (fmtUpper == "BMP" || fmtUpper == "PPM") {
        QImageWriter writer(omni::core::pathToQString(tx.tempPath()), fmtUpper.c_str());
        if (!writer.write(img)) {
            throw std::runtime_error(fmtUpper + " 编码失败: " + writer.errorString().toStdString());
        }
    } else {
        QImageWriter writer(omni::core::pathToQString(tx.tempPath()), fmtUpper.c_str());
        if (!writer.write(img)) {
            throw std::runtime_error("不支持的图片输出格式: " + fmtUpper);
        }
    }

    tx.commit();
    if (progressCb) progressCb(100);
}

void ImageEngine::convertImage(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    omni::core::CompressionLevel compLevel,
    int maxDim,
    bool stripExif,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    omni::core::ConversionOptions opts;
    opts.targetFormat = targetFormat;
    opts.compLevel = compLevel;
    opts.imgMaxDim = maxDim;
    opts.imgStripExif = stripExif;
    convertImageWithOptions(inPath, outPath, opts, progressCb, isCancelled);
}

} // namespace omni::engines
