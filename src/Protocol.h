#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nppterminal {

constexpr std::size_t kMaxOutputChunkBytes = 32u * 1024u;
constexpr std::size_t kMaxInputBytes = 64u * 1024u;

std::string base64Encode(const std::vector<std::uint8_t>& bytes);
bool base64Decode(const std::string& encoded, std::vector<std::uint8_t>& bytes);

std::string wideToUtf8(const std::wstring& value);
std::wstring utf8ToWide(const std::string& value);

// The page uses a JavaScript string with one code unit per byte for mouse and
// other binary terminal input. JSON decoding gives us UTF-8, so map decoded
// code points in the byte range back to their original octets.
bool utf8ByteStringToBytes(const std::string& value, std::vector<std::uint8_t>& bytes);

std::string stateName(int state);

} // namespace nppterminal
