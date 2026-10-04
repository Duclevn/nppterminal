#include "Protocol.h"

#include <windows.h>

#include <array>

namespace nppterminal {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeBase64Char(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

} // namespace

std::string base64Encode(const std::vector<std::uint8_t>& bytes)
{
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);

    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::size_t remaining = bytes.size() - index;
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8 : 0) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0);

        result.push_back(kAlphabet[(value >> 18) & 0x3f]);
        result.push_back(kAlphabet[(value >> 12) & 0x3f]);
        result.push_back(remaining > 1 ? kAlphabet[(value >> 6) & 0x3f] : '=');
        result.push_back(remaining > 2 ? kAlphabet[value & 0x3f] : '=');
    }
    return result;
}

bool base64Decode(const std::string& encoded, std::vector<std::uint8_t>& bytes)
{
    if (encoded.size() % 4 != 0) return false;

    std::vector<std::uint8_t> decoded;
    decoded.reserve((encoded.size() / 4) * 3);

    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const char c0 = encoded[index];
        const char c1 = encoded[index + 1];
        const char c2 = encoded[index + 2];
        const char c3 = encoded[index + 3];
        const int v0 = decodeBase64Char(c0);
        const int v1 = decodeBase64Char(c1);
        const int v2 = c2 == '=' ? 0 : decodeBase64Char(c2);
        const int v3 = c3 == '=' ? 0 : decodeBase64Char(c3);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return false;
        if (c2 == '=' && c3 != '=') return false;
        if (index + 4 != encoded.size() && (c2 == '=' || c3 == '=')) return false;

        const std::uint32_t value = (static_cast<std::uint32_t>(v0) << 18) |
            (static_cast<std::uint32_t>(v1) << 12) |
            (static_cast<std::uint32_t>(v2) << 6) |
            static_cast<std::uint32_t>(v3);
        decoded.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        if (c2 != '=') decoded.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        if (c3 != '=') decoded.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    bytes.swap(decoded);
    return true;
}

std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};

    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length) != length) {
        return {};
    }
    return result;
}

bool utf8ByteStringToBytes(const std::string& value, std::vector<std::uint8_t>& bytes)
{
    std::vector<std::uint8_t> decoded;
    decoded.reserve(value.size());

    for (std::size_t i = 0; i < value.size();) {
        const unsigned char first = static_cast<unsigned char>(value[i]);
        std::uint32_t codePoint = 0;
        std::size_t length = 0;
        if (first <= 0x7f) {
            codePoint = first;
            length = 1;
        } else if ((first & 0xe0) == 0xc0) {
            codePoint = first & 0x1f;
            length = 2;
        } else if ((first & 0xf0) == 0xe0) {
            codePoint = first & 0x0f;
            length = 3;
        } else if ((first & 0xf8) == 0xf0) {
            codePoint = first & 0x07;
            length = 4;
        } else {
            return false;
        }
        if (i + length > value.size()) return false;
        for (std::size_t part = 1; part < length; ++part) {
            const unsigned char next = static_cast<unsigned char>(value[i + part]);
            if ((next & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if ((length == 2 && codePoint < 0x80) ||
            (length == 3 && codePoint < 0x800) ||
            (length == 4 && codePoint < 0x10000) ||
            codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff) ||
            codePoint > 0xff) {
            return false;
        }
        decoded.push_back(static_cast<std::uint8_t>(codePoint));
        i += length;
    }

    bytes.swap(decoded);
    return true;
}

std::string stateName(int state)
{
    switch (state) {
        case 0: return "NoSession";
        case 1: return "Starting";
        case 2: return "Running";
        case 3: return "Stopping";
        case 4: return "Exited";
        case 5: return "Error";
        default: return "Error";
    }
}

} // namespace nppterminal
