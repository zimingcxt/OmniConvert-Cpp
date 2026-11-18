#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace omni::infra {

// RAII transactional file target: writes to a temporary sibling file and atomically renames on commit().
// Automatically cleans up partial temp file on exception, cancellation, or destruction without commit().
class AtomicFileTx {
public:
    explicit AtomicFileTx(std::filesystem::path finalPath);
    ~AtomicFileTx();

    AtomicFileTx(const AtomicFileTx&) = delete;
    AtomicFileTx& operator=(const AtomicFileTx&) = delete;
    AtomicFileTx(AtomicFileTx&& other) noexcept;
    AtomicFileTx& operator=(AtomicFileTx&& other) noexcept;

    const std::filesystem::path& tempPath() const noexcept { return tempPath_; }
    const std::filesystem::path& finalPath() const noexcept { return finalPath_; }

    void commit();
    void rollback() noexcept;

    static void writeBytesAtomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& data);
    static void writeStringAtomic(const std::filesystem::path& path, const std::string& utf8Text);

private:
    std::filesystem::path finalPath_;
    std::filesystem::path tempPath_;
    bool committed_ = false;
};

} // namespace omni::infra
