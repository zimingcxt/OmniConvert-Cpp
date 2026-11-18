#include "omni/infra/text_encoding.hpp"
#include "omni/core/types.hpp"

#include <QFile>
#include <QString>
#include <QStringConverter>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

namespace omni::infra {

namespace {

#ifdef _WIN32
std::optional<std::string> winDecodeCodePage(UINT codePage, const std::uint8_t* data, std::size_t len, bool strict) {
    if (len == 0) return std::string{};
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int wideLen = MultiByteToWideChar(codePage, flags, reinterpret_cast<LPCCH>(data), static_cast<int>(len), nullptr, 0);
    if (wideLen <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(wideLen), L'\0');
    MultiByteToWideChar(codePage, flags, reinterpret_cast<LPCCH>(data), static_cast<int>(len), wide.data(), wideLen);
    return QString::fromStdWString(wide).toUtf8().toStdString();
}

std::optional<std::vector<std::uint8_t>> winEncodeCodePage(UINT codePage, const std::string& utf8Text) {
    if (utf8Text.empty()) return std::vector<std::uint8_t>{};
    const std::wstring wide = QString::fromUtf8(utf8Text.data(), static_cast<qsizetype>(utf8Text.size())).toStdWString();
    const int outLen = WideCharToMultiByte(codePage, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (outLen <= 0) return std::nullopt;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(outLen));
    WideCharToMultiByte(codePage, 0, wide.data(), static_cast<int>(wide.size()), reinterpret_cast<LPSTR>(out.data()), outLen, nullptr, nullptr);
    return out;
}
#else
std::optional<std::vector<std::uint8_t>> posixIconvConvert(
    const char* toCode,
    const char* fromCode,
    const std::uint8_t* data,
    std::size_t len,
    bool strict) {
    if (len == 0) return std::vector<std::uint8_t>{};

    std::string targetSpec = toCode;
    if (!strict) {
        targetSpec += "//IGNORE";
    }
    iconv_t cd = iconv_open(targetSpec.c_str(), fromCode);
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        cd = iconv_open(toCode, fromCode);
        if (cd == reinterpret_cast<iconv_t>(-1)) {
            return std::nullopt;
        }
    }

    std::vector<std::uint8_t> output;
    output.reserve(len * 2 + 64);

    char* inPtr = reinterpret_cast<char*>(const_cast<std::uint8_t*>(data));
    std::size_t inBytesLeft = len;
    char buf[16384];

    while (inBytesLeft > 0) {
        char* outPtr = buf;
        std::size_t outBytesLeft = sizeof(buf);
        const std::size_t res = iconv(cd, &inPtr, &inBytesLeft, &outPtr, &outBytesLeft);
        const std::size_t produced = sizeof(buf) - outBytesLeft;
        if (produced > 0) {
            output.insert(output.end(), reinterpret_cast<std::uint8_t*>(buf), reinterpret_cast<std::uint8_t*>(buf) + produced);
        }
        if (res == static_cast<std::size_t>(-1)) {
            if (errno == E2BIG) {
                continue;
            }
            if (strict) {
                iconv_close(cd);
                return std::nullopt;
            }
            // Skip invalid byte in non-strict mode
            ++inPtr;
            --inBytesLeft;
        }
    }
    iconv_close(cd);
    return output;
}
#endif

} // namespace

bool TextEncoding::isValidUtf8(const std::uint8_t* s, std::size_t len) {
    std::size_t i = 0;
    while (i < len) {
        const std::uint8_t c = s[i];
        if (c <= 0x7F) {
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2 || i + 1 >= len || (s[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len) return false;
            const std::uint8_t b1 = s[i + 1], b2 = s[i + 2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            if (c == 0xE0 && b1 < 0xA0) return false;
            if (c == 0xED && b1 >= 0xA0) return false; // UTF-16 surrogates invalid in UTF-8
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (c > 0xF4 || i + 3 >= len) return false;
            const std::uint8_t b1 = s[i + 1], b2 = s[i + 2], b3 = s[i + 3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
            if (c == 0xF0 && b1 < 0x90) return false;
            if (c == 0xF4 && b1 >= 0x90) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::string TextEncoding::decodeSmartToUtf8(const std::vector<std::uint8_t>& raw) {
    if (raw.empty()) return {};

    // 1. BOM detection
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        return std::string(reinterpret_cast<const char*>(raw.data() + 3), raw.size() - 3);
    }
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        QStringDecoder dec(QStringConverter::Utf16LE);
        QString s = dec.decode(QByteArrayView(reinterpret_cast<const char*>(raw.data() + 2), static_cast<qsizetype>(raw.size() - 2)));
        return s.toUtf8().toStdString();
    }
    if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        QStringDecoder dec(QStringConverter::Utf16BE);
        QString s = dec.decode(QByteArrayView(reinterpret_cast<const char*>(raw.data() + 2), static_cast<qsizetype>(raw.size() - 2)));
        return s.toUtf8().toStdString();
    }

    // Heuristic: UTF-16 without BOM (frequent null bytes on even/odd positions)
    if (raw.size() >= 8 && raw.size() % 2 == 0) {
        std::size_t evenZeros = 0, oddZeros = 0;
        const std::size_t sample = std::min<std::size_t>(raw.size(), 512);
        for (std::size_t i = 0; i < sample; i += 2) {
            if (raw[i] == 0) ++evenZeros;
            if (raw[i + 1] == 0) ++oddZeros;
        }
        if (oddZeros > sample / 6 && evenZeros == 0) {
            QStringDecoder dec(QStringConverter::Utf16LE);
            return QString(dec.decode(QByteArrayView(reinterpret_cast<const char*>(raw.data()), static_cast<qsizetype>(raw.size())))).toUtf8().toStdString();
        }
        if (evenZeros > sample / 6 && oddZeros == 0) {
            QStringDecoder dec(QStringConverter::Utf16BE);
            return QString(dec.decode(QByteArrayView(reinterpret_cast<const char*>(raw.data()), static_cast<qsizetype>(raw.size())))).toUtf8().toStdString();
        }
    }

    // 2. Strict UTF-8 check
    if (isValidUtf8(raw.data(), raw.size())) {
        return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
    }

    // 3. GB18030 (superset of GBK & GB2312) and Big5 fallback
#ifdef _WIN32
    if (auto gb = winDecodeCodePage(54936, raw.data(), raw.size(), true)) {
        return *gb;
    }
    if (auto big5 = winDecodeCodePage(950, raw.data(), raw.size(), true)) {
        return *big5;
    }
    if (auto gbLoose = winDecodeCodePage(54936, raw.data(), raw.size(), false)) {
        return *gbLoose;
    }
#else
    if (auto gb = posixIconvConvert("UTF-8", "GB18030", raw.data(), raw.size(), true)) {
        return std::string(reinterpret_cast<const char*>(gb->data()), gb->size());
    }
    if (auto big5 = posixIconvConvert("UTF-8", "BIG5", raw.data(), raw.size(), true)) {
        return std::string(reinterpret_cast<const char*>(big5->data()), big5->size());
    }
    if (auto gbLoose = posixIconvConvert("UTF-8", "GB18030", raw.data(), raw.size(), false)) {
        return std::string(reinterpret_cast<const char*>(gbLoose->data()), gbLoose->size());
    }
#endif

    // Final tolerant UTF-8 fallback
    return QString::fromUtf8(reinterpret_cast<const char*>(raw.data()), static_cast<qsizetype>(raw.size())).toUtf8().toStdString();
}

std::string TextEncoding::readFileSmartUtf8(const std::filesystem::path& path) {
    QFile f(omni::core::pathToQString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("无法读取文件: " + omni::core::pathToUtf8(path));
    }
    const QByteArray data = f.readAll();
    std::vector<std::uint8_t> vec(data.begin(), data.end());
    return decodeSmartToUtf8(vec);
}

std::vector<std::uint8_t> TextEncoding::encodeFromUtf8(const std::string& utf8Text, const std::string& targetEncoding) {
    std::string enc = targetEncoding;
    std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (enc == "gb18030" || enc == "gbk" || enc == "gb2312") {
#ifdef _WIN32
        if (auto bytes = winEncodeCodePage(54936, utf8Text)) {
            return *bytes;
        }
#else
        if (auto bytes = posixIconvConvert("GB18030", "UTF-8",
                                           reinterpret_cast<const std::uint8_t*>(utf8Text.data()),
                                           utf8Text.size(),
                                           false)) {
            return *bytes;
        }
#endif
    }
    return std::vector<std::uint8_t>(utf8Text.begin(), utf8Text.end());
}

} // namespace omni::infra
