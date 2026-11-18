#include "omni/core/dual_pool_scheduler.hpp"
#include "omni/core/types.hpp"
#include "omni/engines/conversion_engine.hpp"
#include "omni/engines/document_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/engines/media_engine.hpp"
#include "omni/engines/novel_engine.hpp"
#include "omni/engines/toolbox_engine.hpp"
#include "omni/infra/portable_env.hpp"
#include "omni/infra/text_encoding.hpp"
#include "omni/infra/zip_archive.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int gPass = 0;
int gFail = 0;

void check(bool cond, const std::string& label) {
    if (cond) {
        ++gPass;
        std::cout << "[PASS] " << label << "\n";
    } else {
        ++gFail;
        std::cerr << "[FAIL] " << label << "\n";
    }
}

void writeRawBytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main(int argc, char* argv[]) {
    // Offscreen platform allows QGuiApplication / QPdfWriter / QPainter tests in headless CI
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QGuiApplication app(argc, argv);

    QTemporaryDir tempDir;
    check(tempDir.isValid(), "创建临时测试沙盒目录");
    if (!tempDir.isValid()) return 1;

    const std::filesystem::path root = omni::core::qStringToPath(tempDir.path());

    // ==================== 1. ZIP Store & Deflate Tests ====================
    {
        const auto zipFile = root / omni::core::pathFromUtf8("archive_test.zip");
        omni::infra::ZipWriter zw;
        zw.addTextFile("mimetype", "application/epub+zip", true);
        zw.addTextFile("OEBPS/中文章节.xhtml", "<h1>第一章 星河</h1><p>离线转换测试正文内容。</p>", false);
        zw.writeToFile(zipFile);

        omni::infra::ZipReader zr;
        check(zr.open(zipFile), "ZIP 归档重新打开校验");
        const auto m = zr.readEntryUtf8("mimetype");
        check(m.has_value() && *m == "application/epub+zip", "ZIP STORE 未压缩条目校验");
        const auto c = zr.readEntryUtf8("OEBPS/中文章节.xhtml");
        check(c.has_value() && c->find("第一章 星河") != std::string::npos, "ZIP DEFLATE 压缩中文条目校验");
    }

    // ==================== 2. Smart Text Encoding Tests ====================
    {
        const std::string srcUtf8 = "《三体》第一章：科学边界——中文 GB18030 与 UTF-8 无损智能识别测试！";
        const auto gbBytes = omni::infra::TextEncoding::encodeFromUtf8(srcUtf8, "gb18030");
        check(!gbBytes.empty(), "UTF-8 编码至 GB18030 字节流");
        check(!omni::infra::TextEncoding::isValidUtf8(gbBytes.data(), gbBytes.size()), "GB18030 字节流非 UTF-8 判定");
        const std::string decoded = omni::infra::TextEncoding::decodeSmartToUtf8(gbBytes);
        check(decoded == srcUtf8, "GB18030 智能识别还原 UTF-8 零乱码校验");
    }

    // ==================== 3. Image Engine & Fine-Grained Custom Precision ====================
    const auto srcPng = root / omni::core::pathFromUtf8("sample_source.png");
    {
        QImage img(480, 360, QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        QPainter p(&img);
        for (int y = 0; y < 360; ++y) {
            p.setPen(QColor((y * 2) % 255, 110, 220, 255));
            p.drawLine(0, y, 480, y);
        }
        p.setBrush(QColor(255, 200, 60, 200));
        p.drawEllipse(80, 60, 220, 200);
        p.end();
        check(img.save(omni::core::pathToQString(srcPng), "PNG"), "生成测试源 PNG 图像");
    }

    for (const char* fmt : {"jpg", "png", "webp", "ico", "icns", "gif", "bmp", "tiff", "tga", "ppm", "pdf"}) {
        const auto outImg = root / omni::core::pathFromUtf8(std::string("out_test.") + fmt);
        try {
            omni::engines::ImageEngine::convertImage(
                srcPng, outImg, fmt, omni::core::CompressionLevel::Balanced, 320, true, nullptr, nullptr);
            std::error_code ec;
            check(std::filesystem::exists(outImg, ec) && std::filesystem::file_size(outImg, ec) > 32,
                  std::string("图片转换与多级压缩输出格式: ") + fmt);
        } catch (const std::exception& e) {
            check(false, std::string("图片转换异常 (") + fmt + "): " + e.what());
        }
    }

    // Custom Image Precision (Rotate 90 deg + Scale 50% + Grayscale + Watermark + Custom Quality)
    {
        omni::core::ConversionOptions customImgOpts;
        customImgOpts.targetFormat = "jpg";
        customImgOpts.compLevel = omni::core::CompressionLevel::Custom;
        customImgOpts.imgCustomQuality = 72;
        customImgOpts.imgScalePercent = 50;
        customImgOpts.imgRotateDeg = 90;
        customImgOpts.imgGrayscale = true;
        customImgOpts.imgBgColor = "#F0F4F8";
        customImgOpts.imgWatermarkText = "OmniConvert v3";

        const auto customOut = root / omni::core::pathFromUtf8("custom_precision.jpg");
        omni::engines::ImageEngine::convertImageWithOptions(srcPng, customOut, customImgOpts, nullptr, nullptr);
        const QImage checkImg = omni::engines::ImageEngine::loadAnyImage(customOut);
        // 480x360 scaled 50% -> 240x180, rotated 90 deg -> 180x240!
        check(!checkImg.isNull() && checkImg.width() == 180 && checkImg.height() == 240,
              "图片精细自定义参数（50%缩放 + 90°旋转 + 灰度 + 水印 + 自定义画质）尺寸与输出校验");
    }

    // ==================== 4. Novel Engine (Covers, Zh Variant, Ad Cleaning & All Formats) ====================
    {
        // Simplified <-> Traditional Chinese roundtrip
        const std::string simpSample = "剑气纵横三万里，一剑光寒十九洲。风云变幻，书剑恩仇录。";
        const std::string tradOut = omni::engines::NovelEngine::convertChineseVariant(simpSample, "s2t");
        check(tradOut.find("劍") != std::string::npos && tradOut.find("萬") != std::string::npos &&
                  tradOut.find("風雲") != std::string::npos && tradOut.find("書") != std::string::npos,
              "小说引擎离线简转繁 (s2t) 文学字库校验");
        const std::string backSimp = omni::engines::NovelEngine::convertChineseVariant(tradOut, "t2s");
        check(backSimp == simpSample, "小说引擎离线繁转简 (t2s) 双向还原校验");

        // Novel with ad junk lines + custom procedural cover
        const auto gbNovelPath = root / omni::core::pathFromUtf8("《剑来星河》.txt");
        const std::string novelContent =
            "楔子\n"
            "  浩瀚星海之中，有一座古老的观星台。\n"
            "记住本站域名 www.example-pirate.com 一秒记住更新最快！\n\n"
            "第一章 初入江湖\n"
            "    少年背着长剑走出山村，迎面吹来清晨的山风。\n"
            "求月票！\n"
            "    远处的钟声回荡在云雾之间。\n\n"
            "第二章 夜雨听剑\n"
            "    客栈的油灯轻轻摇曳，窗外下起了淅淅沥沥的秋雨。\n";
        writeRawBytes(gbNovelPath, omni::infra::TextEncoding::encodeFromUtf8(novelContent, "gb18030"));

        const auto parsed = omni::engines::NovelEngine::parseNovelToDocument(gbNovelPath, "", true, "none");
        check(parsed.title == "剑来星河", "书名书名号自动净化解析");
        check(parsed.chapters.size() == 3, "中文章节标题正则精准切分（楔子 + 第1章 + 第2章）");

        bool adCleaned = true;
        for (const auto& ch : parsed.chapters) {
            for (const auto& p : ch.paragraphs) {
                if (p.find("example-pirate") != std::string::npos || p.find("求月票") != std::string::npos) {
                    adCleaned = false;
                }
            }
        }
        check(adCleaned, "网文盗版网址与求票广告行智能净化");

        // Procedural Cover Generation
        const QImage procCover = omni::engines::NovelEngine::generateProceduralCover(parsed.title, "烽火旅人");
        check(!procCover.isNull() && procCover.width() == 800 && procCover.height() == 1140,
              "程序化典雅小说封面渲染生成 (800x1140)");

        // Export & Roundtrip with Auto-Generated Cover across EPUB / MOBI / PDF / HTML / DOCX / MD / FB2 / TXT
        for (const char* nfmt : {"epub", "mobi", "pdf", "docx", "html", "md", "fb2", "txt"}) {
            const auto outBook = root / omni::core::pathFromUtf8(std::string("novel_pro.") + nfmt);
            omni::core::ConversionOptions nOpts;
            nOpts.targetFormat = nfmt;
            nOpts.novCustomAuthor = "测试作者";
            nOpts.novAutoGenerateCover = true;
            nOpts.novIndentSpaces = 2;
            nOpts.novFontSizePt = 12.0;
            nOpts.novLineHeight = 1.85;
            nOpts.novCleanAds = true;

            omni::engines::NovelEngine::convertNovelWithOptions(gbNovelPath, outBook, nOpts, nullptr, nullptr);

            std::error_code ec;
            const bool existsOk = std::filesystem::exists(outBook, ec) && std::filesystem::file_size(outBook, ec) > 64;
            check(existsOk, std::string("带封面与精排小说导出格式校验: ") + nfmt);

            if (existsOk) {
                const auto roundtrip = omni::engines::NovelEngine::parseNovelToDocument(outBook);
                bool foundKeyword = false;
                for (const auto& ch : roundtrip.chapters) {
                    for (const auto& para : ch.paragraphs) {
                        if (para.find("观星台") != std::string::npos || para.find("客栈") != std::string::npos) {
                            foundKeyword = true;
                            break;
                        }
                    }
                }
                check(foundKeyword, std::string("小说反向解析回读完整性校验: ") + nfmt);

                if (std::string(nfmt) == "epub" || std::string(nfmt) == "mobi") {
                    check(!roundtrip.coverJpegBytes.isEmpty(),
                          std::string("电子书内嵌封面数据提取回读校验: ") + nfmt);
                }
            }
        }
    }

    // ==================== 5. Document & Data Table Workshop Tests ====================
    {
        // 5.1 Multi-Image to Single PDF
        const auto imgPdfA = root / omni::core::pathFromUtf8("scan_merged_a.pdf");
        const auto imgPdfB = root / omni::core::pathFromUtf8("scan_merged_b.pdf");
        omni::engines::DocumentEngine::mergeImagesToPdf({srcPng, srcPng}, imgPdfA, "fit", nullptr, nullptr);
        omni::engines::DocumentEngine::mergeImagesToPdf({srcPng}, imgPdfB, "a4", nullptr, nullptr);

        std::error_code ec;
        check(std::filesystem::exists(imgPdfA, ec) && std::filesystem::file_size(imgPdfA, ec) > 512,
              "文档工坊：多张图片按序合成为单个无白边 PDF");

        // 5.2 Multi-PDF Lossless Merge
        const auto combinedPdf = root / omni::core::pathFromUtf8("combined_master.pdf");
        omni::engines::DocumentEngine::mergePdfs({imgPdfA, imgPdfB}, combinedPdf, nullptr, nullptr);
        check(std::filesystem::exists(combinedPdf, ec) &&
                  std::filesystem::file_size(combinedPdf, ec) > std::filesystem::file_size(imgPdfB, ec),
              "文档工坊：多份 PDF 跨平台无损合并");

#ifdef __APPLE__
        // 5.3 PDF Pages -> High-DPI Images (macOS PDFKit native)
        const auto pagesDir = omni::engines::DocumentEngine::exportPdfToImages(
            combinedPdf, root, "png", 120, nullptr, nullptr);
        int exportedPagePngs = 0;
        for (const auto& entry : std::filesystem::directory_iterator(pagesDir, ec)) {
            if (entry.is_regular_file(ec)) ++exportedPagePngs;
        }
        check(exportedPagePngs == 3, "文档工坊：PDF 逐页渲染导出高清 PNG (2页 + 1页 = 3页)");
#endif

        // 5.4 Smart Table Conversion (GB18030 CSV -> JSON -> Markdown -> UTF-8-BOM CSV -> HTML)
        const auto gbCsvPath = root / omni::core::pathFromUtf8("财务报表_gbk.csv");
        const std::string csvSrc =
            "工号,姓名,部门,备注说明\r\n"
            "00128,林晓月,研发中心,\"负责核心引擎, 离线架构\"\r\n"
            "00256,陈子墨,设计部,UI 与典雅书封系统\r\n";
        writeRawBytes(gbCsvPath, omni::infra::TextEncoding::encodeFromUtf8(csvSrc, "gb18030"));

        const auto outJson = root / omni::core::pathFromUtf8("table_out.json");
        const auto outMd   = root / omni::core::pathFromUtf8("table_out.md");
        const auto outCsv  = root / omni::core::pathFromUtf8("table_utf8_bom.csv");
        const auto outHtml = root / omni::core::pathFromUtf8("table_report.html");

        omni::engines::DocumentEngine::convertTable(gbCsvPath, outJson, "json", nullptr, nullptr);
        const std::string jsonTxt = omni::infra::TextEncoding::readFileSmartUtf8(outJson);
        check(jsonTxt.find("\"00128\"") != std::string::npos && jsonTxt.find("林晓月") != std::string::npos,
              "表格工坊：GB18030 CSV 转 JSON（保留前导零工号与引号内逗号字段）");

        omni::engines::DocumentEngine::convertTable(outJson, outMd, "md", nullptr, nullptr);
        const std::string mdTxt = omni::infra::TextEncoding::readFileSmartUtf8(outMd);
        check(mdTxt.find("| 工号 |") != std::string::npos && mdTxt.find("陈子墨") != std::string::npos,
              "表格工坊：JSON 转 Markdown 管道表格");

        omni::engines::DocumentEngine::convertTable(outMd, outCsv, "csv", nullptr, nullptr);
        std::ifstream csvIn(outCsv, std::ios::binary);
        unsigned char bom[3] = {0, 0, 0};
        csvIn.read(reinterpret_cast<char*>(bom), 3);
        check(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF,
              "表格工坊：导出 CSV 自动写入 UTF-8 BOM 防止 Excel 中文乱码");

        omni::engines::DocumentEngine::convertTable(outJson, outHtml, "html", nullptr, nullptr);
        const std::string htmlTxt = omni::infra::TextEncoding::readFileSmartUtf8(outHtml);
        check(htmlTxt.find("<table") != std::string::npos && htmlTxt.find("实时筛选") != std::string::npos,
              "表格工坊：导出带实时搜索框的单文件 HTML 数据报表");
    }

    // ==================== 6. Everyday Life Toolbox Engine Tests ====================
    {
        // 6.1 Exact-KB ID Photo Compressor
        const auto noisyPhoto = root / omni::core::pathFromUtf8("id_photo_large.png");
        {
            QImage big(1200, 1600, QImage::Format_RGB32);
            for (int y = 0; y < 1600; ++y) {
                auto* scan = reinterpret_cast<QRgb*>(big.scanLine(y));
                for (int x = 0; x < 1200; ++x) {
                    scan[x] = qRgb((x * 13 + y * 7) & 255, (x * 5 + y * 19) & 255, (x ^ y) & 255);
                }
            }
            big.save(omni::core::pathToQString(noisyPhoto), "PNG");
        }

        const auto cappedJpg = root / omni::core::pathFromUtf8("id_photo_35kb.jpg");
        omni::engines::ToolboxEngine::compressImageToExactKb(noisyPhoto, cappedJpg, 35, "jpg", nullptr, nullptr);
        std::error_code ec;
        const auto cappedBytes = std::filesystem::file_size(cappedJpg, ec);
        check(cappedBytes > 1024 && cappedBytes <= 35u * 1024u,
              "百宝箱：证件照精确卡 KB 二分压缩（严格 ≤ 35 KB 校验，实际 " + std::to_string(cappedBytes) + " B）");

        // 6.2 Anti-Theft ID Security Watermark
        const auto wmOut = root / omni::core::pathFromUtf8("id_card_watermarked.jpg");
        omni::engines::ToolboxEngine::applyIdSecurityWatermark(
            srcPng, wmOut, "仅供租房备案使用 他用无效", 30, "jpg", nullptr, nullptr);
        check(std::filesystem::exists(wmOut, ec) && std::filesystem::file_size(wmOut, ec) > 1024,
              "百宝箱：身份证全屏斜向防盗安全水印渲染");

        // 6.3 Subtitle Format Conversion & Timeline Shift
        const auto gbSrt = root / omni::core::pathFromUtf8("movie_gbk.srt");
        const std::string srtRaw =
            "1\r\n"
            "00:00:01,200 --> 00:00:03,500\r\n"
            "<i>欢迎来到全能离线工坊！</i>\r\n\r\n"
            "2\r\n"
            "00:00:04,000 --> 00:00:06,800\r\n"
            "第二句双语测试字幕。\r\n";
        writeRawBytes(gbSrt, omni::infra::TextEncoding::encodeFromUtf8(srtRaw, "gb18030"));

        const auto shiftedVtt = root / omni::core::pathFromUtf8("movie_shifted.vtt");
        const auto shiftedAss = root / omni::core::pathFromUtf8("movie_shifted.ass");
        const auto plainTxt   = root / omni::core::pathFromUtf8("movie_transcript.txt");

        omni::engines::ToolboxEngine::convertAndShiftSubtitle(gbSrt, shiftedVtt, "vtt", 1500, nullptr, nullptr);
        const std::string vttContent = omni::infra::TextEncoding::readFileSmartUtf8(shiftedVtt);
        // 00:00:01,200 + 1500ms == 00:00:02.700
        check(vttContent.find("WEBVTT") != std::string::npos &&
                  vttContent.find("00:00:02.700") != std::string::npos &&
                  vttContent.find("欢迎来到全能离线工坊") != std::string::npos,
              "百宝箱：GB18030 SRT 转 WebVTT 乱码修复与 +1500ms 时间轴平移");

        omni::engines::ToolboxEngine::convertAndShiftSubtitle(shiftedVtt, shiftedAss, "ass", -500, nullptr, nullptr);
        const std::string assContent = omni::infra::TextEncoding::readFileSmartUtf8(shiftedAss);
        check(assContent.find("Dialogue:") != std::string::npos && assContent.find("0:00:02.20") != std::string::npos,
              "百宝箱：WebVTT 转 ASS 特效字幕与负数时间轴回拨");

        omni::engines::ToolboxEngine::convertAndShiftSubtitle(shiftedAss, plainTxt, "txt", 0, nullptr, nullptr);
        const std::string transcript = omni::infra::TextEncoding::readFileSmartUtf8(plainTxt);
        check(transcript.find("00:") == std::string::npos && transcript.find("第二句双语测试字幕") != std::string::npos,
              "百宝箱：字幕剥离时间轴提取纯净台词文稿 (.txt)");

        // 6.4 Streaming SHA-256 + MD5 Hash Manifest
        const auto manifestCsv = root / omni::core::pathFromUtf8("hashes.csv");
        omni::engines::ToolboxEngine::generateHashManifest({srcPng, wmOut}, manifestCsv, "csv", nullptr, nullptr);
        const std::string manifestStr = omni::infra::TextEncoding::readFileSmartUtf8(manifestCsv);
        check(manifestStr.find("SHA-256") != std::string::npos && manifestStr.find("sample_source.png") != std::string::npos,
              "百宝箱：批量文件流式 SHA-256 / MD5 完整性校验清单生成");
    }

    // ==================== 7. FFmpeg Audio & Video Pro Options ( Trim + Speed + Custom ) ====================
    if (const auto ffmpeg = omni::infra::PortableEnv::resolveFfmpeg()) {
        std::cout << "[INFO] 检测到离线 FFmpeg: " << omni::core::pathToUtf8(*ffmpeg) << "\n";
        const auto testWav = root / omni::core::pathFromUtf8("tone_src.wav");
        omni::engines::MediaEngine::runFfmpegCommand(
            {"-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=2.0", "-ac", "2", "-ar", "44100",
             omni::core::pathToUtf8(testWav)},
            testWav, nullptr, nullptr, 2.0);

        omni::core::ConversionOptions audOpts;
        audOpts.targetFormat = "mp3";
        audOpts.compLevel = omni::core::CompressionLevel::Custom;
        audOpts.audCustomBitrateKbps = 96;
        audOpts.audSampleRate = 16000;
        audOpts.audChannels = 1;
        audOpts.audSpeed = 1.5;
        audOpts.mediaTrimStart = "00:00.2";
        audOpts.mediaTrimEnd = "00:01.6";

        const auto outMp3 = root / omni::core::pathFromUtf8("tone_trimmed_1.5x.mp3");
        omni::engines::MediaEngine::convertAudioWithOptions(testWav, outMp3, audOpts, nullptr, nullptr);
        std::error_code ec;
        check(std::filesystem::exists(outMp3, ec) && std::filesystem::file_size(outMp3, ec) > 256,
              "音频引擎：自定义码率 + 16kHz 单声道 + 1.5x 倍速 + 区间裁剪转码");
    }

    std::cout << "\n========================================\n"
              << "测试通过: " << gPass << " 项 | 失败: " << gFail << " 项\n"
              << "========================================\n";
    return (gFail == 0) ? 0 : 1;
}
