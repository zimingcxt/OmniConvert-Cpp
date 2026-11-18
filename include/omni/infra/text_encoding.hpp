#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace omni::infra {

class TextEncoding {
public:
    // Intelligently decode UTF-8 / UTF-16LE / UTF-16BE / GB18030 / GBK / Big5 bytes into UTF-8 std::string
    static std::string decodeSmartToUtf8(const std::vector<std::uint8_t>& raw);
    static std::string readFileSmartUtf8(const std::filesystem::path& path);

    // Encode UTF-8 string to target encoding ("utf-8" or "gb18030")
    static std::vector<std::uint8_t> encodeFromUtf8(const std::string& utf8Text, const std::string& targetEncoding);

    static bool isValidUtf8(const std::uint8_t* data, std::size_t len);
};

} // namespace omni::infra
