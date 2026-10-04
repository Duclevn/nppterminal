#pragma once

#include "Bridge.h"
#include "DirectoryResolver.h"
#include "Settings.h"
#include "ShellDiscovery.h"
#include "TerminalSession.h"
#include "WebViewHost.h"

#include "DockingFeature/DockingDlgInterface.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nppterminal {

#ifdef NPPTERMINAL_TESTS
class TerminalPanelTestAccess;
#endif

class TerminalPanel final : public DockingDlgInterface {
public:
    static constexpr UINT kOutputMessage = WM_APP + 0x321;
    static constexpr UINT kSessionStateMessage = WM_APP + 0x322;
    static constexpr UINT kInputAckMessage = WM_APP + 0x323;
    static constexpr UINT_PTR kSessionPumpTimer = 1;
    static constexpr UINT kSessionPumpIntervalMs = 33;

    TerminalPanel(HMODULE module, HWND nppWindow, int dockingId);
    ~TerminalPanel() override;

    TerminalPanel(const TerminalPanel&) = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    bool createDock();
    void toggleByUser();
    void showSettings();
    void openTerminalHere();
    void prepareForHostShutdown();
    void cancelHostShutdown();
    void onHostThemeChanged();
    bool shutdownForHost();
    void destroy() override;

    bool isVisible() const;

protected:
    INT_PTR CALLBACK run_dlgProc(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
#ifdef NPPTERMINAL_TESTS
    friend class TerminalPanelTestAccess;
#endif

    struct StateEvent {
        std::uint64_t generation = 0;
        SessionState state = SessionState::NoSession;
        std::wstring message;
    };
    struct AckEvent {
        std::uint64_t generation = 0;
        std::uint64_t id = 0;
    };

    bool ensureWebView();
    void onWebNavigationReady();
    void onWebMessage(const std::wstring& message);
    void onWebError(const std::wstring& message);
    void sendInit();
    void sendTheme();
    void sendSettings();
    void sendState(std::uint64_t generation, SessionState state, const std::wstring& message);
    void sendClear();
    void pumpSession();
    void pumpOutput();
    void maybeReleasePendingExit();
    void applyStateEvent(StateEvent event);
    void clearPendingExit();
    void startSessionPump();
    void stopSessionPump();
    void startSession(std::uint64_t generation, std::uint16_t columns, std::uint16_t rows,
        const std::wstring& explicitDirectory = {});
    bool beginDiscovery(std::uint64_t generation, std::uint16_t columns, std::uint16_t rows,
        const std::wstring& explicitDirectory = {}, bool refreshCatalog = false);
    void finishDiscovery();
    bool snapshotDirectoryCandidates(const std::wstring& explicitDirectory,
        DirectoryCandidates& candidates) const;
    void captureDirectorySnapshot(const std::wstring& explicitDirectory);
    std::wstring activeFilePath() const;
    std::wstring configFilePath() const;
    void loadSettings();
    void populateShellSelector();
    void refreshShellCatalog();
    void onShellSelectionChanged();
    void switchShell(const std::wstring& shellId);
    bool confirmDestructiveAction(const wchar_t* action) const;
    static std::wstring colorCss(COLORREF color);
    void restartSession();
    void killSession(StopReason reason = StopReason::UserKill);
    void clearSessionView();
    void setStatus(const std::wstring& text, bool error = false);
    void updateControlFont();
    void layoutControls();
    void updateButtons();
    void postSessionState(std::uint64_t generation, SessionState state,
        const std::wstring& message);
    void postInputAck(std::uint64_t generation, std::uint64_t id);
    bool onSessionOutput(std::uint64_t generation, std::uint64_t id,
        const std::vector<std::uint8_t>& data, const std::atomic_bool& cancelled);
    void handleProtocol(const std::string& jsonText);
    static std::wstring jsonWide(const nlohmann::json& value);

    HMODULE module_ = nullptr;
    HWND nppWindow_ = nullptr;
    const int dockingId_ = 0;
    HWND shellSelector_ = nullptr;
    HWND startButton_ = nullptr;
    HWND restartButton_ = nullptr;
    HWND clearButton_ = nullptr;
    HWND killButton_ = nullptr;
    HWND retryButton_ = nullptr;
    HWND refreshButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND statusLabel_ = nullptr;
    HFONT controlFont_ = nullptr;
    UINT controlDpi_ = 0;
    bool ownsControlFont_ = false;
    bool userOpened_ = false;
    std::atomic_bool shuttingDown_{false};
    std::atomic<HWND> dispatchWindow_{nullptr};
    bool comOwned_ = false;
    bool pageReady_ = false;
    bool webViewFailed_ = false;
    bool restartPending_ = false;
    bool releaseWebViewAfterStop_ = false;
    bool settingsLoaded_ = false;
    bool discoveryPending_ = false;
    bool discoveryRefresh_ = false;
    bool discoveryOnly_ = false;
    bool pendingOpenHere_ = false;
    bool hostShutdownPending_ = false;
    bool selectedShellExplicit_ = false;
    std::uint64_t discoveryGeneration_ = 0;
    std::uint16_t discoveryColumns_ = 80;
    std::uint16_t discoveryRows_ = 24;
    std::wstring discoveryExplicitDirectory_;
    std::wstring openHereDirectory_;
    std::wstring configPath_;
    std::wstring selectedShellId_;
    std::wstring runningShellId_;
    std::vector<std::wstring> shellIds_;
    std::vector<ShellInfo> shellCatalog_;
    settings::Settings settings_;
    ShellDiscoveryClient discovery_;
    std::optional<DirectoryCandidates> pendingDirectoryCandidates_;
    std::wstring pendingDirectoryKey_;
    std::uint64_t pageGeneration_ = 0;
    std::uint16_t pageColumns_ = 80;
    std::uint16_t pageRows_ = 24;
    SessionState visibleState_ = SessionState::NoSession;
    std::optional<StateEvent> pendingExit_;
    ULONGLONG pendingExitDeadline_ = 0;

#ifdef NPPTERMINAL_TESTS
    // Test-only control/observation points.  They are never compiled into the
    // plugin and are bounded so a noisy shell cannot grow the test process.
    mutable std::optional<bool> testConfirmation_;
    std::string testOutput_;
#endif

    BoundedOutputBridge bridge_;
    WebViewHost webView_;
    TerminalSession session_;
};

} // namespace nppterminal
