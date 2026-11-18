#include "omni/engines/media_engine.hpp"
#include "omni/infra/atomic_file_tx.hpp"
#include "omni/infra/portable_env.hpp"

#include <QProcess>
#include <QRegularExpression>
#include <QStringList>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <stdexcept>

namespace omni::engines {

namespace {

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trimSpaces(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Parse "SS", "MM:SS", or "HH:MM:SS(.ms)" into seconds; returns -1.0 if empty/invalid
double parseFlexibleTimeSec(const std::string& raw) {
    const std::string s = trimSpaces(raw);
    if (s.empty()) return -1.0;

    static const QRegularExpression hmsRe(QStringLiteral(R"(^(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)$)"));
    static const QRegularExpression secRe(QStringLiteral(R"(^(\d+(?:\.\d+)?)$)"));
    const QString qs = QString::fromUtf8(s.c_str());

    if (const auto m = hmsRe.match(qs); m.hasMatch()) {
        const double h = m.captured(1).isEmpty() ? 0.0 : m.captured(1).toDouble();
        const double mins = m.captured(2).toDouble();
        const double secs = m.captured(3).toDouble();
        return h * 3600.0 + mins * 60.0 + secs;
    }
    if (const auto m = secRe.match(qs); m.hasMatch()) {
        return m.captured(1).toDouble();
    }
    return -1.0;
}

std::string formatFfmpegSec(double sec) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", std::max(0.0, sec));
    return std::string(buf);
}

} // namespace

void MediaEngine::runFfmpegCommand(
    const std::vector<std::string>& args,
    const std::filesystem::path& outPath,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled,
    double expectedDurationSec) {

    const auto ffmpegOpt = omni::infra::PortableEnv::resolveFfmpeg();
    if (!ffmpegOpt) {
        throw std::runtime_error("未找到离线 FFmpeg 内核，请将 ffmpeg 放入程序同级 ./bin 目录");
    }

    omni::infra::AtomicFileTx tx(outPath);

    QStringList qArgs;
    qArgs << QStringLiteral("-y") << QStringLiteral("-hide_banner");
    for (const auto& a : args) {
        qArgs << QString::fromUtf8(a.c_str());
    }
    qArgs << QStringLiteral("-progress") << QStringLiteral("pipe:1")
          << QStringLiteral("-nostats")
          << omni::core::pathToQString(tx.tempPath());

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef _WIN32
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpArgs) {
        cpArgs->flags |= CREATE_NO_WINDOW;
    });
#endif

    proc.start(omni::core::pathToQString(*ffmpegOpt), qArgs);
    if (!proc.waitForStarted(8000)) {
        throw std::runtime_error("无法启动 FFmpeg 进程");
    }

    static const QRegularExpression durRe(QStringLiteral(R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))"));
    static const QRegularExpression outTimeUsRe(QStringLiteral(R"(^out_time_us=(\d+))"));
    static const QRegularExpression outTimeStrRe(QStringLiteral(R"(^out_time=(\d+):(\d+):(\d+(?:\.\d+)?))"));

    double totalSeconds = expectedDurationSec > 0.0 ? expectedDurationSec : 0.0;
    std::deque<QString> tailLogs;
    QByteArray lineBuffer;

    auto processLine = [&](const QString& rawLine) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) return;

        tailLogs.push_back(line);
        if (tailLogs.size() > 30) tailLogs.pop_front();

        if (totalSeconds <= 0.0) {
            const auto mDur = durRe.match(line);
            if (mDur.hasMatch()) {
                const int h = mDur.captured(1).toInt();
                const int m = mDur.captured(2).toInt();
                const double s = mDur.captured(3).toDouble();
                totalSeconds = std::max(0.1, h * 3600.0 + m * 60.0 + s);
            }
        }

        if (totalSeconds > 0.0 && progressCb) {
            double curSec = -1.0;
            const auto mUs = outTimeUsRe.match(line);
            if (mUs.hasMatch()) {
                curSec = mUs.captured(1).toLongLong() / 1000000.0;
            } else {
                const auto mTs = outTimeStrRe.match(line);
                if (mTs.hasMatch()) {
                    curSec = mTs.captured(1).toInt() * 3600.0 +
                             mTs.captured(2).toInt() * 60.0 +
                             mTs.captured(3).toDouble();
                }
            }
            if (curSec >= 0.0) {
                const int pct = std::clamp(static_cast<int>(curSec / totalSeconds * 100.0), 1, 99);
                progressCb(pct);
            }
        }
    };

    while (proc.state() != QProcess::NotRunning) {
        if (isCancelled && isCancelled()) {
            proc.terminate();
            if (!proc.waitForFinished(2500)) {
                proc.kill();
                proc.waitForFinished(1000);
            }
            tx.rollback();
            throw std::runtime_error("CANCELLED");
        }

        if (proc.waitForReadyRead(120)) {
            lineBuffer.append(proc.readAll());
            int nlPos = -1;
            while ((nlPos = lineBuffer.indexOf('\n')) >= 0) {
                const QByteArray raw = lineBuffer.left(nlPos);
                lineBuffer.remove(0, nlPos + 1);
                processLine(QString::fromUtf8(raw));
            }
        }
    }

    lineBuffer.append(proc.readAll());
    for (const QByteArray& part : lineBuffer.split('\n')) {
        processLine(QString::fromUtf8(part));
    }

    if (isCancelled && isCancelled()) {
        tx.rollback();
        throw std::runtime_error("CANCELLED");
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        tx.rollback();
        QStringList lastLines;
        const std::size_t startIdx = tailLogs.size() > 6 ? tailLogs.size() - 6 : 0;
        for (std::size_t i = startIdx; i < tailLogs.size(); ++i) {
            lastLines << tailLogs[i];
        }
        throw std::runtime_error(
            QStringLiteral("FFmpeg 编码失败 (退出码 %1)：\n%2")
                .arg(proc.exitCode())
                .arg(lastLines.join('\n'))
                .toUtf8()
                .toStdString());
    }

    tx.commit();
    if (progressCb) progressCb(100);
}

void MediaEngine::convertAudioWithOptions(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const omni::core::ConversionOptions& opts,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    const std::string fmt = toLowerAscii(opts.targetFormat);
    std::vector<std::string> args;

    const double startSec = parseFlexibleTimeSec(opts.mediaTrimStart);
    const double endSec = parseFlexibleTimeSec(opts.mediaTrimEnd);
    double clipDur = 0.0;

    if (startSec >= 0.0) {
        args.insert(args.end(), {"-ss", formatFfmpegSec(startSec)});
    }
    if (endSec > 0.0) {
        if (startSec >= 0.0 && endSec > startSec) {
            clipDur = endSec - startSec;
            args.insert(args.end(), {"-t", formatFfmpegSec(clipDur)});
        } else if (startSec < 0.0) {
            clipDur = endSec;
            args.insert(args.end(), {"-t", formatFfmpegSec(clipDur)});
        }
    }

    args.insert(args.end(), {"-i", omni::core::pathToUtf8(inPath), "-vn"});

    std::vector<std::string> afChain;
    if (std::abs(opts.audSpeed - 1.0) > 0.02) {
        const double spd = std::clamp(opts.audSpeed, 0.5, 2.0);
        char sBuf[32];
        std::snprintf(sBuf, sizeof(sBuf), "atempo=%.2f", spd);
        afChain.emplace_back(sBuf);
        if (clipDur > 0.0) clipDur /= spd;
    }
    if (opts.audLoudnorm) {
        afChain.emplace_back("loudnorm=I=-16:TP=-1.5:LRA=11");
    }
    if (!afChain.empty()) {
        std::string joined = afChain[0];
        for (std::size_t i = 1; i < afChain.size(); ++i) joined += "," + afChain[i];
        args.insert(args.end(), {"-af", joined});
    }

    if (opts.audSampleRate > 0 && fmt != "opus") {
        args.insert(args.end(), {"-ar", std::to_string(opts.audSampleRate)});
    }
    if (opts.audChannels == 1 || opts.audChannels == 2) {
        args.insert(args.end(), {"-ac", std::to_string(opts.audChannels)});
    }

    const int customKbps = std::clamp(opts.audCustomBitrateKbps, 16, 512);
    const std::string customBr = std::to_string(customKbps) + "k";

    if (fmt == "mp3") {
        std::string br = "128k";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: br = "320k"; break;
            case omni::core::CompressionLevel::Light:    br = "192k"; break;
            case omni::core::CompressionLevel::Balanced: br = "128k"; break;
            case omni::core::CompressionLevel::Extreme:  br = "64k";  break;
            case omni::core::CompressionLevel::Custom:   br = customBr; break;
        }
        args.insert(args.end(), {"-c:a", "libmp3lame", "-b:a", br});
    } else if (fmt == "m4a" || fmt == "aac") {
        std::string br = "128k";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: br = "256k"; break;
            case omni::core::CompressionLevel::Light:    br = "192k"; break;
            case omni::core::CompressionLevel::Balanced: br = "128k"; break;
            case omni::core::CompressionLevel::Extreme:  br = "64k";  break;
            case omni::core::CompressionLevel::Custom:   br = customBr; break;
        }
        args.insert(args.end(), {"-c:a", "aac", "-b:a", br});
        if (fmt == "m4a") args.insert(args.end(), {"-movflags", "+faststart"});
    } else if (fmt == "flac") {
        const char* lvl = "8";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: lvl = "5";  break;
            case omni::core::CompressionLevel::Light:    lvl = "8";  break;
            case omni::core::CompressionLevel::Balanced: lvl = "12"; break;
            case omni::core::CompressionLevel::Extreme:  lvl = "12"; break;
            case omni::core::CompressionLevel::Custom:   lvl = "10"; break;
        }
        args.insert(args.end(), {"-c:a", "flac", "-compression_level", lvl});
        if (opts.compLevel == omni::core::CompressionLevel::Extreme) {
            args.insert(args.end(), {"-sample_fmt", "s16"});
        }
    } else if (fmt == "wav") {
        args.insert(args.end(), {"-c:a", "pcm_s16le"});
        if (opts.compLevel == omni::core::CompressionLevel::Extreme && opts.audSampleRate == 0 && opts.audChannels == 0) {
            args.insert(args.end(), {"-ar", "22050", "-ac", "1"});
        }
    } else if (fmt == "aiff") {
        args.insert(args.end(), {"-c:a", "pcm_s16be"});
    } else if (fmt == "ogg") {
        std::string q = "4";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: q = "9"; break;
            case omni::core::CompressionLevel::Light:    q = "6"; break;
            case omni::core::CompressionLevel::Balanced: q = "4"; break;
            case omni::core::CompressionLevel::Extreme:  q = "1"; break;
            case omni::core::CompressionLevel::Custom:   q = std::to_string(std::clamp(customKbps / 32, 0, 10)); break;
        }
        args.insert(args.end(), {"-c:a", "libvorbis", "-q:a", q});
    } else if (fmt == "opus") {
        std::string br = "96k";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: br = "192k"; break;
            case omni::core::CompressionLevel::Light:    br = "128k"; break;
            case omni::core::CompressionLevel::Balanced: br = "96k";  break;
            case omni::core::CompressionLevel::Extreme:  br = "48k";  break;
            case omni::core::CompressionLevel::Custom:   br = customBr; break;
        }
        args.insert(args.end(), {"-c:a", "libopus", "-b:a", br});
    } else if (fmt == "ac3") {
        std::string br = (opts.compLevel == omni::core::CompressionLevel::Custom) ? customBr : "192k";
        args.insert(args.end(), {"-c:a", "ac3", "-b:a", br});
    } else if (fmt == "wma") {
        std::string br = (opts.compLevel == omni::core::CompressionLevel::Custom) ? customBr : "128k";
        args.insert(args.end(), {"-c:a", "wmav2", "-b:a", br});
    } else {
        throw std::runtime_error("不支持的音频输出格式: " + opts.targetFormat);
    }

    runFfmpegCommand(args, outPath, progressCb, isCancelled, clipDur);
}

void MediaEngine::convertAudio(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    omni::core::CompressionLevel compLevel,
    int sampleRate,
    bool loudnorm,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    omni::core::ConversionOptions opts;
    opts.targetFormat = targetFormat;
    opts.compLevel = compLevel;
    opts.audSampleRate = sampleRate;
    opts.audLoudnorm = loudnorm;
    convertAudioWithOptions(inPath, outPath, opts, progressCb, isCancelled);
}

void MediaEngine::convertVideoWithOptions(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const omni::core::ConversionOptions& opts,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    const std::string fmt = toLowerAscii(opts.targetFormat);
    std::vector<std::string> args;

    const double startSec = parseFlexibleTimeSec(opts.mediaTrimStart);
    const double endSec = parseFlexibleTimeSec(opts.mediaTrimEnd);
    double clipDur = 0.0;

    if (startSec >= 0.0) {
        args.insert(args.end(), {"-ss", formatFfmpegSec(startSec)});
    }
    if (endSec > 0.0) {
        if (startSec >= 0.0 && endSec > startSec) {
            clipDur = endSec - startSec;
            args.insert(args.end(), {"-t", formatFfmpegSec(clipDur)});
        } else if (startSec < 0.0) {
            clipDur = endSec;
            args.insert(args.end(), {"-t", formatFfmpegSec(clipDur)});
        }
    }

    args.insert(args.end(), {"-i", omni::core::pathToUtf8(inPath)});

    if (fmt == "gif") {
        int fps = 10, defW = 480, maxColors = 128;
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: fps = 15; defW = 800; maxColors = 256; break;
            case omni::core::CompressionLevel::Light:    fps = 12; defW = 640; maxColors = 256; break;
            case omni::core::CompressionLevel::Balanced: fps = 10; defW = 480; maxColors = 128; break;
            case omni::core::CompressionLevel::Extreme:  fps = 8;  defW = 360; maxColors = 64;  break;
            case omni::core::CompressionLevel::Custom:   fps = 12; defW = 640; maxColors = std::clamp(256 - opts.vidCustomCrf * 4, 32, 256); break;
        }
        if (opts.vidFps > 0) fps = std::clamp(opts.vidFps, 1, 50);
        const int targetW = (opts.vidMaxWidth > 0) ? opts.vidMaxWidth : defW;
        const std::string vf =
            "fps=" + std::to_string(fps) +
            ",scale='min(" + std::to_string(targetW) + ",iw)':-1:flags=lanczos,"
            "split[s0][s1];[s0]palettegen=max_colors=" + std::to_string(maxColors) +
            "[p];[s1][p]paletteuse=dither=bayer";
        args.insert(args.end(), {"-vf", vf, "-an"});
        runFfmpegCommand(args, outPath, progressCb, isCancelled, clipDur);
        return;
    }

    if (fmt == "apng") {
        const int fps = (opts.vidFps > 0) ? std::clamp(opts.vidFps, 1, 60) : 12;
        const int targetW = (opts.vidMaxWidth > 0) ? opts.vidMaxWidth : 640;
        const std::string vf = "fps=" + std::to_string(fps) + ",scale='min(" + std::to_string(targetW) + ",iw)':-1:flags=lanczos";
        args.insert(args.end(), {"-vf", vf, "-plays", "0", "-an", "-f", "apng"});
        runFfmpegCommand(args, outPath, progressCb, isCancelled, clipDur);
        return;
    }

    std::string vfChain;
    if (opts.vidFps > 0) {
        vfChain += "fps=" + std::to_string(std::clamp(opts.vidFps, 1, 120)) + ",";
    }
    vfChain += (opts.vidMaxWidth > 0)
                   ? ("scale='trunc(min(" + std::to_string(opts.vidMaxWidth) + ",iw)/2)*2':-2")
                   : "scale=trunc(iw/2)*2:trunc(ih/2)*2";
    args.insert(args.end(), {"-vf", vfChain});

    const int customCrf = std::clamp(opts.vidCustomCrf, 0, 51);

    if (fmt == "webm") {
        std::string crf = "32";
        const char* abr = "96k";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: crf = "18"; abr = "192k"; break;
            case omni::core::CompressionLevel::Light:    crf = "26"; abr = "128k"; break;
            case omni::core::CompressionLevel::Balanced: crf = "32"; abr = "96k";  break;
            case omni::core::CompressionLevel::Extreme:  crf = "38"; abr = "64k";  break;
            case omni::core::CompressionLevel::Custom:   crf = std::to_string(customCrf); abr = "128k"; break;
        }
        args.insert(args.end(), {"-c:v", "libvpx-vp9", "-crf", crf, "-b:v", "0", "-row-mt", "1"});
        if (opts.vidMute) args.push_back("-an");
        else args.insert(args.end(), {"-c:a", "libopus", "-b:a", abr});
    } else if (fmt == "avi") {
        std::string qv = "9";
        switch (opts.compLevel) {
            case omni::core::CompressionLevel::Lossless: qv = "2";  break;
            case omni::core::CompressionLevel::Light:    qv = "5";  break;
            case omni::core::CompressionLevel::Balanced: qv = "9";  break;
            case omni::core::CompressionLevel::Extreme:  qv = "15"; break;
            case omni::core::CompressionLevel::Custom:   qv = std::to_string(std::clamp(1 + customCrf * 30 / 51, 1, 31)); break;
        }
        args.insert(args.end(), {"-c:v", "mpeg4", "-q:v", qv});
        if (opts.vidMute) args.push_back("-an");
        else args.insert(args.end(), {"-c:a", "libmp3lame", "-b:a", "128k"});
    } else if (fmt == "wmv") {
        const char* bv = (opts.compLevel == omni::core::CompressionLevel::Extreme) ? "800k" : "2000k";
        args.insert(args.end(), {"-c:v", "wmv2", "-b:v", bv});
        if (opts.vidMute) args.push_back("-an");
        else args.insert(args.end(), {"-c:a", "wmav2", "-b:a", "128k"});
    } else {
        // MP4 / MKV / MOV / FLV / TS
        const bool isHighPreset = (opts.compLevel == omni::core::CompressionLevel::Lossless ||
                                   opts.compLevel == omni::core::CompressionLevel::Light);
        const char* preset = isHighPreset ? "medium" : "faster";

        if (toLowerAscii(opts.vidCodec) == "h265" && fmt != "flv") {
            std::string crf = "30";
            switch (opts.compLevel) {
                case omni::core::CompressionLevel::Lossless: crf = "20"; break;
                case omni::core::CompressionLevel::Light:    crf = "25"; break;
                case omni::core::CompressionLevel::Balanced: crf = "30"; break;
                case omni::core::CompressionLevel::Extreme:  crf = "35"; break;
                case omni::core::CompressionLevel::Custom:   crf = std::to_string(customCrf); break;
            }
            args.insert(args.end(), {"-c:v", "libx265", "-crf", crf, "-preset", preset, "-pix_fmt", "yuv420p"});
            if (fmt == "mp4" || fmt == "mov") {
                args.insert(args.end(), {"-tag:v", "hvc1"});
            }
        } else {
            std::string crf = "28";
            switch (opts.compLevel) {
                case omni::core::CompressionLevel::Lossless: crf = "18"; break;
                case omni::core::CompressionLevel::Light:    crf = "23"; break;
                case omni::core::CompressionLevel::Balanced: crf = "28"; break;
                case omni::core::CompressionLevel::Extreme:  crf = "33"; break;
                case omni::core::CompressionLevel::Custom:   crf = std::to_string(customCrf); break;
            }
            args.insert(args.end(), {"-c:v", "libx264", "-crf", crf, "-preset", preset, "-pix_fmt", "yuv420p"});
        }

        if (opts.vidMute) {
            args.push_back("-an");
        } else {
            const char* abr = "128k";
            switch (opts.compLevel) {
                case omni::core::CompressionLevel::Lossless: abr = "256k"; break;
                case omni::core::CompressionLevel::Light:    abr = "192k"; break;
                case omni::core::CompressionLevel::Balanced: abr = "128k"; break;
                case omni::core::CompressionLevel::Extreme:  abr = "96k";  break;
                case omni::core::CompressionLevel::Custom:   abr = "160k"; break;
            }
            args.insert(args.end(), {"-c:a", "aac", "-b:a", abr});
        }

        if (fmt == "mp4" || fmt == "mov") {
            args.insert(args.end(), {"-movflags", "+faststart"});
        } else if (fmt == "ts") {
            args.insert(args.end(), {"-f", "mpegts"});
        }
    }

    runFfmpegCommand(args, outPath, progressCb, isCancelled, clipDur);
}

void MediaEngine::convertVideo(
    const std::filesystem::path& inPath,
    const std::filesystem::path& outPath,
    const std::string& targetFormat,
    omni::core::CompressionLevel compLevel,
    int maxWidth,
    const std::string& codecPref,
    bool muteAudio,
    const omni::core::ProgressCallback& progressCb,
    const omni::core::CancelCheckCallback& isCancelled) {

    omni::core::ConversionOptions opts;
    opts.targetFormat = targetFormat;
    opts.compLevel = compLevel;
    opts.vidMaxWidth = maxWidth;
    opts.vidCodec = codecPref;
    opts.vidMute = muteAudio;
    convertVideoWithOptions(inPath, outPath, opts, progressCb, isCancelled);
}

} // namespace omni::engines
