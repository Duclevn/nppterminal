#include "Bridge.h"
#include "BrokerProtocol.h"
#include "Protocol.h"
#include "TerminalSession.h"
#include "ProfileCleanup.Tests.h"
#include "WebViewHost.h"
#include "WebViewRender.Tests.h"
#include "WebViewSecurity.Tests.h"
#include "ShellCatalog.Tests.h"
#include "Settings.Tests.h"
#include "ShellSmoke.Tests.h"
#include "TerminalPanel.Tests.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace nppterminal;
using Clock = std::chrono::steady_clock;

struct TestFailure final {
    std::string message;
};

void require(bool condition, const std::string& message)
{
    if (!condition) throw TestFailure{message};
}

std::string narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "<wide-string conversion failed>";
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) <= 0) {
        return "<wide-string conversion failed>";
    }
    return result;
}

template <typename Predicate>
bool waitSessionUntil(TerminalSession& session, Predicate&& predicate, DWORD timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate()) {
        session.poll();
        if (Clock::now() >= deadline) return false;
        ::Sleep(5);
    }
    session.poll();
    return true;
}

struct ResourceSnapshot final {
    DWORD handles = 0;
    std::size_t threads = 0;
};

ResourceSnapshot currentProcessResources()
{
    ResourceSnapshot snapshot;
    require(::GetProcessHandleCount(::GetCurrentProcess(), &snapshot.handles) != FALSE,
        "GetProcessHandleCount failed");

    HANDLE processes = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    require(processes != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed");
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (::Thread32First(processes, &entry)) {
        do {
            if (entry.th32OwnerProcessID == ::GetCurrentProcessId()) ++snapshot.threads;
        } while (::Thread32Next(processes, &entry));
    }
    ::CloseHandle(processes);
    return snapshot;
}

void testProtocol()
{
    const std::vector<std::uint8_t> bytes{0, 1, 2, 0x7f, 0x80, 0xfe, 0xff};
    const std::string encoded = base64Encode(bytes);
    std::vector<std::uint8_t> decoded;
    require(base64Decode(encoded, decoded) && decoded == bytes,
        "base64 round trip failed");
    require(!base64Decode("ab=c", decoded), "invalid base64 padding was accepted");
    require(stateName(static_cast<int>(SessionState::Running)) == "Running",
        "state schema name failed");

    std::vector<std::uint8_t> binary;
    require(utf8ByteStringToBytes("A\xC2\x80\xC3\xBF", binary) &&
        binary == std::vector<std::uint8_t>{'A', 0x80, 0xff},
        "binary input decoding failed");
    require(!utf8ByteStringToBytes("\xC2\x01", binary),
        "invalid binary input was accepted");

    BrokerStartData start;
    start.applicationName = L"C:\\Windows\\System32\\cmd.exe";
    start.commandLine = L"/Q /D";
    start.workingDirectory = L"C:\\Windows";
    start.columns = 100;
    start.rows = 30;
    std::vector<std::uint8_t> startPayload;
    std::wstring brokerError;
    require(encodeBrokerStart(start, startPayload, brokerError),
        "broker start encoding failed");
    std::vector<std::uint8_t> frame;
    require(encodeBrokerFrame(BrokerFrameType::Start, 7, 0, 0, startPayload, frame),
        "broker frame encoding failed");

    std::vector<std::uint8_t> partial(frame.begin(), frame.begin() + 10);
    BrokerFrame decodedFrame;
    require(!tryDecodeBrokerFrame(partial, decodedFrame, brokerError) && brokerError.empty(),
        "broker decoder rejected a partial frame");
    partial.insert(partial.end(), frame.begin() + 10, frame.end());
    require(tryDecodeBrokerFrame(partial, decodedFrame, brokerError) && partial.empty() &&
        decodedFrame.type == BrokerFrameType::Start && decodedFrame.generation == 7,
        "broker frame round trip failed");
    BrokerStartData decodedStart;
    require(decodeBrokerStart(decodedFrame.payload, decodedStart, brokerError) &&
        decodedStart.applicationName == start.applicationName &&
        decodedStart.commandLine == start.commandLine &&
        decodedStart.workingDirectory == start.workingDirectory &&
        decodedStart.columns == start.columns && decodedStart.rows == start.rows,
        "broker start round trip failed");

    std::vector<std::uint8_t> invalid = frame;
    invalid[0] ^= 0xffu;
    require(!tryDecodeBrokerFrame(invalid, decodedFrame, brokerError) && !brokerError.empty(),
        "broker decoder accepted an invalid magic value");
    std::vector<std::uint8_t> oversized(kBrokerMaxPayloadBytes + 1, 0);
    require(!encodeBrokerFrame(BrokerFrameType::Output, 7, 1, 0, oversized, frame),
        "broker encoder accepted an oversized payload");

    std::vector<std::uint8_t> trailing = startPayload;
    trailing.push_back(0);
    require(!decodeBrokerStart(trailing, decodedStart, brokerError),
        "broker start decoder accepted trailing bytes");
}

void testBoundedBridge()
{
    BoundedOutputBridge bridge;
    bridge.beginGeneration(7);
    require(bridge.tryPush(OutputChunk{7, 1, {'a', 'b'}}),
        "bridge did not accept a current-generation chunk");
    require(!bridge.tryPush(OutputChunk{6, 2, {'x'}}),
        "bridge accepted a stale-generation chunk");

    const auto chunk = bridge.takeForSend();
    require(chunk && chunk->id == 1 && bridge.inFlightBytes() == 2,
        "bridge in-flight accounting failed");
    bridge.acknowledge(7, 1);
    require(bridge.inFlightBytes() == 0, "bridge acknowledgement did not release space");

    const wchar_t className[] = L"NppTerminalBridgeTestWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    ::RegisterClassW(&windowClass);
    HWND window = ::CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    require(window != nullptr, "message-only bridge window creation failed");
    constexpr UINT kBridgeMessage = WM_APP + 0x47;
    bridge.attachDispatcher(window, kBridgeMessage);
    require(bridge.tryPush(OutputChunk{7, 3, {'z'}}), "bridge second push failed");
    MSG message{};
    require(::PeekMessageW(&message, window, kBridgeMessage, kBridgeMessage, PM_REMOVE),
        "bridge did not post its first dispatch message");
    const auto second = bridge.takeForSend();
    require(second && second->id == 3, "bridge lost its first dispatch chunk");
    bridge.acknowledge(7, 3);

    // Push while the first notification is being handled. onDispatchHandled
    // must repost after it clears the posted bit, or this chunk is stranded.
    require(bridge.tryPush(OutputChunk{7, 4, {'q'}}), "bridge race push failed");
    bridge.onDispatchHandled();
    require(::PeekMessageW(&message, window, kBridgeMessage, kBridgeMessage, PM_REMOVE),
        "bridge lost a chunk at dispatch handoff");
    const auto third = bridge.takeForSend();
    require(third && third->id == 4, "bridge handoff chunk was not sendable");
    bridge.acknowledge(7, 4);

    // Production-sized output must not repost a queue head that cannot fit in
    // the remaining renderer in-flight capacity. An acknowledgement that
    // makes exactly enough room must resume dispatch.
    bridge.beginGeneration(11);
    const std::vector<std::uint8_t> productionChunk(32u * 1024u, 0x22);
    require(bridge.tryPush(OutputChunk{11, 1, {'x'}}),
        "bridge partial-capacity setup rejected the one-byte chunk");
    for (std::uint64_t id = 2; id <= 9; ++id) {
        require(bridge.tryPush(OutputChunk{11, id, productionChunk}),
            "bridge partial-capacity setup rejected a production-sized chunk");
    }
    require(::PeekMessageW(&message, window, kBridgeMessage, kBridgeMessage, PM_REMOVE),
        "bridge did not post the partial-capacity dispatch message");
    for (std::uint64_t id = 1; id <= 8; ++id) {
        const auto sent = bridge.takeForSend();
        require(sent && sent->id == id,
            "bridge partial-capacity setup did not preserve queue order");
    }
    require(!bridge.takeForSend(), "bridge sent a chunk beyond in-flight capacity");
    bridge.onDispatchHandled();
    require(!::PeekMessageW(&message, window, kBridgeMessage, kBridgeMessage, PM_REMOVE),
        "bridge reposted without enough in-flight capacity for the queue head");
    bridge.acknowledge(11, 1);
    require(::PeekMessageW(&message, window, kBridgeMessage, kBridgeMessage, PM_REMOVE),
        "bridge did not resume dispatch after enough acknowledgement capacity");
    const auto resumed = bridge.takeForSend();
    require(resumed && resumed->id == 9,
        "bridge did not resume with the blocked queue head");
    bridge.acknowledge(11, 9);

    bridge.beginGeneration(10);
    const std::vector<std::uint8_t> inFlightBlock(128u * 1024u, 0x33);
    require(bridge.tryPush(OutputChunk{10, 1, inFlightBlock}) &&
        bridge.tryPush(OutputChunk{10, 2, inFlightBlock}) &&
        bridge.tryPush(OutputChunk{10, 3, inFlightBlock}),
        "bridge in-flight setup failed");
    require(bridge.takeForSend() && bridge.takeForSend() && !bridge.takeForSend(),
        "bridge exceeded its in-flight byte limit");
    bridge.acknowledge(10, 1);
    bridge.acknowledge(10, 2);

    // A full bounded queue rejects a new chunk without blocking the producer.
    bridge.beginGeneration(9);
    const std::vector<std::uint8_t> block(1024u * 1024u, 0x5a);
    for (std::uint64_t id = 1; id <= 4; ++id) {
        require(bridge.tryPush(OutputChunk{9, id, block}),
            "bridge did not fill its bounded queue");
    }
    require(!bridge.tryPush(OutputChunk{9, 5, block}),
        "bridge accepted a chunk beyond its bounded queue");
    bridge.cancel();
    ::DestroyWindow(window);
    ::UnregisterClassW(className, windowClass.hInstance);
    bridge.beginGeneration(8);
    require(bridge.generation() == 8 && bridge.queuedBytes() == 0,
        "bridge generation reset failed");
    require(bridge.tryPush(OutputChunk{8, 6, {'n'}}),
        "nonblocking bridge push rejected a current-generation chunk");
    require(!bridge.tryPush(OutputChunk{7, 7, {'s'}}),
        "nonblocking bridge push accepted a stale-generation chunk");
    bridge.cancel();
    require(!bridge.tryPush(OutputChunk{8, 8, {'c'}}),
        "nonblocking bridge push ignored cancellation");
}

std::wstring cmdPath()
{
    wchar_t directory[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(directory, MAX_PATH);
    require(length != 0 && length < MAX_PATH, "GetSystemDirectoryW failed");
    return std::wstring(directory, length) + L"\\cmd.exe";
}

std::wstring currentDirectory()
{
    wchar_t directory[MAX_PATH * 4] = {};
    const DWORD length = ::GetCurrentDirectoryW(
        static_cast<DWORD>(std::size(directory)), directory);
    require(length != 0 && length < std::size(directory), "GetCurrentDirectoryW failed");
    return std::wstring(directory, length);
}

struct SessionProbe final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    SessionState latestState = SessionState::NoSession;
    std::wstring latestMessage;
    std::vector<SessionState> states;
    std::string output;
    std::vector<std::uint64_t> acknowledgements;
    std::atomic_bool blockOutput{false};
    std::atomic_bool outputEntered{false};
    std::atomic_bool inputWriteStarted{false};
    std::atomic_bool inputWriteFinished{false};
    std::atomic_bool inputWriteSucceeded{false};
    std::atomic<std::uint64_t> inputWriteStartedCount{0};
    std::atomic<std::uint64_t> inputWriteFinishedCount{0};
    std::atomic<std::uint64_t> inputWriteActiveId{0};
    bool streamVerification = false;
    std::string streamPending;
    std::uint64_t streamLines = 0;
    std::size_t streamBytes = 0;
    std::size_t streamMaxPending = 0;
    bool streamInvalid = false;
    std::string streamFirstInvalid;
    std::string streamFirstExpected;

    static void stripTerminalControls(std::string& line)
    {
        std::string clean;
        clean.reserve(line.size());
        for (std::size_t index = 0; index < line.size();) {
            const unsigned char value = static_cast<unsigned char>(line[index]);
            if (value == 0x1b && index + 1 < line.size()) {
                const unsigned char kind = static_cast<unsigned char>(line[index + 1]);
                index += 2;
                if (kind == '[') {
                    while (index < line.size()) {
                        const unsigned char final = static_cast<unsigned char>(line[index++]);
                        if (final >= 0x40 && final <= 0x7e) break;
                    }
                } else if (kind == ']') {
                    while (index < line.size()) {
                        const unsigned char next = static_cast<unsigned char>(line[index++]);
                        if (next == 0x07) break;
                        if (next == 0x1b && index < line.size() && line[index] == '\\') {
                            ++index;
                            break;
                        }
                    }
                }
                continue;
            }
            if (value < 0x20 && value != '\t') {
                ++index;
                continue;
            }
            clean.push_back(line[index++]);
        }
        line = std::move(clean);
    }

    void consumeStream(const std::vector<std::uint8_t>& data)
    {
        streamBytes += data.size();
        streamPending.append(reinterpret_cast<const char*>(data.data()), data.size());
        if (streamPending.size() > 1024u * 1024u) {
            streamInvalid = true;
            streamPending.clear();
            return;
        }
        for (;;) {
            const std::size_t end = streamPending.find('\n');
            if (end == std::string::npos) {
                streamMaxPending = std::max(streamMaxPending, streamPending.size());
                return;
            }
            std::string line = streamPending.substr(0, end);
            streamPending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // ConPTY emits terminal-mode and title updates around the first
            // application bytes. Strip only those control sequences; every
            // producer line remains compared byte-for-byte after that.
            stripTerminalControls(line);
            const std::string expected = "NPP_STREAM_" + std::to_string(streamLines + 1);
            if (line != expected) {
                streamInvalid = true;
                if (streamFirstInvalid.empty()) {
                    streamFirstInvalid = line;
                    streamFirstExpected = expected;
                }
            }
            ++streamLines;
        }
    }

    SessionCallbacks callbacks()
    {
        return SessionCallbacks{
            [this](std::uint64_t, std::uint64_t, const std::vector<std::uint8_t>& data,
                const std::atomic_bool&) {
                if (blockOutput.load()) {
                    outputEntered.store(true);
                    return false;
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (streamVerification) {
                    consumeStream(data);
                } else {
                    output.append(reinterpret_cast<const char*>(data.data()), data.size());
                }
                condition.notify_all();
                return true;
            },
            [this](std::uint64_t, SessionState state, const std::wstring& message) {
                std::lock_guard<std::mutex> lock(mutex);
                latestState = state;
                latestMessage = message;
                states.push_back(state);
                condition.notify_all();
            },
            [this](std::uint64_t, std::uint64_t id) {
                std::lock_guard<std::mutex> lock(mutex);
                acknowledgements.push_back(id);
                condition.notify_all();
            },
#ifdef NPPTERMINAL_TESTS
            [this](std::uint64_t, std::uint64_t id) {
                inputWriteStarted.store(true);
                inputWriteStartedCount.fetch_add(1);
                inputWriteActiveId.store(id);
            },
            [this](std::uint64_t, std::uint64_t id, bool succeeded) {
                inputWriteSucceeded.store(succeeded);
                inputWriteFinished.store(true);
                inputWriteFinishedCount.fetch_add(1);
                inputWriteActiveId.compare_exchange_strong(id, 0);
            }
#endif
        };
    }

    bool hasState(SessionState state) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(states.begin(), states.end(), state) != states.end();
    }

    std::string outputCopy() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return output;
    }

    std::wstring latestMessageCopy() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return latestMessage;
    }
};

SessionStartOptions optionsFor(const std::wstring& commandLine)
{
    SessionStartOptions options;
    options.applicationName = cmdPath();
    options.commandLine = commandLine;
    options.workingDirectory = currentDirectory();
    options.columns = 100;
    options.rows = 30;
    return options;
}

void testGenerationRejection()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    SessionStartOptions options = optionsFor(L"/Q /D");
    options.expectedGeneration = 9;
    std::wstring error;
    require(!session.start(options, error), "stale generation start was accepted");
    session.poll();
    require(session.generation() == 1 && session.state() == SessionState::Error &&
        !session.hasProcess(), "stale generation changed native session ownership");
}

void waitForRunning(TerminalSession& session, SessionProbe& probe)
{
    const bool reachedRunning = waitSessionUntil(session, [&] {
        return session.state() == SessionState::Running || probe.hasState(SessionState::Error);
    }, 5000);
    const std::wstring message = probe.latestMessageCopy();
    require(reachedRunning, "session did not reach Running; last broker message: " +
        (message.empty() ? std::string("<none>") : narrow(message)));
    require(session.state() == SessionState::Running, "session entered Error during startup: " +
        (message.empty() ? std::string("<none>") : narrow(message)));
}

void warmupNormalSessionResources()
{
    // The first native process in a test process can initialize Windows
    // process/ConPTY support lazily.  Establish that one-time cost with a
    // real normal session before fault-fixture resource baselines are taken.
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    std::wstring error;
    require(session.start(optionsFor(L"/Q /D"), error),
        "resource-warmup start was rejected");
    waitForRunning(session, probe);
    session.requestStop(StopReason::HostShutdown);
    require(session.waitForStop(5000), "resource-warmup stop did not complete");
    require(!session.hasProcess() && !session.hasPendingWork(),
        "resource-warmup retained native work");
    ::Sleep(100);
}

void waitForExited(TerminalSession& session, SessionProbe& probe)
{
    require(waitSessionUntil(session, [&] {
        const bool terminal = session.state() == SessionState::Exited ||
            probe.hasState(SessionState::Error);
        return terminal && !session.hasPendingWork();
    }, 10000), "session did not reach Exited");
    require(session.state() == SessionState::Exited, "session exited through Error");
}

void testNaturalExitAndFinalTail()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    SessionStartOptions options = optionsFor(
        L"/Q /D /C \"for %A in (NPP) do @echo %A_NATIVE_FINAL_TAIL&exit /b 0\"");
    std::wstring error;
    require(session.start(options, error), "asynchronous natural-exit start was rejected");
    // TerminalSession is caller-thread affine and delivers every callback,
    // including Starting, from poll(). Start itself only queues the state.
    session.poll();
    require(probe.hasState(SessionState::Starting),
        "first poll did not publish Starting");
    waitForExited(session, probe);
    require(!session.hasPendingWork(), "natural exit retained pending session work");
    const std::string output = probe.outputCopy();
    require(output.find("NPP_NATIVE_FINAL_TAIL") != std::string::npos,
        "natural exit lost the final output tail");
    // A natural exit must release the broker and host resources from poll;
    // waitForStop would hide a missing automatic cleanup path.
    require(!session.hasProcess() && session.processId() == 0,
        "natural exit retained process resources");
}

void testInteractiveInputResizeAndKill()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    SessionStartOptions options = optionsFor(L"/Q /D");
    std::wstring error;
    require(session.start(options, error), "interactive start was rejected");
    waitForRunning(session, probe);
    require(session.resize(120, 40), "running resize was rejected");
    const std::vector<std::uint8_t> input{
        'e', 'c', 'h', 'o', ' ', '%', 'C', 'o', 'm', 'S', 'p', 'e', 'c', '%', '\r',
        'e', 'x', 'i', 't', '\r'};
    require(session.write(session.generation(), 41, input), "interactive input was rejected");
    require(waitSessionUntil(session, [&] {
        std::lock_guard<std::mutex> lock(probe.mutex);
        return std::find(probe.acknowledgements.begin(), probe.acknowledgements.end(), 41) !=
            probe.acknowledgements.end();
    }, 5000), "input acknowledgement was not delivered after WriteFile completed");
    waitForExited(session, probe);
    const std::string output = probe.outputCopy();
    require(output.find("\\Windows\\System32\\cmd.exe") != std::string::npos ||
        output.find("\\system32\\cmd.exe") != std::string::npos,
        "interactive cmd output was not observed (echo alone is insufficient)");
    require(session.waitForStop(5000), "interactive supervisor did not join");

    SessionProbe killProbe;
    TerminalSession killed(killProbe.callbacks());
    require(killed.start(optionsFor(L"/Q /D"), error), "kill-cycle start was rejected");
    waitForRunning(killed, killProbe);
    killed.requestStop(StopReason::UserKill);
    require(waitSessionUntil(killed, [&] { return killed.state() == SessionState::NoSession; }, 5000),
        "kill did not complete promptly");
    require(killed.waitForStop(5000), "kill supervisor did not join");
    require(!killed.hasProcess() && killed.processId() == 0,
        "kill retained process resources");
}

std::size_t childProcessCount(DWORD parentId)
{
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parentId) ++count;
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return count;
}

DWORD firstChildProcessId(DWORD parentId)
{
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD childId = 0;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parentId) {
                childId = entry.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return childId;
}

void testNaturalExitDescendantCleanup()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    std::wstring error;
    require(session.start(optionsFor(
        L"/Q /D /C \"start \"\" /b ping -n 30 127.0.0.1 >nul&ping -n 3 127.0.0.1 >nul&exit /b 0\""), error),
        "descendant-cleanup start was rejected");
    require(waitSessionUntil(session, [&] { return session.state() == SessionState::Running; }, 5000),
        "descendant-cleanup session did not reach Running");
    const DWORD rootProcessId = session.processId();
    require(rootProcessId != 0, "descendant-cleanup did not publish a process id");
    require(waitSessionUntil(session, [&] { return childProcessCount(rootProcessId) != 0; }, 5000),
        "descendant-cleanup did not observe a native child");
    waitForExited(session, probe);
    require(session.waitForStop(5000), "descendant-cleanup supervisor did not join");
    ::Sleep(200);
    require(!session.hasProcess() && session.processId() == 0,
        "descendant-cleanup retained process resources");
    require(childProcessCount(rootProcessId) == 0,
        "natural shell exit left a native descendant behind");
}

void testStartupCancellation()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    std::wstring error;
    require(session.start(optionsFor(L"/Q /D"), error),
        "startup-cancellation start was rejected");
    session.requestStop(StopReason::UserKill);
    require(waitSessionUntil(session, [&] { return session.state() == SessionState::NoSession; }, 5000),
        "startup cancellation did not reach NoSession");
    require(session.waitForStop(5000), "startup-cancellation supervisor did not join");
    require(!session.hasProcess(), "startup cancellation retained a process handle");
}

void testRepeatedCycles(std::uint64_t cycles)
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    ResourceSnapshot baseline{};
    const std::uint64_t baselineCycle = std::min<std::uint64_t>(cycles, 4) - 1;
    long long worstStopMs = 0;
    for (std::uint64_t cycle = 0; cycle != cycles; ++cycle) {
        std::wstring error;
        const auto runNaturalExit = [&] {
            const std::uint64_t previousGeneration = session.generation();
            require(session.start(optionsFor(L"/Q /D /C \"exit /b 0\""), error),
                "repeated-cycle natural start was rejected");
            require(waitSessionUntil(session, [&] {
                return session.generation() > previousGeneration &&
                    session.state() == SessionState::Exited;
            }, 10000), "repeated-cycle natural session did not exit");
            require(session.waitForStop(5000), "repeated-cycle natural supervisor did not join");
        };
        const auto runExplicitStop = [&](StopReason reason) {
            require(session.start(optionsFor(L"/Q /D"), error),
                "repeated-cycle stop start was rejected");
            waitForRunning(session, probe);
            const auto stopStarted = Clock::now();
            session.requestStop(reason);
            require(session.waitForStop(5000), "repeated-cycle stop supervisor did not join");
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - stopStarted).count();
            worstStopMs = std::max(worstStopMs, elapsed);
            require(elapsed < 2000, "repeated-cycle explicit stop exceeded the two-second target");
            require(session.state() == SessionState::NoSession,
                "repeated-cycle explicit stop did not reach NoSession");
        };

        if (cycle % 3 == 0) {
            runNaturalExit();
        } else if (cycle % 3 == 1) {
            runExplicitStop(StopReason::UserKill);
        } else {
            runExplicitStop(StopReason::Restart);
            runNaturalExit();
        }
        require(!session.hasProcess() && session.processId() == 0,
            "repeated-cycle process resources leaked");
        if (cycle == baselineCycle) {
            ::Sleep(100);
            baseline = currentProcessResources();
        }
    }
    ::Sleep(100);
    const ResourceSnapshot settled = currentProcessResources();
    require(settled.handles <= baseline.handles + 2,
        "repeated-cycle handle count rose above the settled baseline");
    require(settled.threads <= baseline.threads + 1,
        "repeated-cycle thread count rose above the settled baseline");
    std::wcout << L"cycles=" << cycles << L" baseline_handles=" << baseline.handles
        << L" settled_handles=" << settled.handles << L" baseline_threads="
        << baseline.threads << L" settled_threads=" << settled.threads
        << L" worst_stop_ms=" << worstStopMs << L"\n";
}

void testBlockedStop()
{
    std::wstring error;

    SessionProbe inputProbe;
    TerminalSession inputSession(inputProbe.callbacks());
    SessionStartOptions blockedInputOptions = optionsFor(
        L"/Q /D /C \"ping -n 30 127.0.0.1 >nul\"");
#ifdef NPPTERMINAL_TESTS
    // Replace only the broker's input write endpoint with a small test pipe
    // whose read endpoint remains open and deliberately undrained. The shell
    // and real ConPTY stay alive, so this observes the actual input worker's
    // synchronous WriteFile cancellation path without depending on how a
    // particular ConPTY build drains its console input buffer.
    blockedInputOptions.testBlockInputWrite = true;
#endif
    require(inputSession.start(blockedInputOptions, error),
        "blocked-input start was rejected");
    waitForRunning(inputSession, inputProbe);
    const std::vector<std::uint8_t> input(kMaxInputBytes, 0x41);
    const std::uint64_t inputGeneration = inputSession.generation();
    std::uint64_t acceptedInputWrites = 0;
    std::uint64_t nextInputId = 1;
    std::uint64_t observedActiveId = 0;
    auto activeSince = Clock::now();
    bool writeWasHeld = false;
    const auto inputBlockDeadline = Clock::now() + std::chrono::seconds(5);
    // TerminalSession is caller-thread-affine. Fill and poll on this thread;
    // observing the helper's write does not require a racing DLL producer.
    while (Clock::now() < inputBlockDeadline) {
        if (inputSession.write(inputGeneration, nextInputId, input)) {
            ++acceptedInputWrites;
            ++nextInputId;
        }
        inputSession.poll();
        const std::uint64_t activeId = inputProbe.inputWriteActiveId.load();
        if (activeId != observedActiveId) {
            observedActiveId = activeId;
            activeSince = Clock::now();
        }
        if (activeId != 0 && Clock::now() - activeSince >= std::chrono::milliseconds(100) &&
            inputProbe.inputWriteStartedCount.load() > inputProbe.inputWriteFinishedCount.load()) {
            writeWasHeld = true;
            break;
        }
        ::Sleep(1);
    }
    require(writeWasHeld && acceptedInputWrites != 0,
        "blocked-input producer did not hold a synchronous WriteFile attempt");
    const auto inputStopStarted = Clock::now();
    inputSession.requestStop(StopReason::UserKill);
    require(inputSession.waitForStop(5000), "blocked-input stop did not complete");
    const auto inputStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - inputStopStarted).count();
    require(inputStopMs < 2000, "blocked-input stop exceeded the ordinary two-second target");
    require(!inputSession.hasProcess() && inputSession.processId() == 0,
        "blocked-input stop retained process resources");

    SessionProbe outputProbe;
    outputProbe.blockOutput.store(true);
    TerminalSession outputSession(outputProbe.callbacks());
    require(outputSession.start(optionsFor(
        L"/Q /D /C \"echo NPP_BLOCKED_OUTPUT&ping -n 30 127.0.0.1 >nul\""), error),
        "blocked-output start was rejected");
    waitForRunning(outputSession, outputProbe);
    require(waitSessionUntil(outputSession, [&] { return outputProbe.outputEntered.load(); }, 5000),
        "blocked-output callback was not entered");
    const auto outputStopStarted = Clock::now();
    outputSession.requestStop(StopReason::UserKill);
    require(outputSession.waitForStop(5000), "blocked-output stop did not complete");
    const auto outputStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - outputStopStarted).count();
    require(outputStopMs < 2000, "blocked-output stop exceeded the ordinary two-second target");
    require(!outputSession.hasProcess() && outputSession.processId() == 0,
        "blocked-output stop retained process resources");
}

void testDeterministicStream(std::uint64_t count)
{
    require(count != 0 && count <= 1000000, "stream count is outside the supported test range");
    SessionProbe probe;
    probe.streamVerification = true;
    TerminalSession session(probe.callbacks());
    const std::wstring command = L"/Q /D /C \"for /L %A in (1,1," +
        std::to_wstring(count) + L") do @echo NPP_STREAM_%A\"";
    std::wstring error;
    require(session.start(optionsFor(command), error), "deterministic-stream start was rejected");
    require(waitSessionUntil(session, [&] {
        return session.state() == SessionState::Exited || probe.hasState(SessionState::Error);
    }, 30000), "deterministic stream did not finish");
    require(session.state() == SessionState::Exited, "deterministic stream entered Error");
    require(session.waitForStop(5000), "deterministic-stream supervisor did not join");
    std::lock_guard<std::mutex> lock(probe.mutex);
    std::string finalTail = probe.streamPending;
    // ConPTY can append terminal-mode shutdown controls after the final LF.
    // Apply the same presentation-control filter used for complete records;
    // any remaining application bytes still fail the exact-count test.
    SessionProbe::stripTerminalControls(finalTail);
    require(!probe.streamInvalid && finalTail.empty() && probe.streamLines == count,
        "deterministic stream had lost, reordered, or malformed lines (lines=" +
        std::to_string(probe.streamLines) + ", expected=" + std::to_string(count) +
        ", bytes=" + std::to_string(probe.streamBytes) + ", first='" +
        probe.streamFirstInvalid + "' expected='" + probe.streamFirstExpected +
        "', final_raw_bytes=" + std::to_string(probe.streamPending.size()) +
        ", final_application_bytes=" + std::to_string(finalTail.size()) + ")");
    require(probe.streamBytes != 0, "deterministic stream produced no output");
    require(!session.hasProcess() && session.processId() == 0,
        "deterministic stream retained process resources");
}

void testDeterministicStreamFor(std::uint64_t seconds)
{
    require(seconds != 0 && seconds <= 24u * 60u * 60u,
        "stream duration is outside the supported test range");
    SessionProbe probe;
    probe.streamVerification = true;
    TerminalSession session(probe.callbacks());
    const std::wstring command =
        L"/Q /D /C \"for /L %A in (1,1,2147483647) do @echo NPP_STREAM_%A\"";
    std::wstring error;
    require(session.start(optionsFor(command), error), "stream-duration start was rejected");
    waitForRunning(session, probe);
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    bool reachedDeadline = true;
    while (Clock::now() < deadline) {
        session.poll();
        if (session.state() != SessionState::Running) {
            reachedDeadline = false;
            break;
        }
        ::Sleep(50);
    }
    require(reachedDeadline, "stream-duration session exited before the requested duration");
    session.requestStop(StopReason::UserKill);
    require(session.waitForStop(10000), "stream-duration stop did not complete");
    std::lock_guard<std::mutex> lock(probe.mutex);
    require(!probe.streamInvalid && probe.streamLines >= 100,
        "stream-duration output failed sequence integrity or was too short");
    std::wcout << L"stream_seconds=" << seconds << L" lines=" << probe.streamLines
        << L" bytes=" << probe.streamBytes << L" final_partial_bytes="
        << probe.streamPending.size() << L" max_partial_bytes=" << probe.streamMaxPending
        << L"\n";
    require(!session.hasProcess() && session.processId() == 0,
        "stream-duration retained process resources");
}

enum class JobProbeResult {
    Passed,
    Skipped,
};

std::wstring testExecutablePath()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    require(length != 0 && length < std::size(path), "GetModuleFileNameW failed");
    return std::wstring(path, length);
}

std::atomic_bool ctrlCReceived{false};

BOOL WINAPI ctrlCHandler(DWORD event)
{
    if (event != CTRL_C_EVENT) return FALSE;
    ctrlCReceived.store(true);
    return TRUE;
}

int runCtrlCChild()
{
    DWORD mode = 0;
    if (!::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &mode) ||
        !::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), mode | ENABLE_PROCESSED_INPUT) ||
        !::SetConsoleCtrlHandler(ctrlCHandler, TRUE)) return 2;
    // Do not explicitly enable Ctrl+C with SetConsoleCtrlHandler(NULL, FALSE):
    // that would conceal a launch flag which disabled signals for the child.
    std::cout << "NPP_CTRL_C_READY" << std::endl;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!ctrlCReceived.load() && Clock::now() < deadline) ::Sleep(5);
    if (!ctrlCReceived.load()) return 3;
    std::cout << "NPP_CTRL_C_RECEIVED" << std::endl;
    return 0;
}

void testCtrlCSignal()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    SessionStartOptions options = optionsFor(L"/Q /D");
    options.applicationName = testExecutablePath();
    options.commandLine = L"\"" + options.applicationName + L"\" --ctrl-c-child";
    std::wstring error;
    require(session.start(options, error), "Ctrl+C fixture start was rejected");
    waitForRunning(session, probe);
    require(waitSessionUntil(session, [&] {
        return probe.outputCopy().find("NPP_CTRL_C_READY") != std::string::npos;
    }, 5000), "Ctrl+C fixture did not become ready");
    require(session.write(std::vector<std::uint8_t>{3}), "Ctrl+C input was rejected");
    require(waitSessionUntil(session, [&] {
        return probe.outputCopy().find("NPP_CTRL_C_RECEIVED") != std::string::npos;
    }, 5000), "ConPTY Ctrl+C did not deliver CTRL_C_EVENT");
    waitForExited(session, probe);
    require(!session.hasProcess() && !session.hasPendingWork(),
        "Ctrl+C fixture retained owned work");
    std::wcout << L"ctrl_c_signal=received\n";
}

bool processGone(DWORD processId, DWORD timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, processId);
        if (!process) {
            if (::GetLastError() == ERROR_INVALID_PARAMETER) return true;
        } else {
            const DWORD wait = ::WaitForSingleObject(process, 0);
            ::CloseHandle(process);
            if (wait == WAIT_OBJECT_0) return true;
        }
        if (Clock::now() >= deadline) return false;
        ::Sleep(10);
    }
}

void testBrokerStall()
{
    warmupNormalSessionResources();
    const ResourceSnapshot baseline = currentProcessResources();
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    SessionStartOptions options = optionsFor(
        L"/Q /D /C \"start \"\" /b ping -n 30 127.0.0.1 >nul&"
        L"ping -n 30 127.0.0.1 >nul\"");
    options.testStallOnStop = true;
    std::wstring error;
    require(session.start(options, error), "broker-stall start was rejected");
    waitForRunning(session, probe);
    const DWORD shellProcessId = session.processId();
    const DWORD brokerProcessId = session.brokerProcessId();
    require(shellProcessId != 0 && brokerProcessId != 0,
        "broker-stall did not publish shell and broker process ids");
    DWORD descendantProcessId = 0;
    require(waitSessionUntil(session, [&] {
        descendantProcessId = firstChildProcessId(shellProcessId);
        return descendantProcessId != 0;
    }, 5000), "broker-stall did not observe a shell descendant");

    const auto stopStarted = Clock::now();
    session.requestStop(StopReason::HostShutdown);
    require(session.waitForStop(5000), "broker-stall stop did not complete");
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - stopStarted).count();
    require(stopMs < 2000, "broker-stall stop exceeded the bounded shutdown target");
    require(!session.hasProcess() && session.processId() == 0,
        "broker-stall retained shell resources");
    require(processGone(brokerProcessId, 5000),
        "broker-stall left the helper broker process alive");
    require(processGone(shellProcessId, 5000),
        "broker-stall left the owned shell process alive");
    require(processGone(descendantProcessId, 5000),
        "broker-stall left the owned shell descendant alive");
    session.poll();
    ::Sleep(100);
    const ResourceSnapshot settled = currentProcessResources();
    std::wcerr << L"broker_stall_resources stop_ms=" << stopMs
        << L" baseline_handles=" << baseline.handles << L" settled_handles="
        << settled.handles << L" baseline_threads=" << baseline.threads
        << L" settled_threads=" << settled.threads << L" has_process="
        << (session.hasProcess() ? 1 : 0) << L" pending="
        << (session.hasPendingWork() ? 1 : 0) << L"\n";
    require(!session.hasPendingWork(), "broker-stall retained pending DLL work");
    require(settled.threads <= baseline.threads,
        "broker-stall retained a DLL thread");
    require(settled.handles <= baseline.handles,
        "broker-stall retained DLL handles");
    std::wcout << L"broker_stall_ms=" << stopMs << L" baseline_handles=" << baseline.handles
        << L" settled_handles=" << settled.handles << L" baseline_threads=" << baseline.threads
        << L" settled_threads=" << settled.threads << L"\n";
}

void testThreadStartFailures()
{
    warmupNormalSessionResources();
    const ResourceSnapshot baseline = currentProcessResources();
    for (int index = 1; index <= 3; ++index) {
        SessionProbe probe;
        TerminalSession session(probe.callbacks());
        SessionStartOptions options = optionsFor(L"/Q /D");
        options.testFailThreadStartIndex = index;
        std::wstring error;
        require(session.start(options, error), "thread-failure broker launch was rejected");
        const DWORD brokerId = session.brokerProcessId();
        require(waitSessionUntil(session, [&] {
            return session.state() == SessionState::Error && !session.hasPendingWork();
        }, 10000), "partial helper thread-start failure did not quiesce with Error");
        require(processGone(brokerId, 5000), "partial thread-start failure retained its helper");
        require(!session.hasProcess(), "partial thread-start failure retained DLL process handles");
    }
    ::Sleep(100);
    const ResourceSnapshot settled = currentProcessResources();
    std::wcerr << L"thread_start_failure_resources baseline_handles=" << baseline.handles
        << L" settled_handles=" << settled.handles << L" baseline_threads="
        << baseline.threads << L" settled_threads=" << settled.threads << L"\n";
    require(settled.handles <= baseline.handles && settled.threads <= baseline.threads,
        "partial helper thread-start failure retained DLL handles or threads");
    std::wcout << L"thread_start_failures=3 baseline_handles=" << baseline.handles
        << L" settled_handles=" << settled.handles << L" baseline_threads=" << baseline.threads
        << L" settled_threads=" << settled.threads << L"\n";
}

void testBlockedCommandStop()
{
    warmupNormalSessionResources();
    const ResourceSnapshot baseline = currentProcessResources();
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    SessionStartOptions options = optionsFor(L"/Q /D");
    options.testPauseCommandReader = true;
    std::wstring error;
    require(session.start(options, error), "blocked-command start was rejected");
    waitForRunning(session, probe);
    const DWORD brokerId = session.brokerProcessId();
    const DWORD shellId = session.processId();
    require(session.write(session.generation(), 1,
        std::vector<std::uint8_t>(kMaxInputBytes, 'A')),
        "blocked-command input was rejected");
    require(waitSessionUntil(session, [&] { return session.hasPendingCommandWrite(); }, 5000),
        "blocked-command fixture did not observe pending overlapped I/O");
    const auto started = Clock::now();
    session.requestStop(StopReason::HostShutdown);
    require(session.waitForStop(2000), "blocked-command shutdown failed");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started).count();
    require(elapsed < 2000, "blocked-command shutdown exceeded two seconds");
    require(processGone(brokerId, 5000) && processGone(shellId, 5000),
        "blocked-command shutdown retained an owned process");
    session.poll();
    ::Sleep(100);
    const ResourceSnapshot settled = currentProcessResources();
    std::wcerr << L"blocked_command_resources stop_ms=" << elapsed
        << L" baseline_handles=" << baseline.handles << L" settled_handles="
        << settled.handles << L" baseline_threads=" << baseline.threads
        << L" settled_threads=" << settled.threads << L" has_process="
        << (session.hasProcess() ? 1 : 0) << L" pending="
        << (session.hasPendingWork() ? 1 : 0) << L"\n";
    require(!session.hasPendingWork(), "blocked-command retained pending DLL work");
    require(settled.handles <= baseline.handles && settled.threads <= baseline.threads,
        "blocked-command cancellation retained DLL handles or threads");
    std::wcout << L"blocked_command_ms=" << elapsed << L" baseline_handles=" << baseline.handles
        << L" settled_handles=" << settled.handles << L" baseline_threads=" << baseline.threads
        << L" settled_threads=" << settled.threads << L"\n";
}

void testOrphanCommandCancellation()
{
    warmupNormalSessionResources();
    const ResourceSnapshot baseline = currentProcessResources();
    std::atomic_bool holdObservation{true};
    SessionProbe probe;
    SessionCallbacks callbacks = probe.callbacks();
    callbacks.testHoldCommandCancellationObservation = [&] {
        // This gate only postpones the host's observation. CancelIoEx still
        // owns and completes the real overlapped operation before reclamation.
        return holdObservation.load();
    };
    TerminalSession session(std::move(callbacks));
    SessionStartOptions options = optionsFor(L"/Q /D");
    options.testPauseCommandReader = true;

    for (int cycle = 0; cycle != 3; ++cycle) {
        holdObservation.store(true);
        std::wstring error;
        require(session.start(options, error), "orphan-command start was rejected");
        require(waitSessionUntil(session, [&] {
            return session.state() == SessionState::Running;
        }, 5000), "orphan-command session did not reach Running");

        const DWORD brokerId = session.brokerProcessId();
        const DWORD shellId = session.processId();
        require(brokerId != 0 && shellId != 0,
            "orphan-command did not publish owned process ids");
        require(session.write(session.generation(), static_cast<std::uint64_t>(cycle + 1),
            std::vector<std::uint8_t>(kMaxInputBytes, 'A')),
            "orphan-command input was rejected");
        require(waitSessionUntil(session, [&] { return session.hasPendingCommandWrite(); }, 5000),
            "orphan-command fixture did not observe pending overlapped I/O");

        const auto stopStarted = Clock::now();
        session.requestStop(StopReason::HostShutdown);
        require(session.waitForStop(2000), "orphan-command shutdown failed");
        const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - stopStarted).count();
        require(stopMs < 2000, "orphan-command shutdown exceeded two seconds");
        require(processGone(brokerId, 5000) && processGone(shellId, 5000),
            "orphan-command shutdown retained an owned process");
        require(session.state() == SessionState::Error,
            "orphan-command cancellation failure was not surfaced as Error");
        require(session.hasPendingWork(),
            "orphan-command did not retain the unresolved completion record");

        std::wstring restartError;
        require(!session.start(options, restartError),
            "orphan-command restarted while its completion was held");
        require(restartError.find(L"pending") != std::wstring::npos,
            "orphan-command restart refusal was not actionable");
        require(session.hasPendingWork(),
            "orphan-command restart refusal lost the retained completion record");
        require(!session.hasProcess(), "orphan-command refusal created a helper process");

        holdObservation.store(false);
        require(waitSessionUntil(session, [&] { return !session.hasPendingWork(); }, 5000),
            "orphan-command completion was not reclaimed after clearing the hook");
        require(session.state() == SessionState::Error,
            "orphan-command reclamation changed the surfaced failure state");
        ::Sleep(100);
        const ResourceSnapshot settled = currentProcessResources();
        std::wcerr << L"orphan_command_resources cycle=" << cycle << L" baseline_handles="
            << baseline.handles << L" settled_handles=" << settled.handles
            << L" baseline_threads=" << baseline.threads << L" settled_threads="
            << settled.threads << L"\n";
        require(settled.handles <= baseline.handles && settled.threads <= baseline.threads,
            "orphan-command cycle retained DLL handles or threads");
    }

    std::wcout << L"orphan_command_cycles=3 baseline_handles=" << baseline.handles
        << L" baseline_threads=" << baseline.threads << L"\n";
}

int runPanelStreamChild()
{
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return 2;

    constexpr std::size_t kTargetBytes = 2u * 1024u * 1024u;
    const std::string line = "NPP_PANEL_STREAM_PAYLOAD_0123456789ABCDEF\r\n";
    const std::string marker = "NPP_PANEL_STREAM_DONE\r\n";
    std::string bytes;
    bytes.reserve(kTargetBytes + marker.size());
    while (bytes.size() < kTargetBytes) bytes += line;
    bytes += marker;

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, 64u * 1024u));
        if (!::WriteFile(output, bytes.data() + offset, request, &written, nullptr) ||
            written == 0) return 2;
        offset += written;
    }
    return 0;
}

int runPanelStreamingInputChild()
{
    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) {
        return 2;
    }

    std::mutex outputMutex;
    const auto writeAll = [&](const std::string& value) {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::size_t offset = 0;
        while (offset < value.size()) {
            DWORD written = 0;
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
                value.size() - offset, 64u * 1024u));
            if (!::WriteFile(output, value.data() + offset, request, &written, nullptr) ||
                written == 0) return false;
            offset += written;
        }
        return true;
    };

    constexpr std::size_t kStreamChunkBytes = 4096;
    const std::string line = "NPP_PANEL_STREAMING_OUTPUT_0123456789ABCDEF\r\n";
    std::string streamChunk;
    streamChunk.reserve(kStreamChunkBytes + line.size());
    while (streamChunk.size() < kStreamChunkBytes) streamChunk += line;
    if (!writeAll("NPP_PANEL_STREAMING_READY\r\n")) return 2;

    std::atomic_bool running{true};
    std::thread writer([&] {
        while (running.load()) {
            if (!writeAll(streamChunk)) return;
            ::Sleep(1);
        }
    });

    std::string pending;
    char buffer[1024] = {};
    const std::string probePrefix = "NPP_PANEL_STREAM_PROBE_";
    while (running.load()) {
        DWORD read = 0;
        if (!::ReadFile(input, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) ||
            read == 0) break;
        pending.append(buffer, buffer + read);
        for (;;) {
            const std::size_t newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string command = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) {
                command.pop_back();
            }
            if (command == "NPP_PANEL_STREAM_STOP") {
                writeAll("NPP_PANEL_STREAMING_STOPPED\r\n");
                running.store(false);
                break;
            }
            if (command.rfind(probePrefix, 0) == 0 && command.size() > probePrefix.size()) {
                const std::string response = "NPP_PANEL_STREAM_RESPONSE_" +
                    command.substr(probePrefix.size()) + "\r\n";
                if (!writeAll(response)) running.store(false);
            }
        }
    }
    running.store(false);
    if (writer.joinable()) writer.join();
    return 0;
}

int runJobChild()
{
    SessionProbe probe;
    TerminalSession session(probe.callbacks());
    std::wstring error;
    if (!session.start(optionsFor(
        L"/Q /D /C \"start \"\" /b ping -n 30 127.0.0.1 >nul&ping -n 30 127.0.0.1 >nul\""), error)) {
        std::wcerr << L"START_FAILED " << error << std::endl;
        return 1;
    }
    if (!waitSessionUntil(session, [&] {
        return session.state() == SessionState::Running || probe.hasState(SessionState::Error);
    }, 10000)) {
        std::wcerr << L"START_TIMEOUT" << std::endl;
        session.stop(StopReason::HostShutdown);
        return 1;
    }
    if (session.state() != SessionState::Running) {
        const std::wstring message = probe.latestMessageCopy();
        std::wcerr << L"START_ERROR " << message << std::endl;
        session.stop(StopReason::HostShutdown);
        return message.find(L"AssignProcessToJobObject failed") != std::wstring::npos ? 3 : 1;
    }
    DWORD descendantProcessId = 0;
    require(waitSessionUntil(session, [&] {
        descendantProcessId = firstChildProcessId(session.processId());
        return descendantProcessId != 0;
    }, 5000), "job-probe child did not create a native descendant");
    std::wcout << L"READY " << session.processId() << L" " << descendantProcessId << std::endl;
    for (;;) ::Sleep(1000);
}

JobProbeResult testParentJobAndAbruptHost()
{
    SECURITY_ATTRIBUTES pipeAttributes{};
    pipeAttributes.nLength = sizeof(pipeAttributes);
    pipeAttributes.bInheritHandle = TRUE;
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    require(::CreatePipe(&outputRead, &outputWrite, &pipeAttributes, 0) != FALSE,
        "job-probe output pipe creation failed");
    require(::SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0) != FALSE,
        "job-probe output pipe inheritance setup failed");

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    require(job != nullptr, "job-probe parent job creation failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits)) != FALSE, "job-probe parent job setup failed");

    PROCESS_INFORMATION processInfo{};
    std::vector<wchar_t> commandLine;
    const std::wstring command = L"\"" + testExecutablePath() + L"\" --job-child";
    commandLine.assign(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = outputWrite;
    startupInfo.hStdError = outputWrite;
    const BOOL created = ::CreateProcessW(testExecutablePath().c_str(), commandLine.data(),
        nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startupInfo, &processInfo);
    if (!created) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(job);
        ::CloseHandle(outputRead);
        ::CloseHandle(outputWrite);
        if (error == ERROR_ACCESS_DENIED) {
            std::wcout << L"SKIP parent job assignment is unavailable" << std::endl;
            return JobProbeResult::Skipped;
        }
        throw TestFailure{"job-probe CreateProcessW failed"};
    }

    auto cleanup = [&] {
        if (job) {
            ::CloseHandle(job);
            job = nullptr;
        }
        if (processInfo.hProcess) {
            if (::WaitForSingleObject(processInfo.hProcess, 0) != WAIT_OBJECT_0) {
                ::TerminateProcess(processInfo.hProcess, ERROR_PROCESS_ABORTED);
                ::WaitForSingleObject(processInfo.hProcess, 5000);
            }
            ::CloseHandle(processInfo.hProcess);
            processInfo.hProcess = nullptr;
        }
        if (processInfo.hThread) {
            ::CloseHandle(processInfo.hThread);
            processInfo.hThread = nullptr;
        }
        if (outputRead) {
            ::CloseHandle(outputRead);
            outputRead = nullptr;
        }
        if (outputWrite) {
            ::CloseHandle(outputWrite);
            outputWrite = nullptr;
        }
    };

    try {
        if (!::AssignProcessToJobObject(job, processInfo.hProcess)) {
            const DWORD error = ::GetLastError();
            cleanup();
            if (error == ERROR_ACCESS_DENIED || error == ERROR_NOT_SUPPORTED) {
                std::wcout << L"SKIP parent job nesting is unavailable" << std::endl;
                return JobProbeResult::Skipped;
            }
            throw TestFailure{"job-probe AssignProcessToJobObject failed"};
        }
        require(::ResumeThread(processInfo.hThread) != static_cast<DWORD>(-1),
            "job-probe child resume failed");
        ::CloseHandle(outputWrite);
        outputWrite = nullptr;

        std::string output;
        DWORD shellProcessId = 0;
        DWORD descendantProcessId = 0;
        const auto readyDeadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            DWORD available = 0;
            if (!::PeekNamedPipe(outputRead, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available != 0) {
                char buffer[512] = {};
                DWORD read = 0;
                require(::ReadFile(outputRead, buffer, sizeof(buffer), &read, nullptr) != FALSE,
                    "job-probe output read failed");
                output.append(buffer, buffer + read);
                const std::size_t marker = output.find("READY ");
                if (marker != std::string::npos) {
                    const std::size_t begin = marker + 6;
                    const std::size_t end = output.find_first_of("\r\n", begin);
                    const std::string value = output.substr(begin, end - begin);
                    std::istringstream values(value);
                    values >> shellProcessId >> descendantProcessId;
                    break;
                }
            }
            if (::WaitForSingleObject(processInfo.hProcess, 0) == WAIT_OBJECT_0) break;
            if (Clock::now() >= readyDeadline) break;
            ::Sleep(10);
        }

        if (shellProcessId == 0 || descendantProcessId == 0) {
            DWORD childExitCode = STILL_ACTIVE;
            ::GetExitCodeProcess(processInfo.hProcess, &childExitCode);
            cleanup();
            if (childExitCode == 3) {
                std::wcout << L"SKIP child job nesting is unavailable" << std::endl;
                return JobProbeResult::Skipped;
            }
            throw TestFailure{"job-probe child did not publish a running shell"};
        }
        require(!processGone(shellProcessId, 0), "job-probe shell exited before host termination");
        require(!processGone(descendantProcessId, 0),
            "job-probe descendant exited before host termination");

        // Terminate the helper directly to model abrupt host termination. Keep
        // the outer job open so it cannot mask a broken inner kill-on-close
        // contract by cascading termination into nested job members.
        require(::TerminateProcess(processInfo.hProcess, ERROR_PROCESS_ABORTED) != FALSE,
            "abrupt-host helper termination failed");
        require(::WaitForSingleObject(processInfo.hProcess, 5000) == WAIT_OBJECT_0,
            "abrupt-host helper did not terminate after abrupt termination");
        require(processGone(shellProcessId, 5000),
            "abrupt-host termination left the owned shell process alive");
        require(processGone(descendantProcessId, 5000),
            "abrupt-host termination left the owned descendant alive");
        ::CloseHandle(job);
        job = nullptr;
        cleanup();
        return JobProbeResult::Passed;
    } catch (...) {
        cleanup();
        throw;
    }
}

void runUnit()
{
    testProtocol();
    testBoundedBridge();
    testGenerationRejection();
}

void runConPty(std::uint64_t cycles)
{
    testNaturalExitAndFinalTail();
    testInteractiveInputResizeAndKill();
    testCtrlCSignal();
    testNaturalExitDescendantCleanup();
    testStartupCancellation();
    testRepeatedCycles(cycles);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    for (int index = 1; index < argc; ++index) {
        if (std::wstring(argv[index]) == L"--job-child") return runJobChild();
        if (std::wstring(argv[index]) == L"--ctrl-c-child") return runCtrlCChild();
        if (std::wstring(argv[index]) == L"--panel-stream-child") return runPanelStreamChild();
        if (std::wstring(argv[index]) == L"--panel-stream-input-child") {
            return runPanelStreamingInputChild();
        }
    }
    bool unit = false;
    bool conpty = false;
    bool blockedStop = false;
    bool stress = false;
    bool jobProbe = false;
    bool brokerStall = false;
    bool blockedCommand = false;
    bool orphanCommand = false;
    bool profileCleanup = false;
    bool webViewCleanup = false;
    bool webViewIntegration = false;
    bool webViewRender = false;
    bool webViewSecurity = false;
    bool shellCatalog = false;
    bool settings = false;
    bool shellSmoke = false;
    bool terminalPanel = false;
    bool threadFailures = false;
    std::uint64_t cycles = 4;
    std::uint64_t streamCount = 0;
    std::uint64_t streamSeconds = 0;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        unit = unit || argument == L"--unit";
        conpty = conpty || argument == L"--conpty";
        blockedStop = blockedStop || argument == L"--blocked-stop";
        stress = stress || argument == L"--stress";
        jobProbe = jobProbe || argument == L"--job-probe";
        brokerStall = brokerStall || argument == L"--broker-stall";
        blockedCommand = blockedCommand || argument == L"--blocked-command-stop";
        orphanCommand = orphanCommand || argument == L"--orphan-command";
        profileCleanup = profileCleanup || argument == L"--profile-cleanup";
        webViewCleanup = webViewCleanup || argument == L"--webview-cleanup";
        webViewIntegration = webViewIntegration || argument == L"--webview-integration";
        webViewRender = webViewRender || argument == L"--webview-render";
        webViewSecurity = webViewSecurity || argument == L"--webview-security";
        shellCatalog = shellCatalog || argument == L"--shell-catalog";
        settings = settings || argument == L"--settings";
        shellSmoke = shellSmoke || argument == L"--shell-smoke";
        terminalPanel = terminalPanel || argument == L"--terminal-panel";
        threadFailures = threadFailures || argument == L"--thread-start-failures";
        const std::wstring cyclesPrefix = L"--cycles=";
        const std::wstring streamCountPrefix = L"--stream-count=";
        const std::wstring streamSecondsPrefix = L"--stream-seconds=";
        try {
            if (argument.rfind(cyclesPrefix, 0) == 0) {
                cycles = std::stoull(argument.substr(cyclesPrefix.size()));
            } else if (argument.rfind(streamCountPrefix, 0) == 0) {
                streamCount = std::stoull(argument.substr(streamCountPrefix.size()));
            } else if (argument.rfind(streamSecondsPrefix, 0) == 0) {
                streamSeconds = std::stoull(argument.substr(streamSecondsPrefix.size()));
            }
        } catch (...) {
            std::wcerr << L"invalid numeric option: " << argument << L"\n";
            return 2;
        }
    }
    if (stress) {
        conpty = true;
        if (cycles == 4) cycles = 100;
    }
    if (cycles == 0 || cycles > 1000000) {
        std::wcerr << L"cycles is outside the supported test range\n";
        return 2;
    }
    if (!unit && !conpty && !blockedStop && !jobProbe && !brokerStall && !blockedCommand && !orphanCommand && !profileCleanup && !webViewCleanup && !webViewIntegration && !webViewRender && !webViewSecurity && !shellCatalog && !settings && !shellSmoke && !terminalPanel && !threadFailures &&
        streamCount == 0 && streamSeconds == 0) {
        std::wcerr << L"usage: NppTerminal.Tests.exe --unit [--conpty] [--stress] "
            L"[--cycles=N] [--blocked-stop] [--job-probe] [--broker-stall] "
            L"[--blocked-command-stop] [--orphan-command] [--profile-cleanup] "
            L"[--webview-cleanup] "
            L"[--webview-integration] "
            L"[--webview-render] "
            L"[--webview-security] "
            L"[--shell-catalog] [--settings] [--shell-smoke] [--terminal-panel] "
            L"[--thread-start-failures] "
            L"[--stream-count=N] [--stream-seconds=N]\n";
        return 2;
    }
    try {
        if (unit) runUnit();
        if (conpty) runConPty(cycles);
        if (blockedStop) testBlockedStop();
        if (jobProbe && testParentJobAndAbruptHost() == JobProbeResult::Skipped) return 3;
        if (brokerStall) testBrokerStall();
        if (blockedCommand) testBlockedCommandStop();
        if (orphanCommand) testOrphanCommandCancellation();
        if (profileCleanup) nppterminal::tests::runProfileCleanupTests();
        if (webViewCleanup) WebViewHost::runCleanupObserverTests();
        if (webViewIntegration) WebViewHost::runCleanupIntegrationTests();
        if (webViewRender) nppterminal::tests::runWebViewRenderTests();
        if (webViewSecurity) nppterminal::tests::runWebViewSecurityTests();
        if (shellCatalog) nppterminal::tests::runShellCatalogTests();
        if (settings) nppterminal::tests::runSettingsTests();
        if (shellSmoke) nppterminal::tests::runShellSmokeTests();
        if (terminalPanel) nppterminal::tests::runTerminalPanelTests();
        if (threadFailures) testThreadStartFailures();
        if (streamCount != 0) testDeterministicStream(streamCount);
        if (streamSeconds != 0) testDeterministicStreamFor(streamSeconds);
        std::wcout << L"PASS\n";
        return 0;
    } catch (const TestFailure& failure) {
        std::cerr << "FAIL: " << failure.message << '\n';
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "FAIL: unexpected exception\n";
        return 1;
    }
}
