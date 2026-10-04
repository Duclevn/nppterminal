#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nppterminal {

// The broker protocol is deliberately a small binary protocol.  The native
// DLL and the helper use the same header on all three byte-stream channels;
// every frame is length checked before it is copied or dispatched.
constexpr std::uint32_t kBrokerProtocolMagic = 0x4e505442u; // NPTB
constexpr std::uint16_t kBrokerProtocolVersion = 1;
constexpr std::size_t kBrokerMaxPayloadBytes = 128u * 1024u;
constexpr std::size_t kBrokerFrameHeaderBytes = 36u;

enum class BrokerFrameType : std::uint16_t {
    Start = 1,
    Input = 2,
    Resize = 3,
    Stop = 4,
    TestStallOnStop = 5,

    Output = 100,
    State = 101,
    InputAck = 102,
    InputWriteStarted = 103,
    InputWriteFinished = 104,
    Error = 105,
};

struct BrokerFrame {
    BrokerFrameType type = BrokerFrameType::Error;
    std::uint64_t generation = 0;
    std::uint64_t id = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> payload;
};

struct BrokerStartData {
    std::wstring applicationName;
    std::wstring commandLine;
    std::wstring workingDirectory;
    std::uint16_t columns = 80;
    std::uint16_t rows = 24;
    // Fixed child-only environment behavior for Git Bash.  The broker adds
    // CHERE_INVOKING=1 to the child environment without changing its own
    // process environment.
    bool gitBashPreserveDirectory = false;
#ifdef NPPTERMINAL_TESTS
    bool testStallOnStop = false;
    bool testPauseCommandReader = false;
    // Test-only input transport replacement. The broker keeps the pipe read
    // endpoint open without reading it so a bounded input write remains
    // synchronously blocked until the shutdown cancellation path runs.
    bool testBlockInputWrite = false;
    std::uint32_t testFailThreadStartIndex = 0;
#endif
};

bool encodeBrokerFrame(BrokerFrameType type, std::uint64_t generation,
    std::uint64_t id, std::uint32_t flags, const std::vector<std::uint8_t>& payload,
    std::vector<std::uint8_t>& encoded);

// Removes one complete frame from buffer.  Returns true when a frame was
// removed, false when more bytes are needed.  A false return with a non-empty
// error means the stream is malformed and must be closed by the caller.
bool tryDecodeBrokerFrame(std::vector<std::uint8_t>& buffer, BrokerFrame& frame,
    std::wstring& error);

bool encodeBrokerStart(const BrokerStartData& start, std::vector<std::uint8_t>& payload,
    std::wstring& error);
bool decodeBrokerStart(const std::vector<std::uint8_t>& payload, BrokerStartData& start,
    std::wstring& error);

bool encodeBrokerResize(std::uint16_t columns, std::uint16_t rows,
    std::vector<std::uint8_t>& payload);
bool decodeBrokerResize(const std::vector<std::uint8_t>& payload,
    std::uint16_t& columns, std::uint16_t& rows);

std::vector<std::uint8_t> brokerUtf8(const std::wstring& value);
std::wstring brokerWide(const std::vector<std::uint8_t>& bytes);

} // namespace nppterminal
