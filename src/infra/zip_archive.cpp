#include "omni/infra/zip_archive.hpp"
#include "omni/core/types.hpp"
#include "omni/infra/atomic_file_tx.hpp"

#include <QFile>
#include <zlib.h>

#include <cstring>
#include <stdexcept>

namespace omni::infra {

namespace {

std::uint16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void writeLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

std::vector<std::uint8_t> rawDeflate(const std::vector<std::uint8_t>& input) {
    if (input.empty()) return {};
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("zlib deflateInit2 失败");
    }
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::vector<std::uint8_t> out(deflateBound(&zs, static_cast<uLong>(input.size())) + 64);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&zs, Z_FINISH);
    const std::size_t produced = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        throw std::runtime_error("zlib 压缩数据块失败");
    }
    out.resize(produced);
    return out;
}

bool rawInflate(const std::uint8_t* compData, std::size_t compLen, std::size_t expectedUncomp, std::vector<std::uint8_t>& out) {
    if (expectedUncomp == 0 && compLen == 0) {
        out.clear();
        return true;
    }
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        return false;
    }
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compData));
    zs.avail_in = static_cast<uInt>(compLen);

    out.clear();
    out.reserve(expectedUncomp > 0 ? expectedUncomp : compLen * 2 + 256);

    std::uint8_t chunk[16384];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(chunk);
        zs.avail_out = sizeof(chunk);
        rc = inflate(&zs, Z_NO_FLUSH);
        const std::size_t have = sizeof(chunk) - zs.avail_out;
        if (have > 0) {
            out.insert(out.end(), chunk, chunk + have);
        }
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) {
            rc = Z_STREAM_END;
            break;
        }
    }
    inflateEnd(&zs);
    return rc == Z_STREAM_END || (rc == Z_OK && out.size() == expectedUncomp);
}

std::string normalizeZipName(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    while (!s.empty() && s.front() == '/') {
        s.erase(s.begin());
    }
    return s;
}

} // namespace

bool ZipReader::open(const std::filesystem::path& zipPath, std::string* errOut) {
    QFile f(omni::core::pathToQString(zipPath));
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "无法打开归档文件: " + omni::core::pathToUtf8(zipPath);
        return false;
    }
    const QByteArray bytes = f.readAll();
    std::vector<std::uint8_t> vec(bytes.begin(), bytes.end());
    return openFromMemory(vec, errOut);
}

bool ZipReader::openFromMemory(const std::vector<std::uint8_t>& rawZip, std::string* errOut) {
    buffer_ = rawZip;
    entries_.clear();

    if (buffer_.size() < 22) {
        if (errOut) *errOut = "ZIP 文件过短或已损坏";
        return false;
    }

    // Locate End of Central Directory (EOCD) signature 0x06054b50 from end
    const std::size_t minSearch = buffer_.size() > 65557 ? buffer_.size() - 65557 : 0;
    std::size_t eocdPos = std::string::npos;
    for (std::size_t i = buffer_.size() - 22;; --i) {
        if (readLe32(&buffer_[i]) == 0x06054B50u) {
            eocdPos = i;
            break;
        }
        if (i == minSearch) break;
    }

    if (eocdPos == std::string::npos) {
        if (errOut) *errOut = "未找到 ZIP 中央目录签名 (EOCD)";
        return false;
    }

    const std::uint16_t totalEntries = readLe16(&buffer_[eocdPos + 10]);
    const std::uint32_t cdOffset = readLe32(&buffer_[eocdPos + 16]);

    std::size_t pos = cdOffset;
    for (std::uint16_t idx = 0; idx < totalEntries && pos + 46 <= buffer_.size(); ++idx) {
        if (readLe32(&buffer_[pos]) != 0x02014B50u) {
            break;
        }
        const std::uint16_t method = readLe16(&buffer_[pos + 10]);
        const std::uint32_t compSz = readLe32(&buffer_[pos + 20]);
        const std::uint32_t uncompSz = readLe32(&buffer_[pos + 24]);
        const std::uint16_t nameLen = readLe16(&buffer_[pos + 28]);
        const std::uint16_t extraLen = readLe16(&buffer_[pos + 30]);
        const std::uint16_t commentLen = readLe16(&buffer_[pos + 32]);
        const std::uint32_t localOff = readLe32(&buffer_[pos + 42]);

        if (pos + 46 + nameLen > buffer_.size()) break;
        std::string name(reinterpret_cast<const char*>(&buffer_[pos + 46]), nameLen);
        name = normalizeZipName(std::move(name));

        if (!name.empty() && name.back() != '/') {
            entries_.push_back(EntryMeta{name, method, compSz, uncompSz, localOff});
        }
        pos += 46 + static_cast<std::size_t>(nameLen) + extraLen + commentLen;
    }
    return true;
}

std::vector<std::string> ZipReader::entryNames() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& e : entries_) {
        names.push_back(e.name);
    }
    return names;
}

bool ZipReader::hasEntry(const std::string& name) const {
    const std::string target = normalizeZipName(name);
    for (const auto& e : entries_) {
        if (e.name == target) return true;
    }
    return false;
}

std::optional<std::vector<std::uint8_t>> ZipReader::readEntry(const std::string& name) const {
    const std::string target = normalizeZipName(name);
    for (const auto& e : entries_) {
        if (e.name != target) continue;
        const std::size_t loc = e.localHeaderOffset;
        if (loc + 30 > buffer_.size() || readLe32(&buffer_[loc]) != 0x04034B50u) {
            return std::nullopt;
        }
        const std::uint16_t nameLen = readLe16(&buffer_[loc + 26]);
        const std::uint16_t extraLen = readLe16(&buffer_[loc + 28]);
        const std::size_t dataStart = loc + 30 + nameLen + extraLen;
        if (dataStart + e.compSize > buffer_.size()) {
            return std::nullopt;
        }
        if (e.method == 0) {
            return std::vector<std::uint8_t>(buffer_.begin() + dataStart, buffer_.begin() + dataStart + e.compSize);
        }
        if (e.method == 8) {
            std::vector<std::uint8_t> inflated;
            if (rawInflate(&buffer_[dataStart], e.compSize, e.uncompSize, inflated)) {
                return inflated;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> ZipReader::readEntryUtf8(const std::string& name) const {
    auto bytes = readEntry(name);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

void ZipWriter::addFile(std::string name, std::vector<std::uint8_t> data, bool storeUncompressed) {
    entries_.push_back(ZipEntry{normalizeZipName(std::move(name)), std::move(data), storeUncompressed});
}

void ZipWriter::addTextFile(std::string name, const std::string& utf8Content, bool storeUncompressed) {
    std::vector<std::uint8_t> bytes(utf8Content.begin(), utf8Content.end());
    addFile(std::move(name), std::move(bytes), storeUncompressed);
}

std::vector<std::uint8_t> ZipWriter::finishToMemory() const {
    struct WrittenRecord {
        std::string name;
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint32_t compSize = 0;
        std::uint32_t uncompSize = 0;
        std::uint32_t localOffset = 0;
    };

    std::vector<std::uint8_t> out;
    out.reserve(65536);
    std::vector<WrittenRecord> records;
    records.reserve(entries_.size());

    for (const auto& entry : entries_) {
        const std::uint32_t crc = static_cast<std::uint32_t>(
            crc32(0L,
                  entry.data.empty() ? Z_NULL : reinterpret_cast<const Bytef*>(entry.data.data()),
                  static_cast<uInt>(entry.data.size())));

        std::uint16_t method = 0;
        std::vector<std::uint8_t> payload;
        if (entry.storeUncompressed || entry.data.empty()) {
            method = 0;
            payload = entry.data;
        } else {
            method = 8;
            payload = rawDeflate(entry.data);
            if (payload.size() >= entry.data.size()) {
                method = 0;
                payload = entry.data;
            }
        }

        const std::uint32_t localOffset = static_cast<std::uint32_t>(out.size());
        const std::uint16_t flags = (1u << 11); // UTF-8 filename flag (EFS)

        writeLe32(out, 0x04034B50u); // Local file header signature
        writeLe16(out, 20);          // Version needed 2.0
        writeLe16(out, entry.storeUncompressed && entry.name == "mimetype" ? 0 : flags);
        writeLe16(out, method);
        writeLe16(out, 0x0000);      // Mod time
        writeLe16(out, 0x5421);      // Mod date (2022-01-01)
        writeLe32(out, crc);
        writeLe32(out, static_cast<std::uint32_t>(payload.size()));
        writeLe32(out, static_cast<std::uint32_t>(entry.data.size()));
        writeLe16(out, static_cast<std::uint16_t>(entry.name.size()));
        writeLe16(out, 0);           // Extra field length = 0 (strictly required for EPUB mimetype!)
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.insert(out.end(), payload.begin(), payload.end());

        records.push_back(WrittenRecord{
            entry.name,
            method,
            crc,
            static_cast<std::uint32_t>(payload.size()),
            static_cast<std::uint32_t>(entry.data.size()),
            localOffset});
    }

    const std::uint32_t cdStart = static_cast<std::uint32_t>(out.size());
    for (const auto& r : records) {
        const std::uint16_t flags = (r.method == 0 && r.name == "mimetype") ? 0 : (1u << 11);
        writeLe32(out, 0x02014B50u); // Central directory signature
        writeLe16(out, 20);          // Version made by
        writeLe16(out, 20);          // Version needed
        writeLe16(out, flags);
        writeLe16(out, r.method);
        writeLe16(out, 0x0000);
        writeLe16(out, 0x5421);
        writeLe32(out, r.crc);
        writeLe32(out, r.compSize);
        writeLe32(out, r.uncompSize);
        writeLe16(out, static_cast<std::uint16_t>(r.name.size()));
        writeLe16(out, 0);
        writeLe16(out, 0);
        writeLe16(out, 0);
        writeLe16(out, 0);
        writeLe32(out, 0);
        writeLe32(out, r.localOffset);
        out.insert(out.end(), r.name.begin(), r.name.end());
    }

    const std::uint32_t cdSize = static_cast<std::uint32_t>(out.size()) - cdStart;
    writeLe32(out, 0x06054B50u); // EOCD
    writeLe16(out, 0);
    writeLe16(out, 0);
    writeLe16(out, static_cast<std::uint16_t>(records.size()));
    writeLe16(out, static_cast<std::uint16_t>(records.size()));
    writeLe32(out, cdSize);
    writeLe32(out, cdStart);
    writeLe16(out, 0);

    return out;
}

void ZipWriter::writeToFile(const std::filesystem::path& outPath) const {
    AtomicFileTx::writeBytesAtomic(outPath, finishToMemory());
}

} // namespace omni::infra
