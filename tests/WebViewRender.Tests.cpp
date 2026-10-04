#include "WebViewRender.Tests.h"

#include "Bridge.h"
#include "Protocol.h"
#include "WebViewHost.h"

#include <windows.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nppterminal::tests {

namespace {

using json = nlohmann::json;
using namespace nppterminal;

constexpr std::uint64_t kGeneration = 37;
constexpr std::size_t kRecordCount = 6000;
constexpr std::size_t kChunkBytes = 30u * 1024u;
constexpr DWORD kStartupTimeoutMs = 15000;
constexpr DWORD kScriptTimeoutMs = 5000;
constexpr DWORD kStaleMessageTimeoutMs = 250;
constexpr DWORD kDrainTimeoutMs = 30000;
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;
constexpr wchar_t kWindowClass[] = L"NppTerminalWebViewRenderWindow-v1";

// This source is fixed test code.  It is installed after the shipping page has
// navigated and before any fixture output is posted.  It wraps the real
// xterm.js Terminal.prototype.write, keeps the page callback intact, and reads
// only xterm's public buffer API after parsing has completed.
constexpr wchar_t kRenderProbeScript[] = LR"JS(
(() => {
  const bridge = window.chrome && window.chrome.webview;
  const terminalType = window.Terminal;
  if (!bridge || !terminalType || !terminalType.prototype ||
      typeof terminalType.prototype.write !== 'function') {
    if (bridge) bridge.postMessage({ type: 'renderHook', installed: false });
    return;
  }
  if (window.__nppterminalRenderProbe) {
    bridge.postMessage({ type: 'renderHook', installed: true, existing: true });
    return;
  }
  const originalWrite = terminalType.prototype.write;
  let checksum = 2166136261;
  let bytes = 0;
  let writes = 0;
  const toBytes = data => {
    if (typeof data === 'string') return new TextEncoder().encode(data);
    if (data instanceof Uint8Array) return data;
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    return new Uint8Array(0);
  };
  const publicTail = terminal => {
    const buffer = terminal && terminal.buffer && terminal.buffer.active;
    if (!buffer || typeof buffer.getLine !== 'function') return '';
    const lines = [];
    const start = Math.max(0, buffer.length - 4);
    for (let row = start; row < buffer.length; ++row) {
      const line = buffer.getLine(row);
      lines.push(line && typeof line.translateToString === 'function'
        ? line.translateToString(true) : '');
    }
    return lines.join('\n');
  };
  terminalType.prototype.write = function(data, callback) {
    const input = toBytes(data);
    for (const value of input) {
      checksum ^= value;
      checksum = Math.imul(checksum, 16777619) >>> 0;
    }
    bytes += input.length;
    ++writes;
    const prefix = { bytes, checksum, writes };
    let reported = false;
    const complete = () => {
      if (!reported) {
        reported = true;
        bridge.postMessage({ type: 'renderAck', ...prefix,
          tail: publicTail(this) });
      }
      if (typeof callback === 'function') callback();
    };
    return originalWrite.call(this, data, complete);
  };
  window.__nppterminalRenderProbe = { installed: true };
  bridge.postMessage({ type: 'renderHook', installed: true });
})();
)JS";

struct ExpectedChunk final {
    std::uint64_t id = 0;
    std::vector<std::uint8_t> data;
    std::size_t cumulativeBytes = 0;
    std::uint32_t cumulativeChecksum = kFnvOffset;
};

struct RenderState final {
    bool navigationReady = false;
    bool pageReady = false;
    bool hookInstalled = false;
    std::wstring callbackError;
    std::string protocolError;
    std::size_t renderAckCount = 0;
    std::size_t pageAckCount = 0;
    std::vector<std::uint64_t> pageAckIds;
    std::string finalTail;
    bool finalTailSeen = false;
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error("WEBVIEW_RENDER FAIL: " + message);
}

[[noreturn]] void notRun(const std::string& message)
{
    throw std::runtime_error("WEBVIEW_RENDER NOT_RUN: " + message);
}

void require(bool condition, const std::string& message)
{
    if (!condition) fail(message);
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

std::wstring jsonWide(const json& value)
{
    return utf8ToWide(value.dump());
}

void updateChecksum(std::uint32_t& checksum, const std::uint8_t* bytes,
    std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t value = bytes[index];
        checksum ^= value;
        checksum *= kFnvPrime;
    }
}

std::vector<ExpectedChunk> makeExpectedChunks()
{
    std::vector<ExpectedChunk> chunks;
    std::vector<std::uint8_t> pending;
    pending.reserve(kChunkBytes);
    std::size_t totalBytes = 0;
    std::uint32_t checksum = kFnvOffset;
    std::uint64_t id = 1;

    const auto flush = [&] {
        if (pending.empty()) return;
        chunks.push_back(ExpectedChunk{id, std::move(pending), totalBytes, checksum});
        ++id;
        pending = {};
        pending.reserve(kChunkBytes);
    };

    for (std::size_t index = 0; index < kRecordCount; ++index) {
        std::ostringstream record;
        record << "\x1b[36mNPP_RENDER_" << std::setw(4) << std::setfill('0') << index
            << " payload=0123456789abcdef";
        if (index + 1 == kRecordCount) record << " TAIL-RENDER-END";
        record << "\x1b[0m\r\n";
        const std::string text = record.str();
        const auto* first = reinterpret_cast<const std::uint8_t*>(text.data());
        const auto* last = first + text.size();
        while (first != last) {
            const std::size_t capacity = kChunkBytes - pending.size();
            const std::size_t count = std::min<std::size_t>(capacity,
                static_cast<std::size_t>(last - first));
            pending.insert(pending.end(), first, first + count);
            updateChecksum(checksum, first, count);
            totalBytes += count;
            first += count;
            if (pending.size() == kChunkBytes) flush();
        }
    }
    flush();
    return chunks;
}

bool getUnsigned(const json& value, const char* name, std::uint64_t& result)
{
    if (!value.contains(name) || !value[name].is_number_unsigned()) return false;
    result = value[name].get<std::uint64_t>();
    return true;
}

void receiveMessage(const std::shared_ptr<RenderState>& state,
    const std::vector<ExpectedChunk>& expected, const std::wstring& message)
{
    if (!state->protocolError.empty()) return;
    try {
        const json value = json::parse(wideToUtf8(message));
        if (!value.is_object() || !value.contains("type") ||
            !value["type"].is_string()) {
            state->protocolError = "renderer sent a non-object message";
            return;
        }
        const std::string type = value["type"].get<std::string>();
        if (type == "ready") {
            std::uint64_t generation = 0;
            std::uint64_t columns = 0;
            std::uint64_t rows = 0;
            if (!getUnsigned(value, "generation", generation) ||
                !getUnsigned(value, "cols", columns) ||
                !getUnsigned(value, "rows", rows) || generation != kGeneration ||
                columns == 0 || rows == 0) {
                state->protocolError = "renderer ready message had an invalid generation or grid";
                return;
            }
            state->pageReady = true;
            return;
        }
        if (type == "renderHook") {
            const bool installed = value.contains("installed") &&
                value["installed"].is_boolean() && value["installed"].get<bool>();
            if (!installed) state->protocolError = "xterm write probe was not installed";
            state->hookInstalled = installed;
            return;
        }
        if (type == "resize") {
            std::uint64_t generation = 0;
            std::uint64_t columns = 0;
            std::uint64_t rows = 0;
            if (!getUnsigned(value, "generation", generation) ||
                !getUnsigned(value, "cols", columns) ||
                !getUnsigned(value, "rows", rows) || generation != kGeneration ||
                columns == 0 || rows == 0) {
                state->protocolError = "renderer resize message was invalid";
            }
            return;
        }
        if (type == "renderAck") {
            const std::size_t index = state->renderAckCount;
            if (index >= expected.size()) {
                state->protocolError = "renderer reported more writes than posted";
                return;
            }
            std::uint64_t bytes = 0;
            std::uint64_t checksum = 0;
            std::uint64_t writes = 0;
            if (!getUnsigned(value, "bytes", bytes) ||
                !getUnsigned(value, "checksum", checksum) ||
                !getUnsigned(value, "writes", writes) ||
                !value.contains("tail") || !value["tail"].is_string()) {
                state->protocolError = "renderer write report was incomplete";
                return;
            }
            const ExpectedChunk& prefix = expected[index];
            if (bytes != prefix.cumulativeBytes || checksum != prefix.cumulativeChecksum ||
                writes != index + 1) {
                state->protocolError = "renderer checksum or write count was out of order";
                return;
            }
            state->finalTail = value["tail"].get<std::string>();
            if (state->finalTail.find("TAIL-RENDER-END") != std::string::npos) {
                state->finalTailSeen = true;
            }
            ++state->renderAckCount;
            return;
        }
        if (type == "ack") {
            std::uint64_t generation = 0;
            std::uint64_t id = 0;
            if (!getUnsigned(value, "generation", generation) ||
                !getUnsigned(value, "id", id) || generation != kGeneration ||
                state->pageAckCount >= expected.size() ||
                id != expected[state->pageAckCount].id) {
                state->protocolError = "renderer acknowledgement was stale or out of order";
                return;
            }
            state->pageAckIds.push_back(id);
            ++state->pageAckCount;
            return;
        }
        state->protocolError = "renderer sent an unexpected message type";
    } catch (const std::exception&) {
        state->protocolError = "renderer sent invalid JSON";
    }
}

void pumpMessages()
{
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            ::PostQuitMessage(static_cast<int>(message.wParam));
            fail("the WebView STA received WM_QUIT");
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, DWORD timeoutMs)
{
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        pumpMessages();
        if (predicate()) return true;
        if (::GetTickCount64() >= deadline) return predicate();
        ::Sleep(5);
    }
}

struct ApartmentGuard final {
    ~ApartmentGuard()
    {
        ::CoUninitialize();
    }
};

struct WindowGuard final {
    HWND window = nullptr;
    HMODULE module = nullptr;
    bool unregister = false;

    ~WindowGuard()
    {
        if (window) ::DestroyWindow(window);
        if (unregister) ::UnregisterClassW(kWindowClass, module);
    }
};

void postOutput(WebViewHost& host, const ExpectedChunk& chunk)
{
    const json message = {
        {"type", "output"},
        {"generation", kGeneration},
        {"id", chunk.id},
        {"data", base64Encode(chunk.data)}};
    require(host.postJson(jsonWide(message)), "WebView rejected an output message");
}

} // namespace

void runWebViewRenderTests()
{
    const auto expected = makeExpectedChunks();
    require(!expected.empty(), "renderer fixture produced no output chunks");

    const HMODULE testModule = ::GetModuleHandleW(nullptr);
    if (!testModule) notRun("the test executable module could not be resolved");
    wchar_t modulePath[MAX_PATH * 4] = {};
    const DWORD pathLength = ::GetModuleFileNameW(testModule, modulePath,
        static_cast<DWORD>(std::size(modulePath)));
    if (pathLength == 0 || pathLength >= std::size(modulePath)) {
        notRun("the test executable directory could not be resolved");
    }
    const std::wstring fullModulePath(modulePath, pathLength);
    const std::size_t separator = fullModulePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        notRun("the test executable path has no parent directory");
    }
    const std::wstring moduleDirectory = fullModulePath.substr(0, separator);
    const DWORD webAttributes = ::GetFileAttributesW(
        (moduleDirectory + L"\\web").c_str());
    if (moduleDirectory.empty() || webAttributes == INVALID_FILE_ATTRIBUTES ||
        !(webAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        notRun("the copied shipping web fixture is missing beside the test executable");
    }

    const HRESULT apartmentResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (apartmentResult == RPC_E_CHANGED_MODE) {
        notRun("the native test thread is already initialized in a non-STA apartment");
    }
    if (FAILED(apartmentResult)) notRun("CoInitializeEx(COINIT_APARTMENTTHREADED) failed");
    ApartmentGuard apartmentGuard;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &::DefWindowProcW;
    windowClass.hInstance = testModule;
    windowClass.lpszClassName = kWindowClass;
    const ATOM registered = ::RegisterClassW(&windowClass);
    const bool registeredHere = registered != 0;
    if (!registeredHere && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fail("failed to register the hidden WebView window class");
    }
    const HWND parent = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"NppTerminal WebView render", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, testModule, nullptr);
    if (!parent) {
        if (registeredHere) ::UnregisterClassW(kWindowClass, testModule);
        fail("failed to create the hidden WebView window");
    }
    WindowGuard windowGuard{parent, testModule, registeredHere};

    auto state = std::make_shared<RenderState>();
    WebViewHost host;
    const auto callback = [state, &expected](const std::wstring& message) {
        receiveMessage(state, expected, message);
    };
    if (!host.create(parent, testModule, callback,
        [state] { state->navigationReady = true; },
        [state](const std::wstring& message) { state->callbackError = message; })) {
        if (state->callbackError.find(L"runtime") != std::wstring::npos) {
            notRun("WebView2 runtime startup failed: " + narrow(state->callbackError));
        }
        fail("WebViewHost::create failed: " + narrow(state->callbackError));
    }
    host.resize(RECT{0, 0, 1200, 800});
    require(waitUntil([&] {
        return state->navigationReady || !state->callbackError.empty();
    }, kStartupTimeoutMs), "WebView navigation did not complete");
    if (!state->callbackError.empty()) {
        if (state->callbackError.find(L"runtime") != std::wstring::npos) {
            notRun("WebView2 runtime is unavailable: " + narrow(state->callbackError));
        }
        fail("WebView navigation failed: " + narrow(state->callbackError));
    }
    require(host.isReady(), "WebViewHost reported navigation ready without a view");

    const json init = {{"type", "init"}, {"generation", kGeneration}};
    require(host.postJson(jsonWide(init)), "WebView rejected initialization");
    require(waitUntil([&] {
        return state->pageReady || !state->protocolError.empty() ||
            !state->callbackError.empty();
    }, kStartupTimeoutMs), "renderer did not acknowledge initialization");
    if (!state->callbackError.empty()) {
        fail("WebView failed while initializing the renderer: " + narrow(state->callbackError));
    }
    require(state->protocolError.empty(), state->protocolError);

    struct ScriptState final {
        bool done = false;
        HRESULT result = E_FAIL;
    };
    auto scriptState = std::make_shared<ScriptState>();
    require(host.executeScriptForTest(kRenderProbeScript,
        [scriptState](HRESULT result, const std::wstring&) {
            scriptState->result = result;
            scriptState->done = true;
        }), "WebView rejected the xterm inspection script");
    require(waitUntil([&] {
        return scriptState->done || !state->callbackError.empty();
    }, kScriptTimeoutMs), "xterm inspection script did not complete");
    require(scriptState->done && SUCCEEDED(scriptState->result),
        "xterm inspection script failed");
    require(waitUntil([&] {
        return state->hookInstalled || !state->protocolError.empty();
    }, kScriptTimeoutMs), "xterm write probe did not report installation");
    require(state->protocolError.empty(), state->protocolError);
    require(state->hookInstalled, "xterm write probe was not installed");

    BoundedOutputBridge bridge;
    bridge.beginGeneration(kGeneration);
    require(!bridge.tryPush(OutputChunk{kGeneration - 1, 1, {'s', 't', 'a', 'l', 'e'}}),
        "bounded bridge accepted a stale generation");

    const std::vector<std::uint8_t> staleBytes{'S', 'T', 'A', 'L', 'E', '\r', '\n'};
    const json stale = {{"type", "output"}, {"generation", kGeneration - 1},
        {"id", 900000u}, {"data", base64Encode(staleBytes)}};
    require(host.postJson(jsonWide(stale)), "WebView rejected the stale test message");
    require(waitUntil([&] {
        return state->renderAckCount != 0 || !state->protocolError.empty();
    }, kStaleMessageTimeoutMs) == false,
        "stale generation output reached xterm");
    require(state->protocolError.empty(), state->protocolError);

    for (const ExpectedChunk& chunk : expected) {
        require(bridge.tryPush(OutputChunk{kGeneration, chunk.id, chunk.data}),
            "bounded bridge rejected a current-generation fixture chunk");
    }

    std::size_t maxInFlight = 0;
    bool backpressureObserved = false;
    std::size_t acknowledgedBridgeChunks = 0;
    const auto drain = [&] {
        while (acknowledgedBridgeChunks < state->pageAckIds.size()) {
            bridge.acknowledge(kGeneration, state->pageAckIds[acknowledgedBridgeChunks]);
            ++acknowledgedBridgeChunks;
        }
        for (;;) {
            auto chunk = bridge.takeForSend();
            if (!chunk) break;
            maxInFlight = std::max(maxInFlight, bridge.inFlightBytes());
            postOutput(host, ExpectedChunk{chunk->id, std::move(chunk->data), 0, 0});
        }
        if (bridge.queuedBytes() != 0 && bridge.inFlightBytes() != 0) {
            backpressureObserved = true;
        }
        bridge.onDispatchHandled();
        pumpMessages();
        while (acknowledgedBridgeChunks < state->pageAckIds.size()) {
            bridge.acknowledge(kGeneration, state->pageAckIds[acknowledgedBridgeChunks]);
            ++acknowledgedBridgeChunks;
        }
    };

    const bool drained = waitUntil([&] {
        drain();
        if (!state->protocolError.empty() || !state->callbackError.empty()) return true;
        return state->pageAckCount == expected.size() &&
            state->renderAckCount == expected.size() && bridge.queuedBytes() == 0 &&
            bridge.inFlightBytes() == 0;
    }, kDrainTimeoutMs);
    require(state->protocolError.empty(), state->protocolError);
    require(state->callbackError.empty(), "WebView failed during output: " + narrow(state->callbackError));
    require(drained, "renderer output did not drain to final acknowledgement");
    require(state->renderAckCount == expected.size() &&
        state->pageAckCount == expected.size(),
        "renderer did not report every xterm write and page acknowledgement");
    require(state->finalTailSeen, "xterm public buffer did not expose the final tail");
    require(maxInFlight <= BoundedOutputBridge::kInFlightLimitBytes,
        "bounded bridge exceeded its in-flight byte limit");
    require(backpressureObserved, "bounded bridge never applied output backpressure");

    require(host.closeAndWaitForCleanupForTest(), "renderer fixture cleanup did not finish");
    require(!host.isReady(), "WebViewHost remained ready after normal close");
    std::cout << "WEBVIEW_RENDER PASS bytes=" << expected.back().cumulativeBytes
        << " chunks=" << expected.size() << " checksum="
        << expected.back().cumulativeChecksum << " tail=TAIL-RENDER-END\n";
}

} // namespace nppterminal::tests
