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
    }

    static const std::string& output(const TerminalPanel& panel)
    {
        return panel.testOutput_;
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
    std::uint64_t id)
{
    TerminalPanelTestAccess::setOutput(panel);
    const std::string marker = "[NPP_PANEL_CWD_" + std::to_string(id) + "]";
    const std::string endMarker = "[/NPP_PANEL_CWD_" + std::to_string(id) + "]";
    TerminalPanelTestAccess::input(panel,
        "echo " + marker + "%CD%" + endMarker + "\r\n", id);
    const std::string expected = marker + expectedDirectory + endMarker;
    require(waitUntil([&] {
        return printedText(TerminalPanelTestAccess::output(panel)).find(expected) !=
            std::string::npos;
    }, 10000), "controlled CMD working-directory marker was not observed");
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
    sendCwdQuery(*panel, narrow(fixture.rootA), 1);

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
    sendCwdQuery(*panel, narrow(fixture.rootA), 2);

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
    sendCwdQuery(*panel, narrow(fixture.rootB), 3);

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

    // Toggle after the queued request was killed.  The current file now points
    // at Root B while the abandoned explicit request pointed at Root A, so a
    // stale request cannot satisfy the next normal start by coincidence.
    selectFile(nppState, fixture.fileB);
    panel->toggleByUser();
    require(waitUntil([&] { return TerminalPanelTestAccess::pageReady(*panel); }, 15000),
        "panel did not recreate the renderer after Kill");
    requireRunning(*panel, 20000, "post-kill-toggle");
    sendCwdQuery(*panel, narrow(fixture.rootB), 4);

    const std::uint64_t hostGeneration = TerminalPanelTestAccess::sessionGeneration(*panel);
    const DWORD hostProcess = TerminalPanelTestAccess::processId(*panel);
    panel->prepareForHostShutdown();
    pumpMessages();
    panel->cancelHostShutdown();
    require(TerminalPanelTestAccess::state(*panel) == SessionState::Running &&
        TerminalPanelTestAccess::sessionGeneration(*panel) == hostGeneration &&
        TerminalPanelTestAccess::processId(*panel) == hostProcess,
        "canceled host shutdown did not preserve the usable session");
    sendCwdQuery(*panel, narrow(fixture.rootB), 5);

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
