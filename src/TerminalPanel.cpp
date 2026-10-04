#include "TerminalPanel.h"

#include "Notepad_plus_msgs.h"
#include "Protocol.h"
#include "SettingsDialog.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace nppterminal {

namespace {

using json = nlohmann::json;

constexpr int kToolbarHeight = 32;
constexpr int kStatusHeight = 22;
constexpr DWORD kHostShutdownWaitMs = 2000;

bool getGeneration(const json& value, std::uint64_t& generation)
{
    if (!value.contains("generation") || !value["generation"].is_number_unsigned()) return false;
    generation = value["generation"].get<std::uint64_t>();
    return generation != 0;
}

bool getUnsigned(const json& value, const char* name, std::uint32_t& result)
{
    if (!value.contains(name) || !value[name].is_number_unsigned()) return false;
    const auto parsed = value[name].get<std::uint64_t>();
    if (parsed > 0xffffffffu) return false;
    result = static_cast<std::uint32_t>(parsed);
    return true;
}

struct DarkModeColors final {
    COLORREF background = 0;
    COLORREF softerBackground = 0;
    COLORREF hotBackground = 0;
    COLORREF pureBackground = 0;
    COLORREF errorBackground = 0;
    COLORREF text = 0;
    COLORREF darkerText = 0;
    COLORREF disabledText = 0;
    COLORREF linkText = 0;
    COLORREF edge = 0;
    COLORREF hotEdge = 0;
    COLORREF disabledEdge = 0;
};

std::wstring runningStatus(const std::wstring& shellName, std::uint16_t columns,
    std::uint16_t rows)
{
    return shellName + L" is running \u2014 " + std::to_wstring(columns) + L" \u00d7 " +
        std::to_wstring(rows);
}

std::wstring shellDisplayName(const std::vector<ShellInfo>& catalog,
    const std::wstring& id)
{
    for (const ShellInfo& shell : catalog) {
        if (shell.id == id) return shell.displayName;
    }
    if (id == L"powershell7") return L"PowerShell 7";
    if (id == L"gitbash") return L"Git Bash";
    if (id == L"wsl") return L"WSL";
    return L"Command Prompt";
}

std::wstring parentDirectory(const std::wstring& path)
{
    if (path.empty()) return {};
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

std::wstring queryNotepadPath(HWND nppWindow, UINT message)
{
    if (!nppWindow) return {};
    constexpr std::size_t kInitialCapacity = 512;
    std::vector<wchar_t> buffer(kInitialCapacity, L'\0');
    for (int attempt = 0; attempt != 3; ++attempt) {
        const LRESULT result = ::SendMessageW(nppWindow, message,
            static_cast<WPARAM>(buffer.size()), reinterpret_cast<LPARAM>(buffer.data()));
        if (result == FALSE) return {};
        const std::size_t length = wcsnlen(buffer.data(), buffer.size());
        if (length + 1 < buffer.size()) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2, L'\0');
    }
    return {};
}

bool copyClipboardText(HWND owner, const std::wstring& text)
{
    const std::size_t bytes = text.size() * sizeof(wchar_t);
    if (bytes > kMaxInputBytes || !::OpenClipboard(owner)) return false;
    const bool emptied = ::EmptyClipboard() != FALSE;
    HGLOBAL memory = nullptr;
    if (emptied) {
        memory = ::GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    }
    bool success = memory != nullptr;
    if (success) {
        void* target = ::GlobalLock(memory);
        success = target != nullptr;
        if (success) {
            std::memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            ::GlobalUnlock(memory);
            success = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
    }
    if (!success && memory) ::GlobalFree(memory);
    ::CloseClipboard();
    return success;
}

bool readClipboardText(std::wstring& text)
{
    text.clear();
    if (!::OpenClipboard(nullptr)) return false;
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        ::CloseClipboard();
        return false;
    }
    const wchar_t* source = static_cast<const wchar_t*>(::GlobalLock(data));
    if (!source) {
        ::CloseClipboard();
        return false;
    }
    const SIZE_T allocationBytes = ::GlobalSize(data);
    const std::size_t allocationChars = allocationBytes / sizeof(wchar_t);
    const std::size_t maxChars = std::min<std::size_t>(allocationChars,
        kMaxInputBytes / sizeof(wchar_t) + 1);
    const std::size_t length = wcsnlen(source, maxChars);
    const bool bounded = allocationChars != 0 && length < maxChars &&
        length <= kMaxInputBytes / sizeof(wchar_t);
    if (bounded) text.assign(source, length);
    ::GlobalUnlock(data);
    ::CloseClipboard();
    return bounded;
}

} // namespace

TerminalPanel::TerminalPanel(HMODULE module, HWND nppWindow, int dockingId)
    : DockingDlgInterface(IDD_TERMINAL_PANEL)
    , module_(module)
    , nppWindow_(nppWindow)
    , dockingId_(dockingId)
    , session_(SessionCallbacks{
        [this](std::uint64_t generation, std::uint64_t id,
            const std::vector<std::uint8_t>& bytes, const std::atomic_bool& cancelled) {
            return onSessionOutput(generation, id, bytes, cancelled);
        },
        [this](std::uint64_t generation, SessionState state, const std::wstring& message) {
            postSessionState(generation, state, message);
        },
        [this](std::uint64_t generation, std::uint64_t id) {
            postInputAck(generation, id);
        }})
{
}

TerminalPanel::~TerminalPanel()
{
    destroy();
}

bool TerminalPanel::createDock()
{
    if (isCreated()) return true;
    if (!nppWindow_ || !module_) return false;
    loadSettings();
    DockingDlgInterface::init(module_, nppWindow_);
    tTbData data{};
    DockingDlgInterface::create(&data);
    if (!isCreated()) return false;
    data.dlgID = dockingId_;
    data.uMask = DWS_DF_CONT_BOTTOM;
    data.pszModuleName = L"NppTerminal.dll";
    ::SendMessageW(nppWindow_, NPPM_DMMREGASDCKDLG, 0,
        reinterpret_cast<LPARAM>(&data));
    dispatchWindow_.store(_hSelf);
    bridge_.attachDispatcher(_hSelf, kOutputMessage);
    return true;
}

bool TerminalPanel::isVisible() const
{
    return isCreated() && ::IsWindowVisible(_hSelf) != FALSE;
}

void TerminalPanel::toggleByUser()
{
    userOpened_ = true;
    if (!createDock()) {
        setStatus(L"Unable to create the NppTerminal dock panel.", true);
        return;
    }
    const bool show = !isVisible();
    display(show);
    if (show) {
        if (!session_.hasProcess() && !discoveryPending_ && !restartPending_) {
            captureDirectorySnapshot({});
        }
        if (!ensureWebView()) return;
        if (pageReady_ && visibleState_ != SessionState::Running &&
            pageGeneration_ == session_.nextGeneration()) {
            startSession(pageGeneration_, pageColumns_, pageRows_);
        } else if (!pageReady_ && webView_.isReady()) {
            sendInit();
        }
    }
}

bool TerminalPanel::shutdownForHost()
{
    hostShutdownPending_ = false;
    shuttingDown_.store(true);
    dispatchWindow_.store(nullptr);
    discovery_.cancel();
    discoveryPending_ = false;
    pendingDirectoryCandidates_.reset();
    pendingDirectoryKey_.clear();
    pendingOpenHere_ = false;
    openHereDirectory_.clear();
    stopSessionPump();
    clearPendingExit();
    bridge_.cancel();
    session_.requestStop(StopReason::HostShutdown);
    const bool stopped = session_.waitForStop(kHostShutdownWaitMs);
    webView_.close();
    return stopped;
}

void TerminalPanel::prepareForHostShutdown()
{
    if (shuttingDown_.load()) return;
    // BEFORESHUTDOWN is cancellable.  Keep the dock, WebView, and session
    // usable until Notepad++ sends the final SHUTDOWN notification.
    hostShutdownPending_ = true;
}

void TerminalPanel::cancelHostShutdown()
{
    if (shuttingDown_.load()) return;
    hostShutdownPending_ = false;
    if (session_.hasPendingWork() || discoveryPending_) startSessionPump();
    updateButtons();
}

void TerminalPanel::onHostThemeChanged()
{
    if (shuttingDown_.load()) return;
    if (_hSelf) {
        ::SendMessageW(nppWindow_, NPPM_DARKMODESUBCLASSANDTHEME,
            NppDarkMode::dmfHandleChange, reinterpret_cast<LPARAM>(_hSelf));
        ::InvalidateRect(_hSelf, nullptr, TRUE);
    }
    sendTheme();
}

void TerminalPanel::destroy()
{
    if (shuttingDown_.load() && !isCreated()) return;
    shuttingDown_.store(true);
    dispatchWindow_.store(nullptr);
    discovery_.cancel();
    discoveryPending_ = false;
    stopSessionPump();
    clearPendingExit();
    bridge_.cancel();
    session_.stop(StopReason::PanelDestroy);
    webView_.close();

    if (_hSelf) {
        MSG pending{};
        while (::PeekMessageW(&pending, _hSelf, kSessionStateMessage,
            kSessionStateMessage, PM_REMOVE)) {
            delete reinterpret_cast<StateEvent*>(pending.lParam);
        }
        while (::PeekMessageW(&pending, _hSelf, kInputAckMessage,
            kInputAckMessage, PM_REMOVE)) {
            delete reinterpret_cast<AckEvent*>(pending.lParam);
        }
        ::SetWindowLongPtrW(_hSelf, GWLP_USERDATA, 0);
        StaticDialog::destroy();
        _hSelf = nullptr;
    }
    if (ownsControlFont_ && controlFont_) ::DeleteObject(controlFont_);
    controlFont_ = nullptr;
    controlDpi_ = 0;
    ownsControlFont_ = false;
    if (comOwned_) {
        ::CoUninitialize();
        comOwned_ = false;
    }
}

bool TerminalPanel::ensureWebView()
{
    if (webView_.isReady()) return true;
    if (!isCreated()) return false;
    if (!comOwned_) {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (result == S_OK || result == S_FALSE) {
            comOwned_ = true;
        } else if (result == RPC_E_CHANGED_MODE) {
            onWebError(L"WebView2 requires the Notepad++ UI thread to use a single-threaded COM apartment.");
            return false;
        } else if (FAILED(result)) {
            onWebError(L"COM initialization failed for WebView2.");
            return false;
        }
    }
    webViewFailed_ = false;
    const bool created = webView_.create(_hSelf, module_,
        [this](const std::wstring& message) { onWebMessage(message); },
        [this] { onWebNavigationReady(); },
        [this](const std::wstring& message) { onWebError(message); });
    if (!created) {
        webViewFailed_ = true;
        updateButtons();
    }
    return created;
}

void TerminalPanel::onWebNavigationReady()
{
    if (shuttingDown_.load()) return;
    sendInit();
}

void TerminalPanel::onWebMessage(const std::wstring& message)
{
    if (shuttingDown_.load()) return;
    constexpr std::size_t kMaxWebMessageBytes = 512u * 1024u;
    // Reject oversized renderer input before UTF-8 conversion or JSON parsing.
    // WebViewHost applies the same limit at the COM boundary; keep this guard
    // at the panel boundary so direct/test callbacks cannot bypass it.
    if (message.size() > kMaxWebMessageBytes) return;
    const std::string jsonText = wideToUtf8(message);
    if (jsonText.empty() || jsonText.size() > kMaxWebMessageBytes) return;
    handleProtocol(jsonText);
}

void TerminalPanel::onWebError(const std::wstring& message)
{
    if (shuttingDown_.load()) return;
    webViewFailed_ = true;
    discovery_.cancel();
    discoveryPending_ = false;
    pendingDirectoryCandidates_.reset();
    pendingDirectoryKey_.clear();
    pendingOpenHere_ = false;
    openHereDirectory_.clear();
    clearPendingExit();
    bridge_.cancel();
    if (visibleState_ == SessionState::Running || visibleState_ == SessionState::Starting ||
        session_.hasProcess()) {
        session_.requestStop(StopReason::PanelDestroy);
    }
    setStatus(message, true);
    webView_.close();
    updateButtons();
}

std::wstring TerminalPanel::jsonWide(const nlohmann::json& value)
{
    return utf8ToWide(value.dump());
}

void TerminalPanel::sendInit()
{
    if (!webView_.isReady() || shuttingDown_.load()) return;
    discovery_.cancel();
    discoveryPending_ = false;
    clearPendingExit();
    pageReady_ = false;
    pageGeneration_ = session_.nextGeneration();
    bridge_.beginGeneration(pageGeneration_);
    if (!settingsLoaded_) loadSettings();
    DarkModeColors colors{};
    const bool dark = ::SendMessageW(nppWindow_, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;
    const bool haveColors = ::SendMessageW(nppWindow_, NPPM_GETDARKMODECOLORS,
        sizeof(colors), reinterpret_cast<LPARAM>(&colors)) != FALSE;
    const nlohmann::json theme = dark && haveColors ? nlohmann::json{
        {"background", colorCss(colors.pureBackground)},
        {"foreground", colorCss(colors.text)},
        {"cursor", colorCss(colors.text)},
        {"cursorAccent", colorCss(colors.pureBackground)},
        {"selectionBackground", colorCss(colors.hotBackground)}} : nlohmann::json{
        {"background", dark ? "#1e1e1e" : "#ffffff"}, {"foreground", dark ? "#d4d4d4" : "#202020"},
        {"cursor", dark ? "#d4d4d4" : "#202020"}, {"cursorAccent", dark ? "#1e1e1e" : "#ffffff"},
        {"selectionBackground", dark ? "#264f78" : "#add6ff"}};
    const nlohmann::json message = {
        {"type", "init"}, {"generation", pageGeneration_},
        {"settings", { {"fontFamily", wideToUtf8(settings_.fontFamily)},
            {"fontSize", settings_.fontSize}, {"scrollback", settings_.scrollback}}},
        {"theme", theme}};
    if (!webView_.postJson(jsonWide(message))) {
        onWebError(L"The terminal page did not accept the initialization message.");
        return;
    }
    setStatus(L"Terminal ready; press Start to launch the selected shell.");
    updateButtons();
}

void TerminalPanel::sendTheme()
{
    if (!webView_.isReady() || pageGeneration_ == 0) return;
    DarkModeColors colors{};
    const bool dark = ::SendMessageW(nppWindow_, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;
    const bool haveColors = ::SendMessageW(nppWindow_, NPPM_GETDARKMODECOLORS,
        sizeof(colors), reinterpret_cast<LPARAM>(&colors)) != FALSE;
    const nlohmann::json theme = dark && haveColors ? nlohmann::json{
        {"background", colorCss(colors.pureBackground)},
        {"foreground", colorCss(colors.text)},
        {"cursor", colorCss(colors.text)},
        {"cursorAccent", colorCss(colors.pureBackground)},
        {"selectionBackground", colorCss(colors.hotBackground)}} : nlohmann::json{
        {"background", dark ? "#1e1e1e" : "#ffffff"}, {"foreground", dark ? "#d4d4d4" : "#202020"},
        {"cursor", dark ? "#d4d4d4" : "#202020"}, {"cursorAccent", dark ? "#1e1e1e" : "#ffffff"},
        {"selectionBackground", dark ? "#264f78" : "#add6ff"}};
    const nlohmann::json message = {
        {"type", "theme"}, {"generation", pageGeneration_}, {"theme", theme}};
    webView_.postJson(jsonWide(message));
}

void TerminalPanel::sendSettings()
{
    if (!webView_.isReady() || pageGeneration_ == 0) return;
    const nlohmann::json message = {
        {"type", "settings"}, {"generation", pageGeneration_},
        {"settings", {{"fontFamily", wideToUtf8(settings_.fontFamily)},
            {"fontSize", settings_.fontSize}, {"scrollback", settings_.scrollback}}}};
    webView_.postJson(jsonWide(message));
}

void TerminalPanel::sendState(std::uint64_t generation, SessionState state,
    const std::wstring& message)
{
    if (!webView_.isReady() || generation == 0 || generation != pageGeneration_) return;
    const nlohmann::json value = {
        {"type", "state"},
        {"generation", generation},
        {"state", stateName(static_cast<int>(state))},
        {"message", wideToUtf8(message)}};
    if (!webView_.postJson(jsonWide(value))) return;
}

void TerminalPanel::sendClear()
{
    if (!webView_.isReady() || pageGeneration_ == 0) return;
    const nlohmann::json value = {{"type", "clear"}, {"generation", pageGeneration_}};
    webView_.postJson(jsonWide(value));
}

bool TerminalPanel::onSessionOutput(std::uint64_t generation, std::uint64_t id,
    const std::vector<std::uint8_t>& data, const std::atomic_bool& cancelled)
{
    (void)cancelled;
#ifdef NPPTERMINAL_TESTS
    constexpr std::size_t kTestOutputLimit = 1024u * 1024u;
    if (testOutput_.size() < kTestOutputLimit) {
        const std::size_t count = std::min(kTestOutputLimit - testOutput_.size(), data.size());
        testOutput_.append(reinterpret_cast<const char*>(data.data()), count);
    }
#endif
    if (data.empty() || data.size() > kMaxOutputChunkBytes) return false;
    return bridge_.tryPush(OutputChunk{generation, id, data});
}

void TerminalPanel::pumpSession()
{
    if (shuttingDown_.load()) return;
    if (discoveryPending_) {
        discovery_.poll();
        if (discovery_.finished()) finishDiscovery();
    }
    session_.poll();
    pumpOutput();

    if (!session_.hasPendingWork() && !pendingExit_ && !discoveryPending_) {
        stopSessionPump();
    }
}

void TerminalPanel::pumpOutput()
{
    for (;;) {
        std::optional<OutputChunk> chunk = bridge_.takeForSend();
        if (!chunk) break;
        const nlohmann::json value = {
            {"type", "output"},
            {"generation", chunk->generation},
            {"id", chunk->id},
            {"data", base64Encode(chunk->data)}};
        if (!webView_.postJson(jsonWide(value))) {
            bridge_.returnUnsent(std::move(*chunk));
            // A failed post means the renderer/controller is no longer a
            // usable sink. Stop the session and offer native Retry instead of
            // repeatedly reposting the same chunk forever.
            onWebError(L"The terminal renderer stopped accepting output. Retry the terminal.");
            break;
        }
    }
    bridge_.onDispatchHandled();
    maybeReleasePendingExit();
}

void TerminalPanel::maybeReleasePendingExit()
{
    if (!pendingExit_) return;
    if (bridge_.queuedBytes() != 0 || bridge_.inFlightBytes() != 0) {
        if (::GetTickCount64() >= pendingExitDeadline_) {
            const std::uint64_t generation = pendingExit_->generation;
            clearPendingExit();
            webViewFailed_ = true;
            visibleState_ = SessionState::Error;
            const std::wstring message =
                L"The renderer did not acknowledge the final output. Retry the terminal.";
            setStatus(message, true);
            sendState(generation, SessionState::Error, message);
            updateButtons();
            if (!session_.hasPendingWork()) stopSessionPump();
        }
        return;
    }
    StateEvent event = std::move(*pendingExit_);
    pendingExit_.reset();
    pendingExitDeadline_ = 0;
    applyStateEvent(std::move(event));
    if (!pendingExit_ && !session_.hasPendingWork()) stopSessionPump();
}

void TerminalPanel::applyStateEvent(StateEvent event)
{
    if (shuttingDown_.load() || event.generation != pageGeneration_) return;
    visibleState_ = event.state;
    setStatus(event.state == SessionState::Running ?
            runningStatus(shellDisplayName(shellCatalog_, runningShellId_), pageColumns_, pageRows_) : event.message,
        event.state == SessionState::Error);
    sendState(event.generation, event.state, event.message);
    if (restartPending_ && (event.state == SessionState::NoSession ||
        event.state == SessionState::Exited || event.state == SessionState::Error)) {
        restartPending_ = false;
        if (ensureWebView()) sendInit();
    }
    if (releaseWebViewAfterStop_ && event.state == SessionState::NoSession) {
        releaseWebViewAfterStop_ = false;
        pageReady_ = false;
        webView_.close();
    }
    updateButtons();
}

void TerminalPanel::clearPendingExit()
{
    pendingExit_.reset();
    pendingExitDeadline_ = 0;
}

void TerminalPanel::startSession(std::uint64_t generation, std::uint16_t columns,
    std::uint16_t rows, const std::wstring& explicitDirectory)
{
    if (!userOpened_ || shuttingDown_.load() || generation == 0 || generation != pageGeneration_ ||
        !pageReady_ || visibleState_ == SessionState::Running ||
        visibleState_ == SessionState::Starting || visibleState_ == SessionState::Stopping ||
        session_.hasProcess() || discoveryPending_) return;
    if (!beginDiscovery(generation, columns, rows, explicitDirectory, false)) return;
}

bool TerminalPanel::beginDiscovery(std::uint64_t generation, std::uint16_t columns,
    std::uint16_t rows, const std::wstring& explicitDirectory, bool refreshCatalog)
{
    if (generation == 0 || generation != pageGeneration_ || discoveryPending_) return false;
    discoveryOnly_ = refreshCatalog;
    DirectoryCandidates candidates;
    if (!refreshCatalog && pendingDirectoryCandidates_ && pendingDirectoryKey_ == explicitDirectory) {
        candidates = std::move(*pendingDirectoryCandidates_);
        pendingDirectoryCandidates_.reset();
        pendingDirectoryKey_.clear();
    } else if (!snapshotDirectoryCandidates(explicitDirectory, candidates)) {
        setStatus(L"Unable to capture the current Notepad++ file location.", true);
        updateButtons();
        return false;
    }
    ShellDiscoveryRequest request;
    request.refreshCatalog = refreshCatalog || shellCatalog_.empty();
    request.cachedCatalog = shellCatalog_;
    request.requestedShellId = selectedShellId_.empty() ? settings_.defaultShell : selectedShellId_;
    request.explicitShell = selectedShellExplicit_;
    request.candidates = std::move(candidates);
    std::wstring error;
    if (!discovery_.start(request, error)) {
        setStatus(error.empty() ? L"Shell discovery could not start." : error, true);
        updateButtons();
        return false;
    }
    discoveryPending_ = true;
    discoveryRefresh_ = request.refreshCatalog;
    discoveryGeneration_ = generation;
    discoveryColumns_ = columns;
    discoveryRows_ = rows;
    discoveryExplicitDirectory_ = explicitDirectory;
    pageColumns_ = columns;
    pageRows_ = rows;
    if (!discoveryOnly_) visibleState_ = SessionState::Starting;
    setStatus(L"Detecting installed shells and resolving the working directory...");
    startSessionPump();
    updateButtons();
    return true;
}

void TerminalPanel::finishDiscovery()
{
    if (!discoveryPending_) return;
    const std::uint64_t generation = discoveryGeneration_;
    const bool discoveryOnly = discoveryOnly_;
    const ShellDiscoveryResult result = discovery_.result();
    discoveryPending_ = false;
    discoveryOnly_ = false;
    discoveryGeneration_ = 0;
    discoveryExplicitDirectory_.clear();
    if (shuttingDown_.load() || generation != pageGeneration_) return;
    if (!discoveryOnly && result.error.empty()) {
        for (const ShellInfo& shell : result.catalog) {
            if (shell.applicationName == result.applicationName) {
                selectedShellId_ = shell.id;
                runningShellId_ = shell.id;
                break;
            }
        }
    }
    if (!result.catalog.empty()) {
        shellCatalog_ = result.catalog;
        populateShellSelector();
    }
    if (discoveryOnly) {
        if (result.error.empty()) setStatus(L"Shell catalog refreshed.");
        else setStatus(result.error, true);
        updateButtons();
        return;
    }
    if (!result.error.empty() || result.applicationName.empty() ||
        result.commandLine.empty() || result.workingDirectory.empty()) {
        visibleState_ = SessionState::Error;
        const std::wstring error = result.error.empty() ?
            L"No usable shell or working directory was found." : result.error;
        setStatus(error, true);
        sendState(generation, SessionState::Error, error);
        updateButtons();
        return;
    }
    SessionStartOptions options;
    options.applicationName = result.applicationName;
    options.commandLine = result.commandLine;
    options.workingDirectory = result.workingDirectory;
    options.columns = discoveryColumns_;
    options.rows = discoveryRows_;
    options.expectedGeneration = generation;
    options.gitBashPreserveDirectory = result.gitBashPreserveDirectory;
    std::wstring error;
    if (!session_.start(options, error)) {
        visibleState_ = SessionState::Error;
        setStatus(error, true);
        sendState(generation, SessionState::Error, error);
        updateButtons();
        return;
    }
    setStatus(L"Starting " + shellDisplayName(shellCatalog_, selectedShellId_) + L"...");
    updateButtons();
}

bool TerminalPanel::snapshotDirectoryCandidates(const std::wstring& explicitDirectory,
    DirectoryCandidates& candidates) const
{
    candidates.explicitDirectory = explicitDirectory;
    const std::wstring filePath = activeFilePath();
    if (!filePath.empty()) {
        candidates.activeFileDirectory = parentDirectory(filePath);
    }
    candidates.defaultDirectory = settings_.defaultDirectory;
    wchar_t current[MAX_PATH * 4] = {};
    const DWORD currentLength = ::GetCurrentDirectoryW(static_cast<DWORD>(std::size(current)), current);
    if (currentLength != 0 && currentLength < std::size(current)) {
        candidates.workingDirectory.assign(current, currentLength);
    }
    PWSTR home = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DONT_VERIFY, nullptr, &home))) {
        candidates.homeDirectory.assign(home);
        ::CoTaskMemFree(home);
    }
    // Capture strings only. The contained discovery helper validates every
    // candidate, including UNC paths, without blocking the host UI on I/O.
    return true;
}

void TerminalPanel::captureDirectorySnapshot(const std::wstring& explicitDirectory)
{
    DirectoryCandidates candidates;
    if (snapshotDirectoryCandidates(explicitDirectory, candidates)) {
        pendingDirectoryCandidates_ = std::move(candidates);
        pendingDirectoryKey_ = explicitDirectory;
    } else {
        pendingDirectoryCandidates_.reset();
        pendingDirectoryKey_.clear();
    }
}

std::wstring TerminalPanel::activeFilePath() const
{
    int currentView = 0;
    (void)::SendMessageW(nppWindow_, NPPM_GETCURRENTSCINTILLA, 0,
        reinterpret_cast<LPARAM>(&currentView));
    return queryNotepadPath(nppWindow_, NPPM_GETFULLCURRENTPATH);
}

std::wstring TerminalPanel::configFilePath() const
{
    if (!nppWindow_) return {};
    constexpr std::size_t kCapacity = 512;
    std::vector<wchar_t> buffer(kCapacity, L'\0');
    const LRESULT required = ::SendMessageW(nppWindow_, NPPM_GETPLUGINSCONFIGDIR, 0, 0);
    if (required <= 0 || static_cast<std::size_t>(required) >= 32768) return {};
    buffer.resize(static_cast<std::size_t>(required) + 1, L'\0');
    const LRESULT copied = ::SendMessageW(nppWindow_, NPPM_GETPLUGINSCONFIGDIR,
        static_cast<WPARAM>(buffer.size()), reinterpret_cast<LPARAM>(buffer.data()));
    if (copied == FALSE) return {};
    const std::size_t length = wcsnlen(buffer.data(), buffer.size());
    if (length == 0 || length >= buffer.size()) return {};
    return (std::filesystem::path(buffer.data()) / L"NppTerminal.json").wstring();
}

void TerminalPanel::loadSettings()
{
    if (settingsLoaded_) return;
    settingsLoaded_ = true;
    configPath_ = configFilePath();
    if (configPath_.empty()) {
        settings_ = settings::Settings{};
        return;
    }
    const settings::SettingsLoadResult loaded = settings::loadSettings(configPath_);
    settings_ = loaded.value;
    selectedShellId_ = settings_.defaultShell;
    if (loaded.malformed && !loaded.error.empty()) {
        setStatus(L"Settings were invalid; safe defaults are in use until you save them.", true);
    }
}

void TerminalPanel::populateShellSelector()
{
    if (!shellSelector_) return;
    shellIds_.clear();
    ::SendMessageW(shellSelector_, CB_RESETCONTENT, 0, 0);
    for (const ShellInfo& shell : shellCatalog_) {
        const int index = static_cast<int>(::SendMessageW(shellSelector_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(shell.displayName.c_str())));
        if (index >= 0) shellIds_.push_back(shell.id);
    }
    int selected = -1;
    for (std::size_t index = 0; index < shellIds_.size(); ++index) {
        if (shellIds_[index] == selectedShellId_) {
            selected = static_cast<int>(index);
            break;
        }
    }
    if (selected < 0 && !selectedShellExplicit_ && !shellIds_.empty()) {
        selected = 0;
        selectedShellId_ = shellIds_[0];
    }
    if (selected >= 0) ::SendMessageW(shellSelector_, CB_SETCURSEL, selected, 0);
}

void TerminalPanel::refreshShellCatalog()
{
    if (discoveryPending_ || !pageReady_ || pageGeneration_ == 0) return;
    if (beginDiscovery(pageGeneration_, pageColumns_, pageRows_, {}, true)) {
        setStatus(L"Refreshing the installed shell catalog...");
    }
}

void TerminalPanel::onShellSelectionChanged()
{
    if (!shellSelector_) return;
    const LRESULT index = ::SendMessageW(shellSelector_, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= shellIds_.size()) return;
    const std::wstring chosen = shellIds_[static_cast<std::size_t>(index)];
    if (chosen == selectedShellId_) return;
    if ((visibleState_ == SessionState::Running || visibleState_ == SessionState::Starting ||
        session_.hasProcess()) && !confirmDestructiveAction(L"switch shells")) {
        populateShellSelector();
        return;
    }
    switchShell(chosen);
}

void TerminalPanel::switchShell(const std::wstring& shellId)
{
    selectedShellId_ = shellId;
    selectedShellExplicit_ = true;
    pendingOpenHere_ = false;
    openHereDirectory_.clear();
    releaseWebViewAfterStop_ = false;
    if (visibleState_ == SessionState::Running || visibleState_ == SessionState::Starting ||
        session_.hasProcess()) {
        captureDirectorySnapshot({});
        clearPendingExit();
        bridge_.cancel();
        restartPending_ = true;
        session_.requestStop(StopReason::Restart);
        startSessionPump();
    } else if (pageReady_) {
        startSession(pageGeneration_, pageColumns_, pageRows_);
    }
    updateButtons();
}

bool TerminalPanel::confirmDestructiveAction(const wchar_t* action) const
{
#ifdef NPPTERMINAL_TESTS
    if (testConfirmation_.has_value()) return *testConfirmation_;
#endif
    if (!settings_.confirmBeforeKill || shuttingDown_.load()) return true;
    const std::wstring message = L"Stop the current terminal to " + std::wstring(action) + L"?";
    return ::MessageBoxW(_hSelf, message.c_str(), L"NppTerminal", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

std::wstring TerminalPanel::colorCss(COLORREF color)
{
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"#%02x%02x%02x", static_cast<unsigned>(GetRValue(color)),
        static_cast<unsigned>(GetGValue(color)), static_cast<unsigned>(GetBValue(color)));
    return buffer;
}

void TerminalPanel::showSettings()
{
    if (shuttingDown_.load() || !createDock()) return;
    loadSettings();
    settings::Settings updated;
    const bool accepted = settings::showSettingsDialog(module_, _hSelf, settings_, updated,
        [this](const settings::Settings& candidate, std::wstring& error) {
            if (configPath_.empty()) {
                error = L"Notepad++ did not provide a plugin configuration directory.";
                return false;
            }
            return settings::saveSettings(configPath_, candidate, error);
        });
    if (!accepted) return;
    settings_ = std::move(updated);
    if (!selectedShellExplicit_) selectedShellId_ = settings_.defaultShell;
    populateShellSelector();
    sendSettings();
    updateButtons();
}

void TerminalPanel::openTerminalHere()
{
    userOpened_ = true;
    if (!createDock()) return;
    const std::wstring currentFile = activeFilePath();
    const std::wstring directory = parentDirectory(currentFile);
    if (directory.empty() || !isAbsoluteWindowsPath(directory)) {
        setStatus(L"Open Terminal Here requires a saved file.", true);
        display(true);
        return;
    }
    const bool replacing = discoveryPending_ || session_.hasProcess() ||
        visibleState_ == SessionState::Running || visibleState_ == SessionState::Starting ||
        visibleState_ == SessionState::Stopping;
    if (replacing && !confirmDestructiveAction(L"open a terminal here")) return;
    captureDirectorySnapshot(directory);
    display(true);
    pendingOpenHere_ = true;
    openHereDirectory_ = directory;
    if (replacing) {
        discovery_.cancel();
        discoveryPending_ = false;
        clearPendingExit();
        bridge_.cancel();
        releaseWebViewAfterStop_ = false;
        if (session_.hasProcess() || session_.hasPendingWork()) {
            restartPending_ = true;
            session_.requestStop(StopReason::Restart);
            startSessionPump();
        } else {
            visibleState_ = SessionState::NoSession;
            if (ensureWebView()) sendInit();
        }
        return;
    }
    if (!ensureWebView()) return;
    if (pageReady_ && pageGeneration_ == session_.nextGeneration()) {
        pendingOpenHere_ = false;
        openHereDirectory_.clear();
        startSession(pageGeneration_, pageColumns_, pageRows_, directory);
    } else if (!pageReady_ && webView_.isReady()) {
        sendInit();
    }
}
void TerminalPanel::restartSession()
{
    if (!userOpened_ || shuttingDown_.load()) return;
    if (!confirmDestructiveAction(L"restart the terminal")) return;
    pendingOpenHere_ = false;
    openHereDirectory_.clear();
    releaseWebViewAfterStop_ = false;
    captureDirectorySnapshot({});
    if (discoveryPending_) {
        discovery_.cancel();
        discoveryPending_ = false;
    }
    clearPendingExit();
    bridge_.cancel();
    restartPending_ = true;
    if (session_.state() == SessionState::Running || session_.state() == SessionState::Starting ||
        session_.hasProcess()) {
        session_.requestStop(StopReason::Restart);
    } else {
        restartPending_ = false;
        if (ensureWebView()) sendInit();
    }
}

void TerminalPanel::killSession(StopReason reason)
{
    if (reason != StopReason::HostShutdown && reason != StopReason::PanelDestroy &&
        !confirmDestructiveAction(L"stop the terminal")) return;
    if (discoveryPending_) {
        discovery_.cancel();
        discoveryPending_ = false;
    }
    clearPendingExit();
    bridge_.cancel();
    restartPending_ = false;
    // An accepted Kill abandons any serialized Open Here request and the
    // snapshot captured for it.  Leaving either value behind would let a
    // later page-ready/toggle consume the stale directory after the session
    // has already been explicitly stopped.
    pendingOpenHere_ = false;
    openHereDirectory_.clear();
    pendingDirectoryCandidates_.reset();
    pendingDirectoryKey_.clear();
    releaseWebViewAfterStop_ = reason == StopReason::UserKill;
    if (session_.state() == SessionState::Running || session_.state() == SessionState::Starting ||
        session_.hasProcess()) {
        session_.requestStop(reason);
    } else {
        bridge_.clear();
        stopSessionPump();
        visibleState_ = SessionState::NoSession;
        setStatus(L"Terminal stopped.");
        sendState(pageGeneration_, SessionState::NoSession, L"Terminal stopped.");
        if (releaseWebViewAfterStop_) {
            releaseWebViewAfterStop_ = false;
            pageReady_ = false;
            webView_.close();
        }
    }
    updateButtons();
}

void TerminalPanel::startSessionPump()
{
    if (_hSelf && !shuttingDown_.load()) {
        ::SetTimer(_hSelf, kSessionPumpTimer, kSessionPumpIntervalMs, nullptr);
    }
}

void TerminalPanel::stopSessionPump()
{
    if (_hSelf) ::KillTimer(_hSelf, kSessionPumpTimer);
}

void TerminalPanel::clearSessionView()
{
    sendClear();
}

void TerminalPanel::setStatus(const std::wstring& text, bool error)
{
    if (statusLabel_) {
        ::SetWindowTextW(statusLabel_, text.c_str());
        (void)error;
        ::InvalidateRect(statusLabel_, nullptr, TRUE);
    }
}

void TerminalPanel::updateControlFont()
{
    const UINT windowDpi = ::GetDpiForWindow(_hSelf);
    const UINT dpi = windowDpi != 0 ? windowDpi : USER_DEFAULT_SCREEN_DPI;
    if (controlFont_ && controlDpi_ == dpi) return;

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    HFONT font = nullptr;
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
            &metrics, 0, dpi)) {
        font = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    const HFONT previousFont = controlFont_;
    const bool previousOwned = ownsControlFont_;
    ownsControlFont_ = font != nullptr;
    controlFont_ = font ? font : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    controlDpi_ = dpi;
    for (HWND control : {shellSelector_, startButton_, restartButton_, clearButton_, killButton_,
            retryButton_, refreshButton_, settingsButton_, statusLabel_}) {
        if (control) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
    }
    if (previousOwned && previousFont) ::DeleteObject(previousFont);
}

void TerminalPanel::layoutControls()
{
    if (!isCreated()) return;
    updateControlFont();
    const auto scale = [this](int value) {
        return ::MulDiv(value, static_cast<int>(controlDpi_), USER_DEFAULT_SCREEN_DPI);
    };
    RECT client{};
    ::GetClientRect(_hSelf, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int toolbarHeight = scale(kToolbarHeight);
    const int statusHeight = scale(kStatusHeight);
    int x = scale(6);
    if (shellSelector_) ::MoveWindow(shellSelector_, x, scale(4), scale(130), scale(24), TRUE);
    x += scale(136);
    for (HWND button : {startButton_, restartButton_, clearButton_, killButton_, retryButton_,
        refreshButton_, settingsButton_}) {
        if (button) {
            ::MoveWindow(button, x, scale(4), scale(60), scale(24), TRUE);
            x += scale(64);
        }
    }
    if (statusLabel_) ::MoveWindow(statusLabel_, scale(6), std::max(toolbarHeight, height - statusHeight),
        std::max(0, width - scale(12)), statusHeight, TRUE);
    RECT webBounds{0, toolbarHeight, width, std::max(toolbarHeight, height - statusHeight)};
    webView_.resize(webBounds);
}

void TerminalPanel::updateButtons()
{
    const bool running = visibleState_ == SessionState::Running;
    const bool startDiscoveryPending = discoveryPending_ && !discoveryOnly_;
    const bool canStart = userOpened_ &&
        (visibleState_ == SessionState::NoSession || visibleState_ == SessionState::Exited ||
            visibleState_ == SessionState::Error) && !running && !webViewFailed_ &&
        !startDiscoveryPending && !session_.hasProcess();
    if (startButton_) ::EnableWindow(startButton_, canStart);
    if (restartButton_) ::EnableWindow(restartButton_, !startDiscoveryPending &&
        (running || visibleState_ == SessionState::Exited || pendingExit_.has_value()));
    if (clearButton_) ::EnableWindow(clearButton_, pageReady_);
    if (killButton_) ::EnableWindow(killButton_, !startDiscoveryPending &&
        (running || visibleState_ == SessionState::Starting || pendingExit_.has_value()));
    if (retryButton_) ::EnableWindow(retryButton_, webViewFailed_);
    if (refreshButton_) ::EnableWindow(refreshButton_, pageReady_ && !discoveryPending_);
    if (settingsButton_) ::EnableWindow(settingsButton_, !shuttingDown_.load());
    if (shellSelector_) ::EnableWindow(shellSelector_, !discoveryPending_ &&
        visibleState_ != SessionState::Starting && visibleState_ != SessionState::Stopping &&
        !shellIds_.empty());
}

void TerminalPanel::postSessionState(std::uint64_t generation, SessionState state,
    const std::wstring& message)
{
    const HWND window = dispatchWindow_.load();
    if (shuttingDown_.load() || !window) return;
    auto* event = new StateEvent{generation, state, message};
    if (!::PostMessageW(window, kSessionStateMessage, 0, reinterpret_cast<LPARAM>(event))) {
        delete event;
    }
}

void TerminalPanel::postInputAck(std::uint64_t generation, std::uint64_t id)
{
    const HWND window = dispatchWindow_.load();
    if (shuttingDown_.load() || !window) return;
    auto* event = new AckEvent{generation, id};
    if (!::PostMessageW(window, kInputAckMessage, 0, reinterpret_cast<LPARAM>(event))) {
        delete event;
    }
}

void TerminalPanel::handleProtocol(const std::string& jsonText)
{
    json value;
    try {
        value = json::parse(jsonText);
    } catch (...) {
        return;
    }
    if (!value.is_object() || !value.contains("type") || !value["type"].is_string()) return;
    const std::string type = value["type"].get<std::string>();
    std::uint64_t generation = 0;
    if (type != "ready" && type != "input" && type != "binary" && type != "resize" &&
        type != "ack" && type != "flow" && type != "clipboardCopy" &&
        type != "clipboardRead") return;
    if (!getGeneration(value, generation) || generation != pageGeneration_) return;

    if (type == "clipboardCopy") {
        if (!value.contains("data") || !value["data"].is_string()) return;
        const std::string data = value["data"].get<std::string>();
        if (data.empty() || data.size() > kMaxInputBytes) return;
        const std::wstring text = utf8ToWide(data);
        if (text.empty() || text.size() > kMaxInputBytes / sizeof(wchar_t)) return;
        if (!copyClipboardText(_hSelf, text)) setStatus(L"Unable to copy the terminal selection.", true);
        return;
    }

    if (type == "clipboardRead") {
        std::wstring text;
        if (!readClipboardText(text)) {
            setStatus(L"Unable to read Unicode text from the clipboard.", true);
            return;
        }
        const std::string data = wideToUtf8(text);
        if (data.empty() || data.size() > kMaxInputBytes) {
            setStatus(L"Clipboard text is too large (maximum 64 KiB).", true);
            return;
        }
        const nlohmann::json response = { {"type", "clipboard"},
            {"generation", pageGeneration_}, {"data", data} };
        webView_.postJson(jsonWide(response));
        return;
    }

    if (type == "ready") {
        std::uint32_t columns = 0;
        std::uint32_t rows = 0;
        if (!getUnsigned(value, "cols", columns) || !getUnsigned(value, "rows", rows) ||
            columns == 0 || columns > 500 || rows == 0 || rows > 200) return;
        pageColumns_ = static_cast<std::uint16_t>(columns);
        pageRows_ = static_cast<std::uint16_t>(rows);
        pageReady_ = true;
        setStatus(userOpened_ ? L"Starting the selected shell..." : L"Terminal ready; press Toggle Terminal to start.");
        if (userOpened_ && visibleState_ != SessionState::Running) {
            const std::wstring explicitDirectory = pendingOpenHere_ ? openHereDirectory_ : L"";
            pendingOpenHere_ = false;
            openHereDirectory_.clear();
            startSession(generation, pageColumns_, pageRows_, explicitDirectory);
        }
        updateButtons();
        return;
    }

    if (type == "ack") {
        std::uint32_t id = 0;
        if (getUnsigned(value, "id", id)) bridge_.acknowledge(generation, id);
        pumpOutput();
        return;
    }

    if (type == "resize") {
        std::uint32_t columns = 0;
        std::uint32_t rows = 0;
        if (!getUnsigned(value, "cols", columns) || !getUnsigned(value, "rows", rows) ||
            columns == 0 || columns > 500 || rows == 0 || rows > 200) return;
        pageColumns_ = static_cast<std::uint16_t>(columns);
        pageRows_ = static_cast<std::uint16_t>(rows);
        bool resized = true;
        if (visibleState_ == SessionState::Running) {
            resized = session_.resize(pageColumns_, pageRows_);
            if (!resized) {
                setStatus(L"The terminal resize was rejected by ConPTY.", true);
            }
        }
        if (resized && visibleState_ == SessionState::Running) {
            setStatus(runningStatus(shellDisplayName(shellCatalog_, runningShellId_),
                pageColumns_, pageRows_));
        }
        return;
    }

    if (type == "input" || type == "binary") {
        std::uint32_t id = 0;
        if (!getUnsigned(value, "id", id) || id == 0 || !value.contains("data") ||
            !value["data"].is_string()) return;
        const std::string data = value["data"].get<std::string>();
        std::vector<std::uint8_t> bytes;
        const bool decoded = type == "binary" ? utf8ByteStringToBytes(data, bytes) :
            (bytes.assign(data.begin(), data.end()), true);
        if (!decoded || bytes.empty() || bytes.size() > kMaxInputBytes) {
            setStatus(L"Input was rejected because it exceeds the terminal input limit.", true);
            return;
        }
        if (!session_.write(generation, id, bytes)) {
            setStatus(L"Input is temporarily paused while the shell catches up.", true);
        }
        return;
    }
}

INT_PTR CALLBACK TerminalPanel::run_dlgProc(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
        case WM_INITDIALOG:
            // A dock restored by Notepad++ is already an explicit terminal
            // surface. Keep the shell lazy, but let its Start button create
            // the renderer on demand without requiring a hide/show toggle.
            shellSelector_ = ::CreateWindowExW(0, WC_COMBOBOXW, L"Detecting shells...",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP, 0, 0, 130, 24, _hSelf,
                reinterpret_cast<HMENU>(IDC_TERMINAL_SHELL), module_, nullptr);
            startButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_START), module_, nullptr);
            restartButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Restart", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_RESTART), module_, nullptr);
            clearButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_CLEAR), module_, nullptr);
            killButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Kill", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_KILL), module_, nullptr);
            retryButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Retry", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_RETRY), module_, nullptr);
            refreshButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_REFRESH), module_, nullptr);
            settingsButton_ = ::CreateWindowExW(0, WC_BUTTONW, L"Settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 60, 24, _hSelf, reinterpret_cast<HMENU>(IDC_TERMINAL_SETTINGS), module_, nullptr);
            statusLabel_ = ::CreateWindowExW(0, WC_STATICW, L"Terminal is closed.",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 300, kStatusHeight, _hSelf,
                reinterpret_cast<HMENU>(IDC_TERMINAL_STATUS), module_, nullptr);
            updateControlFont();
            ::SendMessageW(nppWindow_, NPPM_DARKMODESUBCLASSANDTHEME,
                NppDarkMode::dmfInit, reinterpret_cast<LPARAM>(_hSelf));
            bridge_.attachDispatcher(_hSelf, kOutputMessage);
            layoutControls();
            updateButtons();
            return TRUE;

        case WM_SIZE:
        case WM_DPICHANGED:
        case WM_DPICHANGED_AFTERPARENT:
            layoutControls();
            return TRUE;

        case WM_SETFOCUS:
            if (webView_.isReady() && pageGeneration_ != 0) {
                const nlohmann::json value = { {"type", "focus"},
                    {"generation", pageGeneration_} };
                webView_.postJson(jsonWide(value));
            }
            return TRUE;

        case WM_TIMER:
            if (wParam == kSessionPumpTimer) {
                pumpSession();
                return TRUE;
            }
            break;

        case WM_COMMAND:
            if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_TERMINAL_SHELL) {
                onShellSelectionChanged();
                return TRUE;
            }
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case IDC_TERMINAL_START:
                        userOpened_ = true;
                        captureDirectorySnapshot({});
                        if (pageReady_ && pageGeneration_ == session_.nextGeneration()) {
                            startSession(pageGeneration_, pageColumns_, pageRows_);
                        } else if (ensureWebView()) {
                            sendInit();
                        }
                        return TRUE;
                    case IDC_TERMINAL_RESTART:
                        restartSession();
                        return TRUE;
                    case IDC_TERMINAL_CLEAR:
                        clearSessionView();
                        return TRUE;
                    case IDC_TERMINAL_KILL:
                        killSession();
                        return TRUE;
                    case IDC_TERMINAL_RETRY:
                        webViewFailed_ = false;
                        webView_.close();
                        ensureWebView();
                        return TRUE;
                    case IDC_TERMINAL_REFRESH:
                        refreshShellCatalog();
                        return TRUE;
                    case IDC_TERMINAL_SETTINGS:
                        showSettings();
                        return TRUE;
                    default:
                        break;
                }
            }
            break;

        case WM_NOTIFY:
        {
            auto* header = reinterpret_cast<LPNMHDR>(lParam);
            if (header && header->hwndFrom == _hParent && LOWORD(header->code) == DMN_CLOSE) {
                display(false);
                return TRUE;
            }
            break;
        }

        case kOutputMessage:
            pumpOutput();
            return TRUE;

        case kSessionStateMessage:
        {
            std::unique_ptr<StateEvent> event(reinterpret_cast<StateEvent*>(lParam));
            if (event && event->generation == pageGeneration_) {
                if (event->state == SessionState::Exited &&
                    (bridge_.queuedBytes() != 0 || bridge_.inFlightBytes() != 0)) {
                    pendingExit_ = std::move(*event);
                    pendingExitDeadline_ = ::GetTickCount64() + 5000;
                    startSessionPump();
                    visibleState_ = SessionState::Stopping;
                    setStatus(L"Finishing terminal output...");
                    sendState(pageGeneration_, SessionState::Stopping,
                        L"Finishing terminal output...");
                    updateButtons();
                } else {
                    if (event->state != SessionState::Exited) clearPendingExit();
                    applyStateEvent(std::move(*event));
                }
            }
            return TRUE;
        }

        case kInputAckMessage:
        {
            std::unique_ptr<AckEvent> event(reinterpret_cast<AckEvent*>(lParam));
            if (event && event->generation == pageGeneration_ && webView_.isReady()) {
                const nlohmann::json value = {{"type", "inputAck"},
                    {"generation", event->generation}, {"id", event->id}};
                webView_.postJson(jsonWide(value));
            }
            return TRUE;
        }

        default:
            break;
    }
    return DockingDlgInterface::run_dlgProc(message, wParam, lParam);
}

} // namespace nppterminal
