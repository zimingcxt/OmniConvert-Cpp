#include "omni/core/dual_pool_scheduler.hpp"
#include "omni/core/types.hpp"
#include "omni/engines/conversion_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/engines/media_engine.hpp"
#include "omni/engines/novel_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/portable_env.hpp"
#include "omni/infra/text_encoding.hpp"
#include "omni/infra/zip_archive.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace omni;

namespace {

void requireTrue(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        throw std::runtime_error(msg);
    }
    std::cout << "[PASS] " << msg << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    // Headless offscreen platform support for CI & terminal verification
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    QTemporaryDir tempDir;
    requireTrue(tempDir.isValid(), "创建临时测试沙盒目录");
    const std::filesystem::path root = core::qStringToPath(tempDir.path());

    // 1. Test ZIP Read/Write & EPUB mimetype uncompressed constraint
    {
        const auto zipPath = root / "test_archive.zip";
        infra::ZipWriter zw;
        zw.addTextFile("mimetype", "application/epub+zip", true);
        zw.addTextFile("folder/hello.txt", "你好，OmniConvert C++20!", false);
        zw.writeToFile(zipPath);

        infra::ZipReader zr;
        requireTrue(zr.open(zipPath), "ZIP 归档重新打开校验");
        requireTrue(zr.readEntryUtf8("mimetype").value_or("") == "application/epub+zip", "ZIP STORE 未压缩条目校验");
        requireTrue(zr.readEntryUtf8("folder/hello.txt").value_or("") == "你好，OmniConvert C++20!", "ZIP DEFLATE 压缩中文条目校验");
    }

    // 2. Test GB18030 <-> UTF-8 Smart Charset Detection
    {
        const std::string original = "第一章 风起云涌\n这是一段用于验证老旧 TXT 小说 GB18030 编码无乱码识别的测试文本。";
        const auto gbBytes = infra::TextEncoding::encodeFromUtf8(original, "gb18030");
        requireTrue(!gbBytes.empty(), "UTF-8 编码至 GB18030 字节流");
        requireTrue(!infra::TextEncoding::isValidUtf8(gbBytes.data(), gbBytes.size()), "GB18030 字节流非 UTF-8 判定");
        const std::string decoded = infra::TextEncoding::decodeSmartToUtf8(gbBytes);
        requireTrue(decoded == original, "GB18030 智能识别还原 UTF-8 零乱码校验");
    }

    // 3. Test Image Engine across PNG (Quantized), JPG, WEBP, ICO (Multi-layer), GIF89a, BMP, TIFF (+ HEIC/AVIF if supported)
    {
        QImage sample(320, 240, QImage::Format_RGBA8888);
        sample.fill(Qt::transparent);
        {
            QPainter p(&sample);
            QLinearGradient grad(0, 0, 320, 240);
            grad.setColorAt(0.0, QColor(59, 130, 246, 255));
            grad.setColorAt(0.5, QColor(16, 185, 129, 220));
            grad.setColorAt(1.0, QColor(239, 68, 68, 180));
            p.fillRect(10, 10, 300, 220, grad);
            p.setPen(Qt::white);
            p.drawText(sample.rect(), Qt::AlignCenter, QStringLiteral("OmniConvert C++/Qt6"));
        }
        const auto srcPng = root / "sample_src.png";
        requireTrue(sample.save(core::pathToQString(srcPng), "PNG"), "生成测试源 PNG 图像");

        std::vector<std::string> imgFormats = {"jpg", "png", "webp", "ico", "gif", "bmp", "tiff"};
        if (engines::ImageEngine::supportsHeic()) imgFormats.push_back("heic");
        if (engines::ImageEngine::supportsAvif()) imgFormats.push_back("avif");

        for (const auto& fmt : imgFormats) {
            const auto outImg = root / ("out_sample." + fmt);
            engines::ImageEngine::convertImage(
                srcPng, outImg, fmt, core::CompressionLevel::Balanced, 256, true, nullptr, nullptr);
            std::error_code ec;
            requireTrue(std::filesystem::exists(outImg, ec) && std::filesystem::file_size(outImg, ec) > 32,
                        "图片转换与多级压缩输出格式: " + fmt);
        }
    }

    // 4. Test Novel Engine: GB18030 TXT -> EPUB / MOBI / PDF / DOCX / HTML / MD / FB2 -> Roundtrip Parse!
    {
        const std::string novelTxt =
            "楔子\n"
            "　　夜色如水，星河低垂。\n"
            "第一章 初入江湖\n"
            "    少年背着行囊推开了木门，迎面吹来凛冽的朔风。\n"
            "    “此去经年，定不负韶华！”\n"
            "第二章 万剑归宗\n"
            "    山巅之上，云海翻涌，剑意纵横三万里。\n";
        const auto gbTxtPath = root / core::pathFromUtf8("《测试武侠录》.txt");
        infra::AtomicFileTx::writeBytesAtomic(gbTxtPath, infra::TextEncoding::encodeFromUtf8(novelTxt, "gb18030"));

        const auto parsed = engines::NovelEngine::parseNovelToDocument(gbTxtPath);
        requireTrue(parsed.title == "测试武侠录", "书名书名号自动净化解析");
        requireTrue(parsed.chapters.size() == 3, "中文章节标题正则精准切分 (楔子 + 第1章 + 第2章)");

        for (const std::string& fmt : {"epub", "mobi", "pdf", "docx", "html", "md", "fb2", "txt"}) {
            const auto outBook = root / ("book_out." + fmt);
            engines::NovelEngine::convertNovel(
                gbTxtPath, outBook, fmt, true, "金古梁",
                fmt == "txt" ? "gb18030" : "utf-8", "a4", nullptr, nullptr);
            std::error_code ec;
            requireTrue(std::filesystem::exists(outBook, ec) && std::filesystem::file_size(outBook, ec) > 64,
                        "小说导出格式校验: " + fmt);

            // Roundtrip read-back verification for all structured formats!
            const auto reparsed = engines::NovelEngine::parseNovelToDocument(outBook);
            requireTrue(!reparsed.chapters.empty() && !reparsed.chapters[0].paragraphs.empty(),
                        "小说反向解析回读完整性校验: " + fmt);
        }
    }

    // 5. Test Audio & Video Engine via FFmpeg (if available in environment)
    if (const auto ff = infra::PortableEnv::resolveFfmpeg()) {
        const auto synthMp4 = root / "synth_input.mp4";
        QProcess genProc;
        genProc.start(core::pathToQString(*ff), {
            QStringLiteral("-y"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("testsrc=duration=1.2:size=320x240:rate=15"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=1.2"),
            QStringLiteral("-c:v"), QStringLiteral("libx264"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-c:a"), QStringLiteral("aac"),
            core::pathToQString(synthMp4)
        });
        if (genProc.waitForFinished(20000) && genProc.exitCode() == 0) {
            // Video -> MP3 audio extraction with loudnorm
            const auto outMp3 = root / "extracted_audio.mp3";
            engines::MediaEngine::convertAudio(
                synthMp4, outMp3, "mp3", core::CompressionLevel::Balanced, 44100, true, nullptr, nullptr);
            requireTrue(std::filesystem::exists(outMp3) && std::filesystem::file_size(outMp3) > 256,
                        "视频提取 MP3 音轨 + loudnorm 响度均衡");

            // Video -> Compressed MP4 (H.264) & Palette GIF
            const auto outVid = root / "compressed_vid.mp4";
            engines::MediaEngine::convertVideo(
                synthMp4, outVid, "mp4", core::CompressionLevel::Extreme, 240, "h264", false, nullptr, nullptr);
            requireTrue(std::filesystem::exists(outVid) && std::filesystem::file_size(outVid) > 256,
                        "视频 H.264 多级压缩导出");

            const auto outGif = root / "animated_palette.gif";
            engines::MediaEngine::convertVideo(
                synthMp4, outGif, "gif", core::CompressionLevel::Balanced, 240, "h264", true, nullptr, nullptr);
            requireTrue(std::filesystem::exists(outGif) && std::filesystem::file_size(outGif) > 256,
                        "视频转高质量调色盘动图 GIF");
        }
    }

    // 6. Test DualPoolScheduler Batch Execution
    {
        std::vector<core::TaskItem> batch;
        for (int i = 0; i < 4; ++i) {
            const auto p = root / ("sched_in_" + std::to_string(i) + ".txt");
            infra::AtomicFileTx::writeStringAtomic(p, "第一章 测试\n段落内容 " + std::to_string(i));
            batch.push_back(core::TaskItem{i, p, core::WorkMode::Novel});
        }
        core::ConversionOptions opts;
        opts.outputDir = root / "batch_out";
        opts.targetFormat = "epub";

        int finishedOk = 0;
        core::DualPoolScheduler sched;
        sched.executeBatch(
            batch,
            opts,
            nullptr,
            [&](const core::TaskResult& r) { if (r.ok) ++finishedOk; },
            [&](int s, int f, std::uintmax_t) {
                requireTrue(s == 4 && f == 0, "DualPoolScheduler 多线程并发批量转换 4/4 成功");
            });
        requireTrue(finishedOk == 4, "DualPoolScheduler 回调计数一致性校验");
    }

    std::cout << "\n=== ALL OMNICONVERT C++/QT6 TESTS PASSED ===\n";
    return 0;
}
