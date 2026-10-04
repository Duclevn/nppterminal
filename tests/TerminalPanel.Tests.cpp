#include "TerminalPanel.Tests.h"

#include "Notepad_plus_msgs.h"
#include "TerminalPanel.h"
#include "resource.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace nppterminal {

// Narrow accessors keep the production class free of test actions while
// allowing this fixture to assert the serialized state transitions that are
// otherwise only visible through the WebView/session callbacks.
class TerminalPanelTestAccess final {
public:
    static void setSettings(TerminalPanel& panel, const std::wstring& directory)
    {
        panel.settingsLoaded_ = true;
        panel.settings_.defaultShell = L"cmd";
        panel.settings_.defaultDirectory = directory;
        panel.settings_.confirmBeforeKill = true;
        panel.selectedShellId_ = L"cmd";
        panel.selectedShellExplicit_ = true;
        panel.testConfirmation_ = true;
        panel.testOutput_.clear();
    }

    static void setConfirmation(TerminalPanel& panel, bool value)
    {
        panel.testConfirmation_ = value;
    }

    static void setOutput(TerminalPanel& panel, const std::string& value = {})
    {
        panel.testOutput_ = value;
        panel.testOutputDroppedBytes_ = 0;
        panel.testLastInputAckId_ = 0;
        panel.testLastOutputId_ = 0;
        panel.testLastOutputAckId_ = 0;
    }

    static const std::string& output(const TerminalPanel& panel)
    {
        return panel.testOutput_;
    }

    static std::size_t outputDroppedBytes(const TerminalPanel& panel)
    {
        return panel.testOutputDroppedBytes_;
    }

    static bool webViewReady(const TerminalPanel& panel)
    {
        return panel.webView_.isReady();
    }

    static bool pageReady(const TerminalPanel& panel)
    {
        return panel.pageReady_;
    }

    static std::uint64_t pageGeneration(const TerminalPanel& panel)
    {
        return panel.pageGeneration_;
    }

    static std::uint64_t sessionGeneration(const TerminalPanel& panel)
    {
        return panel.session_.generation();
    }

    static SessionState state(const TerminalPanel& panel)
    {
        return panel.visibleState_;
    }

    static std::wstring status(const TerminalPanel& panel)
    {
        wchar_t text[2048] = {};
        ::GetWindowTextW(panel.statusLabel_, text, static_cast<int>(std::size(text)));
        return text;
    }

    static DWORD processId(const TerminalPanel& panel)
    {
        return panel.session_.processId();
    }

    static DWORD brokerProcessId(const TerminalPanel& panel)
    {
        return panel.session_.brokerProcessId();
    }

    static bool sessionProcess(const TerminalPanel& panel)
    {
        return panel.session_.hasProcess();
    }

    static bool discoveryPending(const TerminalPanel& panel)
    {
        return panel.discoveryPending_;
    }

    static bool pendingOpenHere(const TerminalPanel& panel)
    {
        return panel.pendingOpenHere_;
    }

    static bool restartPending(const TerminalPanel& panel)
    {
        return panel.restartPending_;
    }

    static std::wstring pendingOpenHereDirectory(const TerminalPanel& panel)
    {
        return panel.openHereDirectory_;
    }

    static bool pendingDirectorySnapshot(const TerminalPanel& panel)
    {
        return panel.pendingDirectoryCandidates_.has_value();
    }

    static std::wstring pendingDirectoryKey(const TerminalPanel& panel)
    {
        return panel.pendingDirectoryKey_;
    }

    static void refresh(TerminalPanel& panel)
    {
        panel.refreshShellCatalog();
    }

    static void openHere(TerminalPanel& panel)
    {
        panel.openTerminalHere();
    }

    static void kill(TerminalPanel& panel, StopReason reason = StopReason::UserKill)
    {
        panel.killSession(reason);
    }

    static void input(TerminalPanel& panel, const std::string& value, std::uint64_t id)
    {
        const nlohmann::json message = { {"type", "input"},
            {"generation", panel.pageGeneration_}, {"id", id}, {"data", value} };
        panel.handleProtocol(message.dump());
    }

    static bool sessionPumpPosted(const TerminalPanel& panel)
    {
        return panel.sessionPumpPosted_;
    }

    static bool inputPumpBurstActive(const TerminalPanel& panel)
    {
        return panel.inputPumpBurstDeadline_ != 0;
    }

    static std::uint64_t sessionPumpCount(const TerminalPanel& panel)
    {
        return panel.testSessionPumpCount_;
    }

    static std::uint64_t inputAckId(const TerminalPanel& panel)
    {
        return panel.testLastInputAckId_;
    }

    static std::uint64_t lastOutputId(const TerminalPanel& panel)
    {
        return panel.testLastOutputId_;
    }

    static std::uint64_t lastOutputAckId(const TerminalPanel& panel)
    {
        return panel.testLastOutputAckId_;
    }

    static void resize(TerminalPanel& panel, std::uint16_t columns, std::uint16_t rows)
    {
        const nlohmann::json message = { {"type", "resize"},
            {"generation", panel.pageGeneration_}, {"cols", columns}, {"rows", rows} };
        panel.handleProtocol(message.dump());
    }

    static bool closeWebViewAndWait(TerminalPanel& panel)
    {
        return panel.webView_.closeAndWaitForCleanupForTest();
    }

};

} // namespace nppterminal

namespace nppterminal::tests {

namespace {

using nppterminal::TerminalPanelTestAccess;

constexpr wchar_t kWindowClass[] = L"NppTerminalPanelTestNppWindow-v1";

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error("TERMINAL_PANEL FAIL: " + message);
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

// ConPTY can add VT presentation controls and physical line wrapping to a
// long directory.  Strip only those presentation bytes in this test fixture;
// the production renderer remains responsible for terminal parsing.
std::string printedText(const std::string& bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(bytes[index]);
        if (ch == 0x1b && index + 1 < bytes.size()) {
            if (bytes[index + 1] == '[') {
                index += 2;
                while (index < bytes.size() &&
                    (static_cast<unsigned char>(bytes[index]) < 0x40 ||
                        static_cast<unsigned char>(bytes[index]) > 0x7e)) {
                    ++index;
                }
                continue;
            }
            if (bytes[index + 1] == ']') {
                index += 2;
                while (index < bytes.size() && bytes[index] != '\a') {
                    if (bytes[index] == '\x1b' && index + 1 < bytes.size() &&
                        bytes[index + 1] == '\\') {
                        ++index;
                        break;
                    }
                    ++index;
                }
                continue;
            }
            ++index;
            continue;
        }
        if (ch >= 0x20 && ch != 0x7f) text.push_back(static_cast<char>(ch));
    }
    return text;
}

// Keep timeout failures useful without dumping the full (potentially noisy)
// ConPTY transcript.  Escaping controls preserves enough VT/line information
// to distinguish an echoed command from a response that the text projection
// could not recognize.
std::string diagnosticTail(const std::string& bytes)
{
    constexpr std::size_t kDiagnosticTailBytes = 768;
    constexpr char kHex[] = "0123456789abcdef";
    const std::size_t start = bytes.size() > kDiagnosticTailBytes ?
        bytes.size() - kDiagnosticTailBytes : 0;
    std::string result;
    result.reserve((bytes.size() - start) * 2);
    for (std::size_t index = start; index < bytes.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(bytes[index]);
        if (ch >= 0x20 && ch <= 0x7e) {
            result.push_back(static_cast<char>(ch));
        } else {
            result += "\\x";
            result.push_back(kHex[ch >> 4]);
            result.push_back(kHex[ch & 0x0f]);
        }
    }
    return result;
}

void pumpMessages()
{
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            ::PostQuitMessage(static_cast<int>(message.wParam));
            throw std::runtime_error("TERMINAL_PANEL FAIL: panel fixture received WM_QUIT");
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

std::wstring uniqueDirectory(const wchar_t* label)
{
    wchar_t temp[MAX_PATH * 4] = {};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    require(length != 0 && length < std::size(temp), "GetTempPathW failed");
    GUID guid{};
    require(SUCCEEDED(::CoCreateGuid(&guid)), "CoCreateGuid failed");
    wchar_t guidText[64] = {};
    require(::StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) != 0,
        "StringFromGUID2 failed");
    std::wstring root(temp, length);
    if (!root.empty() && root.back() == L'\\') root.pop_back();
    root += L"\\NppTerminalPanel.Tests-";
    root += label;
    root += L"-";
    root += guidText;
    require(::CreateDirectoryW(root.c_str(), nullptr) != FALSE,
        "panel fixture root creation failed");
    return root;
}

std::wstring panelTestExecutablePath()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path,
        static_cast<DWORD>(std::size(path)));
    require(length != 0 && length < std::size(path),
        "panel test executable path could not be resolved");
    return std::wstring(path, length);
}

bool createDirectory(const std::wstring& path)
{
    return ::CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
        ::GetLastError() == ERROR_ALREADY_EXISTS;
}

HANDLE pinDirectory(const std::wstring& path)
{
    return ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

struct Fixture final {
    std::wstring root;
    std::wstring rootA;
    std::wstring rootB;
    std::wstring config;
    std::wstring fileA;
    std::wstring fileB;
    HANDLE rootHandle = nullptr;
    HANDLE rootAHandle = nullptr;
    HANDLE rootBHandle = nullptr;
    HANDLE configHandle = nullptr;

    Fixture()
    {
        root = uniqueDirectory(L"fixture");
        rootA = root + L"\\RootA";
        rootB = root + L"\\RootB";
        config = root + L"\\Config";
        require(createDirectory(rootA) && createDirectory(rootB) && createDirectory(config),
            "panel fixture directory creation failed");
        fileA = rootA + L"\\active.txt";
        fileB = rootB + L"\\active.txt";
        for (const std::wstring& file : {fileA, fileB}) {
            HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "panel fixture file creation failed");
            ::CloseHandle(handle);
        }
        rootHandle = pinDirectory(root);
        rootAHandle = pinDirectory(rootA);
        rootBHandle = pinDirectory(rootB);
        configHandle = pinDirectory(config);
        require(rootHandle != INVALID_HANDLE_VALUE && rootAHandle != INVALID_HANDLE_VALUE &&
            rootBHandle != INVALID_HANDLE_VALUE && configHandle != INVALID_HANDLE_VALUE,
            "panel fixture directory pin failed");
    }

    ~Fixture()
    {
        // Remove only this fixture's files while the directory handles still
        // pin the owned ancestry.  Close child handles before removing their
        // directories; release the root pin only immediately before removing
        // the root itself.
        ::DeleteFileW(fileA.c_str());
        ::DeleteFileW(fileB.c_str());
        const auto closeDirectoryHandle = [](HANDLE& handle) {
            if (handle && handle != INVALID_HANDLE_VALUE) {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        };
        closeDirectoryHandle(configHandle);
        closeDirectoryHandle(rootBHandle);
        closeDirectoryHandle(rootAHandle);
        ::RemoveDirectoryW(config.c_str());
        ::RemoveDirectoryW(rootB.c_str());
        ::RemoveDirectoryW(rootA.c_str());
        if (rootHandle && rootHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(rootHandle);
            rootHandle = nullptr;
        }
        ::RemoveDirectoryW(root.c_str());
    }
};

struct FakeNppState final {
    std::wstring currentFile;
    std::wstring configDirectory;
    HWND panel = nullptr;
};

LRESULT CALLBACK fakeNppProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<FakeNppState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<FakeNppState*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return ::DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case NPPM_GETCURRENTSCINTILLA:
        if (lParam) *reinterpret_cast<int*>(lParam) = 0;
        return TRUE;
    case NPPM_GETFULLCURRENTPATH: {
        if (!lParam || wParam == 0) return FALSE;
        auto* buffer = reinterpret_cast<wchar_t*>(lParam);
        if (state->currentFile.size() + 1 > static_cast<std::size_t>(wParam)) return FALSE;
        std::copy(state->currentFile.begin(), state->currentFile.end(), buffer);
        buffer[state->currentFile.size()] = L'\0';
        return TRUE;
    }
    case NPPM_GETPLUGINSCONFIGDIR: {
        if (!lParam) return static_cast<LRESULT>(state->configDirectory.size());
        if (wParam == 0 || state->configDirectory.size() + 1 > static_cast<std::size_t>(wParam)) {
            return FALSE;
        }
        auto* buffer = reinterpret_cast<wchar_t*>(lParam);
        std::copy(state->configDirectory.begin(), state->configDirectory.end(), buffer);
        buffer[state->configDirectory.size()] = L'\0';
        return TRUE;
    }
    case NPPM_ISDARKMODEENABLED:
        return FALSE;
    case NPPM_GETDARKMODECOLORS:
        return FALSE;
    case NPPM_DMMREGASDCKDLG: {
        auto* data = reinterpret_cast<tTbData*>(lParam);
        state->panel = data ? data->hClient : nullptr;
        return state->panel ? TRUE : FALSE;
    }
    case NPPM_DMMSHOW:
        if (lParam && ::IsWindow(reinterpret_cast<HWND>(lParam))) {
            ::ShowWindow(reinterpret_cast<HWND>(lParam), SW_SHOW);
            state->panel = reinterpret_cast<HWND>(lParam);
        }
        return TRUE;
    case NPPM_DMMHIDE:
        if (lParam && ::IsWindow(reinterpret_cast<HWND>(lParam))) {
            ::ShowWindow(reinterpret_cast<HWND>(lParam), SW_HIDE);
        }
        return TRUE;
    case NPPM_DMMUPDATEDISPINFO:
    case NPPM_MODELESSDIALOG:
    case NPPM_DARKMODESUBCLASSANDTHEME:
        return TRUE;
    default:
        break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

struct WindowGuard final {
    HWND window = nullptr;
    HMODULE module = nullptr;
    bool registered = false;

    ~WindowGuard()
    {
        if (window) ::DestroyWindow(window);
        if (registered) ::UnregisterClassW(kWindowClass, module);
    }
};

struct ApartmentGuard final {
    ~ApartmentGuard()
    {
        ::CoUninitialize();
    }
};

struct PanelGuard final {
    TerminalPanel* panel = nullptr;

    ~PanelGuard()
    {
        if (panel) {
            panel->shutdownForHost();
        }
    }
};

struct HandleGuard final {
    HANDLE handle = nullptr;

    ~HandleGuard()
    {
        if (handle && handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    }
};

void selectFile(FakeNppState& state, const std::wstring& file)
{
    state.currentFile = file;
}

std::string panelStateMetadata(const TerminalPanel& panel, const char* phase)
{
    return std::string("phase=") + phase +
        " state=" + std::to_string(static_cast<int>(TerminalPanelTestAccess::state(panel))) +
        " hasProcess=" + (TerminalPanelTestAccess::sessionProcess(panel) ? "1" : "0") +
        " discoveryPending=" + (TerminalPanelTestAccess::discoveryPending(panel) ? "1" : "0") +
        " pageReady=" + (TerminalPanelTestAccess::pageReady(panel) ? "1" : "0") +
        " webViewReady=" + (TerminalPanelTestAccess::webViewReady(panel) ? "1" : "0") +
        " restartPending=" + (TerminalPanelTestAccess::restartPending(panel) ? "1" : "0") +
        " pendingOpenHere=" + (TerminalPanelTestAccess::pendingOpenHere(panel) ? "1" : "0") +
        " sessionPumpPosted=" + (TerminalPanelTestAccess::sessionPumpPosted(panel) ? "1" : "0") +
        " inputPumpBurst=" + (TerminalPanelTestAccess::inputPumpBurstActive(panel) ? "1" : "0") +
        " sessionGeneration=" + std::to_string(TerminalPanelTestAccess::sessionGeneration(panel)) +
        " pageGeneration=" + std::to_string(TerminalPanelTestAccess::pageGeneration(panel));
}

void requireRunning(TerminalPanel& panel, DWORD timeoutMs, const char* phase)
{
    if (!waitUntil([&] {
        return TerminalPanelTestAccess::state(panel) == SessionState::Running &&
            TerminalPanelTestAccess::sessionProcess(panel);
    }, timeoutMs)) {
        throw std::runtime_error("TERMINAL_PANEL FAIL: terminal panel did not reach Running; " +
            panelStateMetadata(panel, phase) + "; status=" + narrow(TerminalPanelTestAccess::status(panel)));
    }
}

void requireStopped(TerminalPanel& panel, DWORD timeoutMs)
{
    require(waitUntil([&] {
        return TerminalPanelTestAccess::state(panel) == SessionState::NoSession &&
            !TerminalPanelTestAccess::sessionProcess(panel) &&
            !TerminalPanelTestAccess::discoveryPending(panel);
    }, timeoutMs), "terminal panel session did not stop");
}

void sendCwdQuery(TerminalPanel& panel, const std::string& expectedDirectory,
    std::uint64_t id, const char* phase)
{
    TerminalPanelTestAccess::setOutput(panel);
    // Compare the real shell value before emitting a short marker.  Capturing
    // the full path in terminal output makes this lifecycle check depend on
    // ConPTY cursor rewrites when the panel is only 100 columns wide.
    const std::string marker = "[NPP_PANEL_CWD_" + std::to_string(id) + "_MATCH]";
    // Escape one marker character in the command itself so the terminal's
    // command echo cannot satisfy the marker search before CMD evaluates IF.
    const std::string commandMarker = "[NPP_PANEL_CWD_" + std::to_string(id) +
        "^_MATCH]";
    const std::string command = "if /I \"%CD%\"==\"" + expectedDirectory +
        "\" echo " + commandMarker + "\r\n";
    TerminalPanelTestAccess::input(panel, command, id);
    const std::string expected = marker;
    if (!waitUntil([&] {
        return printedText(TerminalPanelTestAccess::output(panel)).find(expected) !=
            std::string::npos;
    }, 10000)) {
        const std::string& output = TerminalPanelTestAccess::output(panel);
        const std::string rendered = printedText(output);
        throw std::runtime_error("TERMINAL_PANEL FAIL: controlled CMD working-directory marker was not observed; " +
            panelStateMetadata(panel, phase) +
            " id=" + std::to_string(id) +
            " expected=" + expected +
            " captured_bytes=" + std::to_string(output.size()) +
            " dropped_bytes=" +
                std::to_string(TerminalPanelTestAccess::outputDroppedBytes(panel)) +
            " input_ack=" + std::to_string(TerminalPanelTestAccess::inputAckId(panel)) +
            " last_output_id=" + std::to_string(TerminalPanelTestAccess::lastOutputId(panel)) +
            " last_render_ack=" +
                std::to_string(TerminalPanelTestAccess::lastOutputAckId(panel)) +
            " session_pumps=" +
            std::to_string(TerminalPanelTestAccess::sessionPumpCount(panel)) +
            " status=" + narrow(TerminalPanelTestAccess::status(panel)) +
            " output_tail=" + diagnosticTail(output) +
            " text_tail=" + diagnosticTail(rendered));
    }
}

struct OutputTiming final {
    std::size_t bytes = 0;
    std::uint64_t milliseconds = 0;
};

struct InputTiming final {
    std::size_t bytes = 0;
    std::uint64_t outputMilliseconds = 0;
    std::uint64_t ackMilliseconds = 0;
    std::uint64_t renderMilliseconds = 0;
};

OutputTiming waitForOutputMarker(TerminalPanel& panel, const std::string& marker,
    std::size_t startingBytes, DWORD timeoutMs)
{
    const auto started = std::chrono::steady_clock::now();
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    bool markerSeen = false;
    std::uint64_t markerOutputId = 0;
    for (;;) {
        pumpMessages();
        const std::string& output = TerminalPanelTestAccess::output(panel);
        if (!markerSeen && output.find(marker) != std::string::npos) {
            markerSeen = true;
            markerOutputId = TerminalPanelTestAccess::lastOutputId(panel);
        }
        if (markerSeen && markerOutputId != 0 &&
            TerminalPanelTestAccess::lastOutputAckId(panel) >= markerOutputId) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            return OutputTiming{output.size() - startingBytes,
                static_cast<std::uint64_t>(std::max<std::int64_t>(1, elapsed))};
        }
        if (::GetTickCount64() >= deadline) break;
        ::Sleep(1);
    }
    const std::string& output = TerminalPanelTestAccess::output(panel);
    throw std::runtime_error("TERMINAL_PANEL FAIL: output marker was not observed: " + marker +
        " captured_bytes=" + std::to_string(output.size()) +
        " dropped_bytes=" + std::to_string(TerminalPanelTestAccess::outputDroppedBytes(panel)) +
        " last_output_id=" + std::to_string(TerminalPanelTestAccess::lastOutputId(panel)) +
        " last_render_ack=" + std::to_string(
            TerminalPanelTestAccess::lastOutputAckId(panel)) +
        " state=" + std::to_string(static_cast<int>(TerminalPanelTestAccess::state(panel))) +
        " status=" + narrow(TerminalPanelTestAccess::status(panel)));
}

InputTiming waitForInputResponse(TerminalPanel& panel, const std::string& marker,
    std::uint64_t id, std::size_t startingBytes, DWORD timeoutMs)
{
    const auto started = std::chrono::steady_clock::now();
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    bool outputSeen = false;
    bool ackSeen = false;
    bool renderSeen = false;
    std::uint64_t outputMilliseconds = 0;
    std::uint64_t ackMilliseconds = 0;
    std::uint64_t markerOutputId = 0;
    std::uint64_t renderMilliseconds = 0;
    for (;;) {
        pumpMessages();
        const auto elapsed = static_cast<std::uint64_t>(std::max<std::int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()));
        const std::string& output = TerminalPanelTestAccess::output(panel);
        if (!outputSeen && output.find(marker) != std::string::npos) {
            outputSeen = true;
            outputMilliseconds = elapsed;
            markerOutputId = TerminalPanelTestAccess::lastOutputId(panel);
        }
        if (!ackSeen && TerminalPanelTestAccess::inputAckId(panel) == id) {
            ackSeen = true;
            ackMilliseconds = elapsed;
        }
        if (!renderSeen && outputSeen && markerOutputId != 0 &&
            TerminalPanelTestAccess::lastOutputAckId(panel) >= markerOutputId) {
            renderSeen = true;
            renderMilliseconds = elapsed;
        }
        if (outputSeen && ackSeen && renderSeen) {
            return InputTiming{output.size() - startingBytes, outputMilliseconds,
                ackMilliseconds, renderMilliseconds};
        }
        if (::GetTickCount64() >= deadline) break;
        ::Sleep(1);
    }
    throw std::runtime_error("TERMINAL_PANEL FAIL: input output/ack response was not observed");
}

std::uint64_t percentile95(std::vector<std::uint64_t> values)
{
    require(!values.empty(), "timing sample set was empty");
    std::sort(values.begin(), values.end());
    const std::size_t index = (values.size() * 95u + 99u) / 100u - 1u;
    return values[std::min(index, values.size() - 1u)];
}

std::uint64_t percentile50(std::vector<std::uint64_t> values)
{
    require(!values.empty(), "timing sample set was empty");
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1u) / 2u];
}

} // namespace

void runTerminalPanelTests()
{
    const HMODULE module = ::GetModuleHandleW(nullptr);
    require(module != nullptr, "panel test module could not be resolved");
    const HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    require(apartment == S_OK || apartment == S_FALSE,
        "panel tests require a single-threaded COM apartment");
    ApartmentGuard apartmentGuard;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &fakeNppProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = kWindowClass;
    const ATOM atom = ::RegisterClassW(&windowClass);
    const bool registeredHere = atom != 0;
    require(registeredHere || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "panel fixture window class registration failed");

    Fixture fixture;
    FakeNppState nppState{fixture.fileA, fixture.config};
    const HWND npp = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"NppTerminal panel fixture", WS_POPUP,
        0, 0, 1200, 800, nullptr, nullptr, module, &nppState);
    require(npp != nullptr, "panel fixture Npp window creation failed");
    WindowGuard windowGuard{npp, module, registeredHere};
    ::ShowWindow(npp, SW_SHOWNOACTIVATE);
    ::UpdateWindow(npp);

    // Warm common-control caches before measuring repeated lazy dock lifetimes.
    // These docks must release their newly owned DPI-specific fonts without
    // creating a WebView or session.
    {
        TerminalPanel warmPanel(module, npp, 77);
        require(warmPanel.createDock(), "font lifetime warm-up dock creation failed");
        warmPanel.destroy();
    }
    const DWORD baselineGdiObjects = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    for (int cycle = 0; cycle < 12; ++cycle) {
        TerminalPanel idlePanel(module, npp, 77);
        require(idlePanel.createDock(), "font lifetime dock creation failed");
        require(!TerminalPanelTestAccess::webViewReady(idlePanel) &&
            !TerminalPanelTestAccess::sessionProcess(idlePanel),
            "font lifetime check eagerly started terminal resources");
        idlePanel.destroy();
        require(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS) <= baselineGdiObjects,
            "lazy dock destruction leaked GDI font or control objects");
    }

    auto panel = std::make_unique<TerminalPanel>(module, npp, 77);
    PanelGuard panelGuard{panel.get()};
    require(panel->createDock(), "TerminalPanel lazy dock creation failed");
    TerminalPanelTestAccess::setSettings(*panel, fixture.rootA);
    require(!TerminalPanelTestAccess::webViewReady(*panel) &&
        !TerminalPanelTestAccess::sessionProcess(*panel),
        "createDock eagerly created the renderer or shell");

    panel->toggleByUser();
    require(waitUntil([&] { return TerminalPanelTestAccess::pageReady(*panel); }, 15000),
        "panel WebView page did not become ready");
    requireRunning(*panel, 20000, "initial-start");
    const std::uint64_t firstGeneration = TerminalPanelTestAccess::sessionGeneration(*panel);
    const DWORD firstProcess = TerminalPanelTestAccess::processId(*panel);
    const DWORD firstBroker = TerminalPanelTestAccess::brokerProcessId(*panel);
    require(firstGeneration != 0 && firstProcess != 0 && firstBroker != 0,
        "panel session did not expose owned process identities");
    HandleGuard firstShellHandle{::OpenProcess(SYNCHRONIZE, FALSE, firstProcess)};
    HandleGuard firstBrokerHandle{::OpenProcess(SYNCHRONIZE, FALSE, firstBroker)};
    require(firstShellHandle.handle && firstShellHandle.handle != INVALID_HANDLE_VALUE &&
        firstBrokerHandle.handle && firstBrokerHandle.handle != INVALID_HANDLE_VALUE,
        "panel fixture could not capture original process handles");
    sendCwdQuery(*panel, narrow(fixture.rootA), 1, "initial");

    // Input must schedule a prompt session pump immediately. The child writes
    // a buffered stream through the same ConPTY and renderer path, avoiding
    // shell command production time in the transport throughput measurement.
    TerminalPanelTestAccess::setOutput(*panel);
    const std::string streamMarker = "NPP_PANEL_STREAM_DONE";
    const std::string streamCommand = "\"" + narrow(panelTestExecutablePath()) +
        "\" --panel-stream-child\r\n";
    const std::size_t streamStartBytes = TerminalPanelTestAccess::output(*panel).size();
    TerminalPanelTestAccess::input(*panel, streamCommand, 100);
    require(TerminalPanelTestAccess::sessionPumpPosted(*panel),
        "panel input did not post immediate session work");
    const OutputTiming streamTiming = waitForOutputMarker(*panel, streamMarker,
        streamStartBytes, 10000);
    require(streamTiming.bytes >= 256u * 1024u,
        "panel transport regression did not receive a substantial output stream");
    const double streamMiBPerSecond = static_cast<double>(streamTiming.bytes) /
        static_cast<double>(streamTiming.milliseconds) * 1000.0 / (1024.0 * 1024.0);
    require(streamMiBPerSecond > 1.1,
        "panel transport throughput remained at the timer-bound rate");
    std::wcout << L"TERMINAL_PANEL TRANSPORT bytes=" << streamTiming.bytes
        << L" elapsed_ms=" << streamTiming.milliseconds << L" MiB_per_s="
        << streamMiBPerSecond << L"\n";

    std::vector<std::uint64_t> outputLatencies;
    std::vector<std::uint64_t> ackLatencies;
    std::vector<std::uint64_t> renderLatencies;
    for (std::uint64_t sample = 0; sample < 20; ++sample) {
        TerminalPanelTestAccess::setOutput(*panel);
        const std::string inputMarker = "NPP_PANEL_INPUT_LATENCY_" + std::to_string(sample);
        const std::size_t inputStartBytes = TerminalPanelTestAccess::output(*panel).size();
        const std::uint64_t inputId = 101 + sample;
        TerminalPanelTestAccess::input(*panel, "echo " + inputMarker + "\r\n", inputId);
        require(TerminalPanelTestAccess::sessionPumpPosted(*panel),
            "interactive input did not schedule a session pump");
        require(TerminalPanelTestAccess::inputPumpBurstActive(*panel),
            "interactive input did not start its bounded poll burst");
        const InputTiming inputTiming = waitForInputResponse(*panel, inputMarker, inputId,
            inputStartBytes, 1000);
        outputLatencies.push_back(inputTiming.outputMilliseconds);
        ackLatencies.push_back(inputTiming.ackMilliseconds);
        renderLatencies.push_back(inputTiming.renderMilliseconds);
    }
    const std::uint64_t outputP50 = percentile50(outputLatencies);
    const std::uint64_t outputP95 = percentile95(outputLatencies);
    const std::uint64_t ackP50 = percentile50(ackLatencies);
    const std::uint64_t ackP95 = percentile95(ackLatencies);
    const std::uint64_t renderP50 = percentile50(renderLatencies);
    const std::uint64_t renderP95 = percentile95(renderLatencies);
    std::wcout << L"TERMINAL_PANEL INPUT output_p50_ms=" << outputP50
        << L" output_p95_ms=" << outputP95 << L" ack_p50_ms=" << ackP50
        << L" ack_p95_ms=" << ackP95 << L" render_p50_ms=" << renderP50
        << L" render_p95_ms=" << renderP95 << L"\n";
    for (std::size_t sample = 0; sample < outputLatencies.size(); ++sample) {
        std::wcout << L"TERMINAL_PANEL INPUT_SAMPLE index=" << sample
            << L" output_ms=" << outputLatencies[sample]
            << L" ack_ms=" << ackLatencies[sample]
            << L" render_ms=" << renderLatencies[sample] << L"\n";
    }

    // Keep a real child producing output while probes are sent directly to
    // its stdin. The unique response marker can only be emitted after the
    // child receives that probe, so command echo is not counted as success.
    TerminalPanelTestAccess::setOutput(*panel);
    const std::string streamingReadyMarker = "NPP_PANEL_STREAMING_READY";
    const std::string streamingChildExitedMarker = "NPP_PANEL_STREAM_CHILD_EXITED";
    const std::string streamingCommand = "\"" + narrow(panelTestExecutablePath()) +
        "\" --panel-stream-input-child && echo " + streamingChildExitedMarker + "\r\n";
    const std::size_t streamingStartBytes = TerminalPanelTestAccess::output(*panel).size();
    TerminalPanelTestAccess::input(*panel, streamingCommand, 1000);
    require(TerminalPanelTestAccess::sessionPumpPosted(*panel),
        "streaming child launch did not post immediate session work");
    waitForOutputMarker(*panel, streamingReadyMarker, streamingStartBytes, 10000);

    std::vector<std::uint64_t> streamingOutputLatencies;
    std::vector<std::uint64_t> streamingAckLatencies;
    std::vector<std::uint64_t> streamingRenderLatencies;
    std::vector<std::size_t> streamingBytes;
    for (std::uint64_t sample = 0; sample < 20; ++sample) {
        TerminalPanelTestAccess::setOutput(*panel);
        const std::string responseMarker = "NPP_PANEL_STREAM_RESPONSE_" +
            std::to_string(sample);
        const std::uint64_t inputId = 1100 + sample;
        TerminalPanelTestAccess::input(*panel,
            "NPP_PANEL_STREAM_PROBE_" + std::to_string(sample) + "\r\n", inputId);
        require(TerminalPanelTestAccess::sessionPumpPosted(*panel),
            "streaming input did not schedule a session pump");
        const InputTiming timing = waitForInputResponse(*panel, responseMarker, inputId, 0, 2500);
        streamingBytes.push_back(timing.bytes);
        streamingOutputLatencies.push_back(timing.outputMilliseconds);
        streamingAckLatencies.push_back(timing.ackMilliseconds);
        streamingRenderLatencies.push_back(timing.renderMilliseconds);
    }
    const std::uint64_t streamingOutputP50 = percentile50(streamingOutputLatencies);
    const std::uint64_t streamingOutputP95 = percentile95(streamingOutputLatencies);
    const std::uint64_t streamingAckP50 = percentile50(streamingAckLatencies);
    const std::uint64_t streamingAckP95 = percentile95(streamingAckLatencies);
    const std::uint64_t streamingRenderP50 = percentile50(streamingRenderLatencies);
    const std::uint64_t streamingRenderP95 = percentile95(streamingRenderLatencies);
    std::wcout << L"TERMINAL_PANEL STREAMING_INPUT output_p50_ms=" << streamingOutputP50
        << L" output_p95_ms=" << streamingOutputP95 << L" ack_p50_ms=" << streamingAckP50
        << L" ack_p95_ms=" << streamingAckP95 << L" render_p50_ms=" << streamingRenderP50
        << L" render_p95_ms=" << streamingRenderP95 << L"\n";
    for (std::size_t sample = 0; sample < streamingOutputLatencies.size(); ++sample) {
        std::wcout << L"TERMINAL_PANEL STREAMING_INPUT_SAMPLE index=" << sample
            << L" bytes=" << streamingBytes[sample]
            << L" output_ms=" << streamingOutputLatencies[sample]
            << L" ack_ms=" << streamingAckLatencies[sample]
            << L" render_ms=" << streamingRenderLatencies[sample] << L"\n";
    }

    TerminalPanelTestAccess::setOutput(*panel);
    const std::string streamingStoppedMarker = "NPP_PANEL_STREAMING_STOPPED";
    const std::size_t streamingStopStartBytes = TerminalPanelTestAccess::output(*panel).size();
    TerminalPanelTestAccess::input(*panel, "NPP_PANEL_STREAM_STOP\r\n", 1200);
    waitForOutputMarker(*panel, streamingStoppedMarker, streamingStopStartBytes, 5000);
    const std::size_t streamingChildExitStartBytes =
        TerminalPanelTestAccess::output(*panel).size();
    waitForOutputMarker(*panel, streamingChildExitedMarker, streamingChildExitStartBytes, 5000);

    require(outputP95 < 50 && ackP95 < 50 && renderP95 < 50,
        "interactive input was not serviced promptly by the panel");

    const std::uint64_t pumpsBeforeHide = TerminalPanelTestAccess::sessionPumpCount(*panel);
    panel->toggleByUser();
    require(!panel->isVisible(), "panel fixture did not hide the terminal dock");
    require(!TerminalPanelTestAccess::inputPumpBurstActive(*panel),
        "hiding the panel retained the interactive poll burst");
    const ULONGLONG hiddenDeadline = ::GetTickCount64() + 250;
    while (::GetTickCount64() < hiddenDeadline) {
        pumpMessages();
        ::Sleep(1);
    }
    const std::uint64_t hiddenPumps = TerminalPanelTestAccess::sessionPumpCount(*panel) -
        pumpsBeforeHide;
    require(hiddenPumps <= 5, "hidden panel retained an overly frequent idle pump");
    std::wcout << L"TERMINAL_PANEL HIDDEN elapsed_ms=250 pump_count=" << hiddenPumps << L"\n";
    panel->toggleByUser();
    require(panel->isVisible(), "panel fixture did not restore the terminal dock");

    TerminalPanelTestAccess::resize(*panel, 100, 30);
    require(waitUntil([&] { return TerminalPanelTestAccess::pageGeneration(*panel) != 0; }, 1000),
        "panel resize lost its page generation");
    TerminalPanelTestAccess::refresh(*panel);
    require(waitUntil([&] { return !TerminalPanelTestAccess::discoveryPending(*panel); }, 10000),
        "Refresh did not finish its catalog discovery");
    require(TerminalPanelTestAccess::state(*panel) == SessionState::Running &&
        TerminalPanelTestAccess::sessionProcess(*panel) &&
        TerminalPanelTestAccess::sessionGeneration(*panel) == firstGeneration &&
        TerminalPanelTestAccess::processId(*panel) == firstProcess &&
        TerminalPanelTestAccess::brokerProcessId(*panel) == firstBroker,
        "Refresh replaced or lost the running panel session");
    sendCwdQuery(*panel, narrow(fixture.rootA), 2, "after-refresh");

    selectFile(nppState, fixture.fileB);
    TerminalPanelTestAccess::setConfirmation(*panel, false);
    TerminalPanelTestAccess::openHere(*panel);
    require(TerminalPanelTestAccess::sessionGeneration(*panel) == firstGeneration &&
        TerminalPanelTestAccess::processId(*panel) == firstProcess &&
        !TerminalPanelTestAccess::pendingOpenHere(*panel),
        "canceled Open Terminal Here changed the running session");

    TerminalPanelTestAccess::setConfirmation(*panel, true);
    TerminalPanelTestAccess::openHere(*panel);
    // The replacement must use the directory snapshot captured by Open Here,
    // even if the host's current file changes before discovery starts.
    selectFile(nppState, fixture.fileA);
    require(TerminalPanelTestAccess::pendingOpenHere(*panel),
        "accepted Open Terminal Here did not queue its directory");
    require(waitUntil([&] {
        return TerminalPanelTestAccess::state(*panel) == SessionState::Running &&
            TerminalPanelTestAccess::sessionProcess(*panel) &&
            TerminalPanelTestAccess::sessionGeneration(*panel) != firstGeneration;
    }, 20000), "accepted Open Terminal Here did not start a replacement session");
    require(waitUntil([&] {
        return ::WaitForSingleObject(firstShellHandle.handle, 0) == WAIT_OBJECT_0 &&
            ::WaitForSingleObject(firstBrokerHandle.handle, 0) == WAIT_OBJECT_0;
    }, 5000), "accepted Open Terminal Here left the original process family alive");
    sendCwdQuery(*panel, narrow(fixture.rootB), 3, "after-open-here");

    // Queue a different directory, then prove that a canceled Kill preserves
    // the request before the accepted Kill clears it.
    selectFile(nppState, fixture.fileA);
    TerminalPanelTestAccess::openHere(*panel);
    require(TerminalPanelTestAccess::pendingOpenHere(*panel),
        "queued Open Terminal Here request was not visible before Kill");
    const std::wstring queuedDirectory =
        TerminalPanelTestAccess::pendingOpenHereDirectory(*panel);
    TerminalPanelTestAccess::setConfirmation(*panel, false);
    TerminalPanelTestAccess::kill(*panel);
    require(TerminalPanelTestAccess::pendingOpenHere(*panel) &&
        TerminalPanelTestAccess::pendingOpenHereDirectory(*panel) == queuedDirectory &&
        TerminalPanelTestAccess::pendingDirectorySnapshot(*panel),
        "canceled Kill discarded the queued Open Terminal Here request");
    TerminalPanelTestAccess::setConfirmation(*panel, true);
    TerminalPanelTestAccess::kill(*panel);
    require(!TerminalPanelTestAccess::pendingOpenHere(*panel) &&
        TerminalPanelTestAccess::pendingOpenHereDirectory(*panel).empty() &&
        !TerminalPanelTestAccess::pendingDirectorySnapshot(*panel) &&
        TerminalPanelTestAccess::pendingDirectoryKey(*panel).empty() &&
        !TerminalPanelTestAccess::restartPending(*panel),
        "Kill left stale queued Open Terminal Here state");
    requireStopped(*panel, 10000);

    // An accepted Kill leaves the dock visible while releasing its renderer.
    // Exercise the real user sequence explicitly: hide that dock, then show
    // it again so the next start must recreate the renderer.
    require(panel->isVisible(), "accepted Kill unexpectedly hid the terminal dock");
    panel->toggleByUser();
    require(!panel->isVisible() && !TerminalPanelTestAccess::pageReady(*panel),
        "hiding the killed terminal did not leave the dock hidden without a page");

    // The current file now points at Root B while the abandoned explicit
    // request pointed at Root A, so a stale request cannot satisfy the next
    // normal start by coincidence.
    selectFile(nppState, fixture.fileB);
    panel->toggleByUser();
    require(panel->isVisible(), "showing the killed terminal did not restore the dock");
    require(waitUntil([&] { return TerminalPanelTestAccess::pageReady(*panel); }, 15000),
        "panel did not recreate the renderer after Kill");
    requireRunning(*panel, 20000, "post-kill-toggle");
    sendCwdQuery(*panel, narrow(fixture.rootB), 4, "after-kill");

    const std::uint64_t hostGeneration = TerminalPanelTestAccess::sessionGeneration(*panel);
    const DWORD hostProcess = TerminalPanelTestAccess::processId(*panel);
    panel->prepareForHostShutdown();
    pumpMessages();
    panel->cancelHostShutdown();
    require(TerminalPanelTestAccess::state(*panel) == SessionState::Running &&
        TerminalPanelTestAccess::sessionGeneration(*panel) == hostGeneration &&
        TerminalPanelTestAccess::processId(*panel) == hostProcess,
        "canceled host shutdown did not preserve the usable session");
    sendCwdQuery(*panel, narrow(fixture.rootB), 5, "after-host-cancel");

    TerminalPanelTestAccess::kill(*panel, StopReason::HostShutdown);
    requireStopped(*panel, 10000);
    require(TerminalPanelTestAccess::closeWebViewAndWait(*panel),
        "panel WebView cleanup did not finish");
    require(panel->shutdownForHost(), "panel final host shutdown did not stop cleanly");
    panelGuard.panel = nullptr;
    panel->destroy();
    panel.reset();

    std::wcout << L"TERMINAL_PANEL PASS lazy=1 refresh=1 openhere=1 kill-queue=1 host-cancel=1\n";
}

} // namespace nppterminal::tests
