#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace omni::infra {

struct PortableSettings {
    bool darkMode = true;
    std::string lastMode = "image";
    std::string outDir;
    bool useCustomDir = false;
};

class PortableEnv {
public:
    // Directory containing the portable app (on macOS: folder containing OmniConvert.app; on Win/Linux: exe dir)
    static std::filesystem::path appRootDir();

    // Actual executable binary directory (Contents/MacOS on macOS, exe dir on Windows)
    static std::filesystem::path executableDir();

    // Portable settings file: prefers appRootDir()/omni_settings.json if writable, else ~/.omniconvert/omni_settings.json
    static std::filesystem::path portableConfigPath();

    static PortableSettings loadSettings();
    static bool saveSettings(const PortableSettings& settings);

    // Ensure +x bit on POSIX systems (fixes binaries copied from FAT32/exFAT drives)
    static bool ensureExecutable(const std::filesystem::path& binPath);

    // Offline FFmpeg lookup: portable ./bin -> bundle ./bin -> Resources/bin -> system PATH / Homebrew
    static std::optional<std::filesystem::path> resolveFfmpeg();

    // Optional Calibre ebook-convert lookup
    static std::optional<std::filesystem::path> resolveCalibreConvert();

    // Generate collision-free output path that never overwrites source file
    static std::filesystem::path resolveSafeOutputPath(
        const std::filesystem::path& inPath,
        const std::filesystem::path& customOutDir,
        const std::string& targetExt,
        const std::string& suffixHint = "");

    // Reveal file or folder in macOS Finder / Windows Explorer / Linux file manager
    static void revealInFileManager(const std::filesystem::path& target);
};

} // namespace omni::infra
