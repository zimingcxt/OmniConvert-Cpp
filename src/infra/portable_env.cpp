#include "omni/infra/portable_env.hpp"
#include "omni/core/types.hpp"
#include "omni/infra/atomic_file_tx.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <cstdlib>
#include <system_error>
#include <vector>

namespace omni::infra {

std::filesystem::path PortableEnv::executableDir() {
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        return omni::core::qStringToPath(appDir);
    }
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

std::filesystem::path PortableEnv::appRootDir() {
    const auto exeDir = executableDir();
#ifdef __APPLE__
    // Inside OmniConvert.app/Contents/MacOS -> portable root is the parent folder containing OmniConvert.app
    if (exeDir.filename() == "MacOS" && exeDir.parent_path().filename() == "Contents") {
        const auto bundleDir = exeDir.parent_path().parent_path();
        if (bundleDir.extension() == ".app" && bundleDir.has_parent_path()) {
            return bundleDir.parent_path();
        }
    }
#endif
    return exeDir;
}

std::filesystem::path PortableEnv::portableConfigPath() {
    const auto root = appRootDir();
    const auto portableCfg = root / "omni_settings.json";

    // Check if portableCfg is writable or root directory allows creating files
    QFileInfo existingInfo(omni::core::pathToQString(portableCfg));
    if (existingInfo.exists() && existingInfo.isWritable()) {
        return portableCfg;
    }

    const QString probePath = omni::core::pathToQString(root) +
                              QStringLiteral("/.omni_write_test_%1").arg(QCoreApplication::applicationPid());
    QFile probe(probePath);
    if (probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        probe.write("ok", 2);
        probe.close();
        QFile::remove(probePath);
        return portableCfg;
    }

    // Read-only location (e.g. /Applications or Program Files) -> fallback to ~/.omniconvert/omni_settings.json
    const QString homeDir = QDir::homePath();
    const auto fallbackDir = omni::core::qStringToPath(homeDir) / ".omniconvert";
    std::error_code ec;
    std::filesystem::create_directories(fallbackDir, ec);
    return fallbackDir / "omni_settings.json";
}

PortableSettings PortableEnv::loadSettings() {
    PortableSettings out;
    const auto cfgPath = portableConfigPath();
    QFile f(omni::core::pathToQString(cfgPath));
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        return out;
    }
    const QJsonObject obj = doc.object();
    if (obj.contains("dark_mode")) out.darkMode = obj.value("dark_mode").toBool(true);
    if (obj.contains("last_mode")) out.lastMode = obj.value("last_mode").toString("image").toUtf8().toStdString();
    if (obj.contains("out_dir")) out.outDir = obj.value("out_dir").toString().toUtf8().toStdString();
    if (obj.contains("use_custom_dir")) out.useCustomDir = obj.value("use_custom_dir").toBool(false);
    return out;
}

bool PortableEnv::saveSettings(const PortableSettings& settings) {
    try {
        QJsonObject obj;
        obj.insert("dark_mode", settings.darkMode);
        obj.insert("last_mode", QString::fromUtf8(settings.lastMode.c_str()));
        obj.insert("out_dir", QString::fromUtf8(settings.outDir.c_str()));
        obj.insert("use_custom_dir", settings.useCustomDir);
        const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
        AtomicFileTx::writeStringAtomic(portableConfigPath(), std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        return true;
    } catch (...) {
        return false;
    }
}

bool PortableEnv::ensureExecutable(const std::filesystem::path& binPath) {
    QFileInfo fi(omni::core::pathToQString(binPath));
    if (!fi.exists() || !fi.isFile()) {
        return false;
    }
#ifndef _WIN32
    if (!fi.isExecutable()) {
        QFile f(fi.absoluteFilePath());
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }
    fi.refresh();
    return fi.isExecutable();
#else
    return true;
#endif
}

std::optional<std::filesystem::path> PortableEnv::resolveFfmpeg() {
#ifdef _WIN32
    const std::string exeName = "ffmpeg.exe";
#else
    const std::string exeName = "ffmpeg";
#endif

    const auto root = appRootDir();
    const auto exeDir = executableDir();

    std::vector<std::filesystem::path> candidates = {
        root / "bin" / exeName,
        root / exeName,
        exeDir / "bin" / exeName,
        exeDir / exeName,
    };
#ifdef __APPLE__
    // Also check OmniConvert.app/Contents/Resources/bin/ffmpeg
    if (exeDir.filename() == "MacOS") {
        const auto contentsDir = exeDir.parent_path();
        candidates.push_back(contentsDir / "Resources" / "bin" / exeName);
        candidates.push_back(contentsDir / "Resources" / exeName);
    }
    candidates.emplace_back("/opt/homebrew/bin/ffmpeg");
    candidates.emplace_back("/usr/local/bin/ffmpeg");
#endif

    for (const auto& c : candidates) {
        if (ensureExecutable(c)) {
            return c;
        }
    }

    const QString sysExe = QStandardPaths::findExecutable(QString::fromStdString(exeName));
    if (!sysExe.isEmpty()) {
        const auto p = omni::core::qStringToPath(sysExe);
        if (ensureExecutable(p)) {
            return p;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> PortableEnv::resolveCalibreConvert() {
#ifdef _WIN32
    const QString exeName = QStringLiteral("ebook-convert.exe");
#else
    const QString exeName = QStringLiteral("ebook-convert");
#endif
    const QString sysExe = QStandardPaths::findExecutable(exeName);
    if (!sysExe.isEmpty()) {
        return omni::core::qStringToPath(sysExe);
    }

    std::vector<std::filesystem::path> candidates;
#ifdef __APPLE__
    candidates.emplace_back("/Applications/calibre.app/Contents/MacOS/ebook-convert");
#elif defined(_WIN32)
    for (const char* envKey : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        if (const char* val = std::getenv(envKey)) {
            candidates.push_back(omni::core::pathFromUtf8(val) / "Calibre2" / "ebook-convert.exe");
        }
    }
#endif
    for (const auto& c : candidates) {
        if (ensureExecutable(c)) {
            return c;
        }
    }
    return std::nullopt;
}

std::filesystem::path PortableEnv::resolveSafeOutputPath(
    const std::filesystem::path& inPath,
    const std::filesystem::path& customOutDir,
    const std::string& targetExt,
    const std::string& suffixHint) {

    std::string ext = targetExt;
    for (char& ch : ext) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ext.empty() || ext.front() != '.') {
        ext.insert(ext.begin(), '.');
    }

    const std::filesystem::path folder = customOutDir.empty() ? inPath.parent_path() : customOutDir;
    std::error_code ec;
    if (!folder.empty()) {
        std::filesystem::create_directories(folder, ec);
    }

    const QString stem = omni::core::pathToQString(inPath.stem());
    const QString qExt = QString::fromUtf8(ext.c_str());

    auto isCollision = [&](const std::filesystem::path& cand) -> bool {
        std::error_code e1, e2;
        if (std::filesystem::exists(cand, e1)) return true;
        const auto absIn = std::filesystem::weakly_canonical(inPath, e1);
        const auto absCand = std::filesystem::weakly_canonical(cand, e2);
        return !e1 && !e2 && absIn == absCand;
    };

    std::filesystem::path candidate = folder / omni::core::qStringToPath(stem + qExt);
    if (isCollision(candidate)) {
        const QString tag = suffixHint.empty() ? QStringLiteral("_converted") : QString::fromUtf8(suffixHint.c_str());
        candidate = folder / omni::core::qStringToPath(stem + tag + qExt);
        int idx = 1;
        while (isCollision(candidate)) {
            candidate = folder / omni::core::qStringToPath(QStringLiteral("%1%2_%3%4").arg(stem, tag).arg(idx).arg(qExt));
            ++idx;
        }
    }
    return candidate;
}

void PortableEnv::revealInFileManager(const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::path actual = target;
    if (!std::filesystem::exists(actual, ec)) {
        actual = actual.parent_path();
    }
    if (actual.empty() || !std::filesystem::exists(actual, ec)) {
        return;
    }

    const bool isFile = std::filesystem::is_regular_file(actual, ec);
    const QString qPath = QDir::toNativeSeparators(omni::core::pathToQString(actual));

#ifdef _WIN32
    if (isFile) {
        QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + qPath});
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(qPath));
    }
#elif defined(__APPLE__)
    if (isFile) {
        QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), qPath});
    } else {
        QProcess::startDetached(QStringLiteral("/usr/bin/open"), {qPath});
    }
#else
    const std::filesystem::path dirPath = isFile ? actual.parent_path() : actual;
    QDesktopServices::openUrl(QUrl::fromLocalFile(omni::core::pathToQString(dirPath)));
#endif
}

} // namespace omni::infra
