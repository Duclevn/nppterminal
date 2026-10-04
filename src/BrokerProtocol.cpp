#include "BrokerProtocol.h"

#include "Protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace nppterminal {

namespace {

#pragma pack(push, 1)
struct WireHeader {
    std::uint32_t magic = kBrokerProtocolMagic;
    std::uint16_t version = kBrokerProtocolVersion;
    std::uint16_t type = 0;
    std::uint32_t payloadBytes = 0;
    std::uint64_t generation = 0;
    std::uint64_t id = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == kBrokerFrameHeaderBytes,
    "broker frame header must remain fixed width");

constexpr std::uint32_t kStartFlagGitBashPreserveDirectory = 0x8u;

#ifdef NPPTERMINAL_TESTS
constexpr std::uint32_t kStartFlagTestStallOnStop = 0x1u;
constexpr std::uint32_t kStartFlagTestPauseCommandReader = 0x2u;
constexpr std::uint32_t kStartFlagTestBlockInputWrite = 0x4u;
#endif

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned int shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

bool readU16(const std::vector<std::uint8_t>& bytes, std::size_t& offset,
    std::uint16_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
    offset += 2;
    return true;
}

bool readU32(const std::vector<std::uint8_t>& bytes, std::size_t& offset,
    std::uint32_t& value)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = static_cast<std::uint32_t>(bytes[offset]) |
        static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
        static_cast<std::uint32_t>(bytes[offset + 2]) << 16 |
        static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
    offset += 4;
    return true;
}

bool appendString(std::vector<std::uint8_t>& payload, const std::wstring& value,
    std::wstring& error)
{
    const std::vector<std::uint8_t> utf8 = brokerUtf8(value);
    if (value.empty() || utf8.empty() || utf8.size() > std::numeric_limits<std::uint32_t>::max() ||
        std::find(utf8.begin(), utf8.end(), 0) != utf8.end()) {
        error = L"The broker start string is empty or is not valid UTF-8.";
        return false;
    }
    appendU32(payload, static_cast<std::uint32_t>(utf8.size()));
    payload.insert(payload.end(), utf8.begin(), utf8.end());
    return true;
}

bool readString(const std::vector<std::uint8_t>& payload, std::size_t& offset,
    std::wstring& value)
{
    std::uint32_t size = 0;
    if (!readU32(payload, offset, size) || size > kBrokerMaxPayloadBytes ||
        payload.size() - offset < size) return false;
    std::vector<std::uint8_t> bytes(payload.begin() + offset,
        payload.begin() + offset + size);
    offset += size;
    if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) return false;
    value = brokerWide(bytes);
    return !value.empty();
}

} // namespace

std::vector<std::uint8_t> brokerUtf8(const std::wstring& value)
{
    const std::string encoded = wideToUtf8(value);
    return std::vector<std::uint8_t>(encoded.begin(), encoded.end());
}

std::wstring brokerWide(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) return {};
    return utf8ToWide(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool encodeBrokerFrame(BrokerFrameType type, std::uint64_t generation,
    std::uint64_t id, std::uint32_t flags, const std::vector<std::uint8_t>& payload,
    std::vector<std::uint8_t>& encoded)
{
    if (payload.size() > kBrokerMaxPayloadBytes) return false;
    switch (type) {
        case BrokerFrameType::Start:
        case BrokerFrameType::Input:
        case BrokerFrameType::Resize:
        case BrokerFrameType::Stop:
        case BrokerFrameType::TestStallOnStop:
        case BrokerFrameType::Output:
        case BrokerFrameType::State:
        case BrokerFrameType::InputAck:
        case BrokerFrameType::InputWriteStarted:
        case BrokerFrameType::InputWriteFinished:
        case BrokerFrameType::Error:
            break;
        default:
            return false;
    }
    WireHeader header;
    header.type = static_cast<std::uint16_t>(type);
    header.payloadBytes = static_cast<std::uint32_t>(payload.size());
    header.generation = generation;
    header.id = id;
    header.flags = flags;
    encoded.resize(sizeof(header) + payload.size());
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(encoded.data() + sizeof(header), payload.data(), payload.size());
    }
    return true;
}

bool tryDecodeBrokerFrame(std::vector<std::uint8_t>& buffer, BrokerFrame& frame,
    std::wstring& error)
{
    error.clear();
    if (buffer.size() < sizeof(WireHeader)) return false;

    WireHeader header;
    std::memcpy(&header, buffer.data(), sizeof(header));
    if (header.magic != kBrokerProtocolMagic || header.version != kBrokerProtocolVersion ||
        header.reserved != 0) {
        error = L"The broker protocol version or magic is invalid.";
        return false;
    }
    if (header.payloadBytes > kBrokerMaxPayloadBytes) {
        error = L"The broker frame is larger than the protocol limit.";
        return false;
    }
    switch (static_cast<BrokerFrameType>(header.type)) {
        case BrokerFrameType::Start:
        case BrokerFrameType::Input:
        case BrokerFrameType::Resize:
        case BrokerFrameType::Stop:
        case BrokerFrameType::TestStallOnStop:
        case BrokerFrameType::Output:
        case BrokerFrameType::State:
        case BrokerFrameType::InputAck:
        case BrokerFrameType::InputWriteStarted:
        case BrokerFrameType::InputWriteFinished:
        case BrokerFrameType::Error:
            break;
        default:
            error = L"The broker frame type is invalid.";
            return false;
    }
    const std::size_t frameBytes = sizeof(header) + header.payloadBytes;
    if (buffer.size() < frameBytes) return false;

    frame.type = static_cast<BrokerFrameType>(header.type);
    frame.generation = header.generation;
    frame.id = header.id;
    frame.flags = header.flags;
    frame.payload.assign(buffer.begin() + sizeof(header), buffer.begin() + frameBytes);
    buffer.erase(buffer.begin(), buffer.begin() + frameBytes);
    return true;
}

bool encodeBrokerStart(const BrokerStartData& start, std::vector<std::uint8_t>& payload,
    std::wstring& error)
{
    payload.clear();
    payload.reserve(32);
    appendU16(payload, start.columns);
    appendU16(payload, start.rows);
    std::uint32_t flags = start.gitBashPreserveDirectory ?
        kStartFlagGitBashPreserveDirectory : 0;
#ifdef NPPTERMINAL_TESTS
    if (start.testFailThreadStartIndex > 3) {
        error = L"The broker worker-failure test index is invalid.";
        return false;
    }
    flags |= (start.testStallOnStop ? kStartFlagTestStallOnStop : 0) |
        (start.testPauseCommandReader ? kStartFlagTestPauseCommandReader : 0) |
        (start.testBlockInputWrite ? kStartFlagTestBlockInputWrite : 0);
#endif
    appendU32(payload, flags);
#ifdef NPPTERMINAL_TESTS
    // This field exists only in the test protocol build.  Production helpers
    // therefore cannot receive a thread-creation fault-injection request.
    appendU32(payload, start.testFailThreadStartIndex);
#endif
    if (!appendString(payload, start.applicationName, error) ||
        !appendString(payload, start.commandLine, error) ||
        !appendString(payload, start.workingDirectory, error)) {
        payload.clear();
        return false;
    }
    return payload.size() <= kBrokerMaxPayloadBytes;
}

bool decodeBrokerStart(const std::vector<std::uint8_t>& payload, BrokerStartData& start,
    std::wstring& error)
{
    error.clear();
    std::size_t offset = 0;
    std::uint32_t flags = 0;
#ifdef NPPTERMINAL_TESTS
    std::uint32_t failThreadStartIndex = 0;
#endif
    if (!readU16(payload, offset, start.columns) ||
        !readU16(payload, offset, start.rows) ||
        !readU32(payload, offset, flags) || start.columns == 0 || start.rows == 0 ||
        start.columns > 500 || start.rows > 200 ||
#ifdef NPPTERMINAL_TESTS
        !readU32(payload, offset, failThreadStartIndex) ||
#endif
        !readString(payload, offset, start.applicationName) ||
        !readString(payload, offset, start.commandLine) ||
        !readString(payload, offset, start.workingDirectory) || offset != payload.size()) {
        error = L"The broker start frame is malformed.";
        return false;
    }
#ifdef NPPTERMINAL_TESTS
    if ((flags & ~(kStartFlagGitBashPreserveDirectory |
        kStartFlagTestStallOnStop | kStartFlagTestPauseCommandReader |
        kStartFlagTestBlockInputWrite)) != 0 ||
        failThreadStartIndex > 3) {
        error = L"The broker start flags are invalid.";
        return false;
    }
    start.testStallOnStop = (flags & kStartFlagTestStallOnStop) != 0;
    start.testPauseCommandReader = (flags & kStartFlagTestPauseCommandReader) != 0;
    start.testBlockInputWrite = (flags & kStartFlagTestBlockInputWrite) != 0;
    start.testFailThreadStartIndex = failThreadStartIndex;
#else
    if ((flags & ~kStartFlagGitBashPreserveDirectory) != 0) {
        error = L"The broker start flags are invalid.";
        return false;
    }
#endif
    start.gitBashPreserveDirectory =
        (flags & kStartFlagGitBashPreserveDirectory) != 0;
    return true;
}

bool encodeBrokerResize(std::uint16_t columns, std::uint16_t rows,
    std::vector<std::uint8_t>& payload)
{
    if (columns == 0 || rows == 0 || columns > 500 || rows > 200) return false;
    payload.clear();
    appendU16(payload, columns);
    appendU16(payload, rows);
    return true;
}

bool decodeBrokerResize(const std::vector<std::uint8_t>& payload,
    std::uint16_t& columns, std::uint16_t& rows)
{
    std::size_t offset = 0;
    return readU16(payload, offset, columns) && readU16(payload, offset, rows) &&
        offset == payload.size() && columns != 0 && rows != 0 && columns <= 500 && rows <= 200;
}

} // namespace nppterminal
