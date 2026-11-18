#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omni::infra {

struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> data;
    bool storeUncompressed = false;
};

class ZipReader {
public:
    bool open(const std::filesystem::path& zipPath, std::string* errOut = nullptr);
    bool openFromMemory(const std::vector<std::uint8_t>& rawZip, std::string* errOut = nullptr);

    std::vector<std::string> entryNames() const;
    bool hasEntry(const std::string& name) const;
    std::optional<std::vector<std::uint8_t>> readEntry(const std::string& name) const;
    std::optional<std::string> readEntryUtf8(const std::string& name) const;

private:
    struct EntryMeta {
        std::string name;
        std::uint16_t method = 0;
        std::uint32_t compSize = 0;
        std::uint32_t uncompSize = 0;
        std::uint32_t localHeaderOffset = 0;
    };
    std::vector<std::uint8_t> buffer_;
    std::vector<EntryMeta> entries_;
};

class ZipWriter {
public:
    void addFile(std::string name, std::vector<std::uint8_t> data, bool storeUncompressed = false);
    void addTextFile(std::string name, const std::string& utf8Content, bool storeUncompressed = false);

    std::vector<std::uint8_t> finishToMemory() const;
    void writeToFile(const std::filesystem::path& outPath) const;

private:
    std::vector<ZipEntry> entries_;
};

} // namespace omni::infra
