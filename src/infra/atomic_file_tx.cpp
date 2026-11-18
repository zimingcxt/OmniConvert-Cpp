#include "omni/infra/atomic_file_tx.hpp"
#include "omni/core/types.hpp"

#include <QFile>
#include <QUuid>
#include <stdexcept>
#include <system_error>

namespace omni::infra {

AtomicFileTx::AtomicFileTx(std::filesystem::path finalPath)
    : finalPath_(std::move(finalPath)) {
    std::error_code ec;
    const auto parent = finalPath_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    const QString uid = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString ext = omni::core::pathToQString(finalPath_.extension());
    const QString tmpName = QStringLiteral(".omni_tmp_") + uid + ext;
    tempPath_ = parent / omni::core::qStringToPath(tmpName);
}

AtomicFileTx::~AtomicFileTx() {
    if (!committed_) {
        rollback();
    }
}

AtomicFileTx::AtomicFileTx(AtomicFileTx&& other) noexcept
    : finalPath_(std::move(other.finalPath_)),
      tempPath_(std::move(other.tempPath_)),
      committed_(other.committed_) {
    other.committed_ = true;
}

AtomicFileTx& AtomicFileTx::operator=(AtomicFileTx&& other) noexcept {
    if (this != &other) {
        if (!committed_) {
            rollback();
        }
        finalPath_ = std::move(other.finalPath_);
        tempPath_ = std::move(other.tempPath_);
        committed_ = other.committed_;
        other.committed_ = true;
    }
    return *this;
}

void AtomicFileTx::commit() {
    if (committed_) return;
    std::error_code ec;
    if (!std::filesystem::exists(tempPath_, ec)) {
        throw std::runtime_error("临时输出文件未生成: " + omni::core::pathToUtf8(tempPath_));
    }
    // On Windows std::filesystem::rename replaces existing target in C++17,
    // but removing target first avoids edge-case sharing conflicts.
    if (std::filesystem::exists(finalPath_, ec)) {
        std::filesystem::remove(finalPath_, ec);
    }
    std::filesystem::rename(tempPath_, finalPath_, ec);
    if (ec) {
        // Cross-device fallback: copy + remove
        std::error_code copyEc;
        std::filesystem::copy_file(tempPath_, finalPath_, std::filesystem::copy_options::overwrite_existing, copyEc);
        std::filesystem::remove(tempPath_, ec);
        if (copyEc) {
            throw std::runtime_error("写入目标文件失败: " + omni::core::pathToUtf8(finalPath_));
        }
    }
    committed_ = true;
}

void AtomicFileTx::rollback() noexcept {
    if (!tempPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
    }
    committed_ = true;
}

void AtomicFileTx::writeBytesAtomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    AtomicFileTx tx(path);
    QFile f(omni::core::pathToQString(tx.tempPath()));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("无法创建输出文件: " + omni::core::pathToUtf8(path));
    }
    if (!data.empty()) {
        const qint64 written = f.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()));
        if (written != static_cast<qint64>(data.size())) {
            throw std::runtime_error("写入文件数据不完整: " + omni::core::pathToUtf8(path));
        }
    }
    f.flush();
    f.close();
    tx.commit();
}

void AtomicFileTx::writeStringAtomic(const std::filesystem::path& path, const std::string& utf8Text) {
    AtomicFileTx tx(path);
    QFile f(omni::core::pathToQString(tx.tempPath()));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("无法创建输出文件: " + omni::core::pathToUtf8(path));
    }
    if (!utf8Text.empty()) {
        const qint64 written = f.write(utf8Text.data(), static_cast<qint64>(utf8Text.size()));
        if (written != static_cast<qint64>(utf8Text.size())) {
            throw std::runtime_error("写入文本文件不完整: " + omni::core::pathToUtf8(path));
        }
    }
    f.flush();
    f.close();
    tx.commit();
}

} // namespace omni::infra
