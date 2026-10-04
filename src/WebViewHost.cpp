#include "WebViewHost.h"

#include "ProfileCleanup.h"
#include "Protocol.h"

#include <wrl.h>
#include <WebView2.h>
#include <shlobj.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef NPPTERMINAL_TESTS
#include <iostream>
#endif

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace nppterminal {

namespace {

constexpr wchar_t kOrigin[] = L"https://nppterminal.invalid/";
constexpr wchar_t kIndex[] = L"https://nppterminal.invalid/index.html";
constexpr wchar_t kMissingSecurityCapability[] =
    L"The installed WebView2 Runtime is too old for NppTerminal's security policy. "
    L"Install or update the Evergreen WebView2 Runtime, then retry.";
constexpr wchar_t kCleanupObserverClass[] = L"NppTerminalCleanupObserver-v1";
constexpr UINT_PTR kCleanupObserverTimerId = 0x4e505443u;
constexpr UINT kCleanupObserverTimeoutMs = 30u * 1000u;

std::wstring win32Error(DWORD code)
{
    if (code == ERROR_SUCCESS) code = ::GetLastError();
    wchar_t buffer[512] = {};
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    if (length == 0) return L"Windows error " + std::to_wstring(code);
    std::wstring message(buffer, length);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

std::wstring moduleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    std::filesystem::path value(path);
    return value.parent_path().wstring();
}

std::wstring localAppData()
{
    PWSTR path = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) {
        std::wstring value(path);
        ::CoTaskMemFree(path);
        return value;
    }
    return {};
}

bool isAllowedUri(const std::wstring& uri)
{
    return uri.rfind(kOrigin, 0) == 0;
}

std::wstring pathForCompare(const std::filesystem::path& value)
{
    return value.lexically_normal().wstring();
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const std::wstring leftText = pathForCompare(left);
    const std::wstring rightText = pathForCompare(right);
    return !leftText.empty() && !_wcsicmp(leftText.c_str(), rightText.c_str());
}

bool profileBelongsToUserDataFolder(ICoreWebView2Profile* profile,
    const std::wstring& userDataFolder, const std::wstring& expectedProfileName)
{
    if (!profile || userDataFolder.empty() || expectedProfileName.empty()) return false;

    LPWSTR profileName = nullptr;
    if (FAILED(profile->get_ProfileName(&profileName)) || !profileName) {
        if (profileName) ::CoTaskMemFree(profileName);
        return false;
    }
    const bool nameMatches = !_wcsicmp(profileName, expectedProfileName.c_str());
    ::CoTaskMemFree(profileName);
    if (!nameMatches) return false;

    LPWSTR profilePath = nullptr;
    const HRESULT pathResult = profile->get_ProfilePath(&profilePath);
    if (FAILED(pathResult) || !profilePath) {
        if (profilePath) ::CoTaskMemFree(profilePath);
        return false;
    }
    const std::filesystem::path actualProfilePath(profilePath);
    ::CoTaskMemFree(profilePath);

    const std::filesystem::path userDataPath(userDataFolder);
    // ProfileName is the ownership key. The path check accepts the runtime's
    // actual leaf (which may be normalized, such as "Profile 1"), but only
    // when its immediate parent is this host's exact EBWebView directory.
    // Never treat an arbitrary descendant or another profile root as owned.
    return samePath(actualProfilePath.parent_path(),
        userDataPath / L"EBWebView");
}

void webViewModuleAnchor()
{
}

bool pinPluginModule()
{
    static std::once_flag once;
    static bool success = false;
    std::call_once(once, [] {
        HMODULE pinned = nullptr;
        // WebView2 completion handlers are executable code in this DLL. Pin
        // the module for process lifetime before registering the first async
        // callback so a host-side FreeLibrary cannot unload their code.
        success = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&webViewModuleAnchor), &pinned) != FALSE;
    });
    return success;
}

} // namespace

struct WebViewHost::AsyncState {
    std::atomic_bool alive{true};
    WebViewHost* owner = nullptr;
    std::shared_ptr<WebViewHost::CleanupState> cleanupState;
#ifdef NPPTERMINAL_TESTS
    std::atomic_bool environmentCallbackDelivered{false};
#endif
};

struct WebViewHost::CleanupState : std::enable_shared_from_this<WebViewHost::CleanupState> {
    struct ObserverGuard final {
        std::weak_ptr<CleanupState> state;
        std::atomic_bool active{true};
    };

    profilecleanup::OwnedProfile profile;
    std::wstring moduleDirectory;
    ComPtr<ICoreWebView2Environment5> environment5;
    ComPtr<ICoreWebView2Environment8> environment8;
    EventRegistrationToken browserProcessExitedToken{};
    bool browserProcessExitedRegistered = false;
    bool closeRequested = false;
    bool browserProcessExited = false;
    bool browserProcessExitedBeforeClose = false;
    bool markerClosing = false;
    bool completionAttempted = false;
    bool environmentCreationInitiated = false;
    bool environmentAttached = false;
    bool processSnapshotComplete = false;
    std::vector<profilecleanup::ProcessIdentity> browserProcesses;
    HWND observerWindow = nullptr;
    UINT_PTR observerTimer = 0;
    std::shared_ptr<ObserverGuard> observerGuard;
    bool observerExpired = false;
    bool observerAbandoned = false;
    bool helperLaunchAttempted = false;
#ifdef NPPTERMINAL_TESTS
    bool testEmptySnapshot = false;
    bool testSuppressBrowserProcessExited = false;
    DWORD testObserverTimeoutMs = 0;
    unsigned int testBrowserProcessExitedAddCount = 0;
    unsigned int testBrowserProcessExitedRemoveCount = 0;
    bool testHelperLaunchSucceeded = false;
#endif

    CleanupState(profilecleanup::OwnedProfile ownedProfile, std::wstring modulePath)
        : profile(std::move(ownedProfile))
        , moduleDirectory(std::move(modulePath))
    {
    }

    ~CleanupState()
    {
        stopObserver();
        profilecleanup::releaseOwnedProfileLease(profile);
    }

    static LRESULT CALLBACK observerWindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* guard = reinterpret_cast<ObserverGuard*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            guard = create ? static_cast<ObserverGuard*>(create->lpCreateParams) : nullptr;
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(guard));
        }
        if (guard && guard->active.load()) {
            if (message == WM_TIMER && wParam == kCleanupObserverTimerId) {
                if (const auto state = guard->state.lock(); state &&
                    guard->active.load()) {
                    state->onObserverExpired();
                }
                return 0;
            }
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    static bool registerObserverWindowClass()
    {
        static std::once_flag once;
        static ATOM registered = 0;
        std::call_once(once, [] {
            HMODULE module = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&webViewModuleAnchor), &module) || !module) {
                return;
            }
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = &CleanupState::observerWindowProc;
            windowClass.hInstance = module;
            windowClass.lpszClassName = kCleanupObserverClass;
            registered = ::RegisterClassExW(&windowClass);
            if (!registered && ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
                registered = 1;
            }
            // The plugin module is already pinned before any production
            // observer can be created.  The test executable is process-lived.
            // Release only this lookup reference; the class stores the module
            // handle value but does not own a separate reference.
            ::FreeLibrary(module);
        });
        return registered != 0;
    }

    DWORD observerTimeoutForClose() const
    {
#ifdef NPPTERMINAL_TESTS
        if (testObserverTimeoutMs != 0) return testObserverTimeoutMs;
#endif
        return kCleanupObserverTimeoutMs;
    }

    bool startObserver(DWORD timeoutMs = kCleanupObserverTimeoutMs)
    {
        if (observerWindow || !closeRequested || !markerClosing ||
            observerAbandoned || completionAttempted) return observerWindow != nullptr;
        if (!registerObserverWindowClass()) return false;

        auto guard = std::make_shared<ObserverGuard>();
        guard->state = shared_from_this();
        HMODULE module = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&webViewModuleAnchor), &module) || !module) {
            return false;
        }
        const HWND window = ::CreateWindowExW(0, kCleanupObserverClass, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, module, guard.get());
        ::FreeLibrary(module);
        if (!window) return false;
        const UINT_PTR timer = ::SetTimer(window, kCleanupObserverTimerId,
            timeoutMs == 0 ? 1u : timeoutMs, nullptr);
        if (!timer) {
            guard->active.store(false);
            guard->state.reset();
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            ::DestroyWindow(window);
            return false;
        }
        observerGuard = std::move(guard);
        observerWindow = window;
        observerTimer = timer;
        return true;
    }

    void stopObserver()
    {
        const auto guard = observerGuard;
        if (guard) {
            guard->active.store(false);
            guard->state.reset();
        }
        const HWND window = observerWindow;
        observerWindow = nullptr;
        observerTimer = 0;
        if (window) {
            (void)::KillTimer(window, kCleanupObserverTimerId);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            ::DestroyWindow(window);
        }
        observerGuard.reset();
    }

    void onObserverExpired()
    {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        if (completionAttempted || !closeRequested || !markerClosing) {
            stopObserver();
            return;
        }
        observerExpired = true;
        observerAbandoned = true;
        // A missing post-close BrowserProcessExited event is ambiguous.  The
        // durable Closing marker is intentionally left untouched; only the
        // event proof may authorize Released and helper deletion.
        stopObserver();
        detachBrowserProcessExitedHandler();
    }

#ifdef NPPTERMINAL_TESTS
    void injectCompleteEmptySnapshotForTest()
    {
        testEmptySnapshot = true;
    }
#endif

    bool captureProcesses()
    {
        browserProcesses.clear();
        processSnapshotComplete = false;
        if (!environment8) return false;

        ComPtr<ICoreWebView2ProcessInfoCollection> collection;
        UINT count = 0;
        if (FAILED(environment8->GetProcessInfos(&collection)) || !collection ||
            FAILED(collection->get_Count(&count))) return false;
        bool complete = count <= profilecleanup::kMaxTrackedProcesses;
        const UINT boundedCount = std::min<UINT>(count,
            static_cast<UINT>(profilecleanup::kMaxTrackedProcesses));
        browserProcesses.reserve(boundedCount);
        for (UINT index = 0; index < boundedCount; ++index) {
            ComPtr<ICoreWebView2ProcessInfo> processInfo;
            INT32 processId = 0;
            if (FAILED(collection->GetValueAtIndex(index, &processInfo)) || !processInfo ||
                FAILED(processInfo->get_ProcessId(&processId)) || processId <= 0) {
                complete = false;
                continue;
            }
            profilecleanup::ProcessIdentity identity;
            if (!profilecleanup::queryProcessIdentity(static_cast<DWORD>(processId), identity)) {
                complete = false;
                continue;
            }
            const auto duplicate = std::find_if(browserProcesses.begin(), browserProcesses.end(),
                [&identity](const profilecleanup::ProcessIdentity& existing) {
                    return existing.processId == identity.processId;
                });
            if (duplicate == browserProcesses.end()) browserProcesses.push_back(identity);
        }
        processSnapshotComplete = complete;
        return true;
    }

    bool persistSnapshot(profilecleanup::CleanupPhase phase, std::wstring& error)
    {
        return profilecleanup::updateOwnedProfile(profile, phase, browserProcesses,
            processSnapshotComplete, error);
    }

#ifdef NPPTERMINAL_TESTS
    bool persistLateClosingSnapshot()
    {
        if (!closeRequested || !markerClosing) return true;
        if (!captureProcesses() || !processSnapshotComplete) return false;
        std::wstring error;
        if (!profilecleanup::acquireOwnedProfileLease(profile, error)) return false;
        const bool persisted = persistSnapshot(profilecleanup::CleanupPhase::Closing, error);
        profilecleanup::releaseOwnedProfileLease(profile);
        return persisted;
    }
#endif

    void detachBrowserProcessExitedHandler()
    {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        stopObserver();
        if (environment5 && browserProcessExitedRegistered) {
#ifdef NPPTERMINAL_TESTS
            ++testBrowserProcessExitedRemoveCount;
#endif
            (void)environment5->remove_BrowserProcessExited(browserProcessExitedToken);
        }
        browserProcessExitedRegistered = false;
        environment8.Reset();
        environment5.Reset();
    }

    void completeReleased()
    {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        if (!closeRequested || !markerClosing || completionAttempted || !browserProcessExited) return;
        stopObserver();
        std::wstring error;
        if (!profilecleanup::acquireOwnedProfileLease(profile, error)) {
            observerAbandoned = true;
            profilecleanup::releaseOwnedProfileLease(profile);
            // Do not retain an observer cycle after a terminal persistence
            // failure. The marker remains Closing and recovery will not infer
            // BrowserProcessExited from PID state.
            detachBrowserProcessExitedHandler();
            return;
        }
        if (!persistSnapshot(profilecleanup::CleanupPhase::Released, error)) {
            observerAbandoned = true;
            profilecleanup::releaseOwnedProfileLease(profile);
            detachBrowserProcessExitedHandler();
            return;
        }
        profilecleanup::releaseOwnedProfileLease(profile);
        completionAttempted = true;
        helperLaunchAttempted = true;
#ifdef NPPTERMINAL_TESTS
        testHelperLaunchSucceeded = profilecleanup::launchCleanupHelper(
            moduleDirectory, profile, error);
#else
        (void)profilecleanup::launchCleanupHelper(moduleDirectory, profile, error);
#endif
        detachBrowserProcessExitedHandler();
    }

    void completeWithoutEnvironment()
    {
        if (completionAttempted || closeRequested || environmentCreationInitiated ||
            environmentAttached) return;
        closeRequested = true;
        browserProcessExited = true;
        processSnapshotComplete = true;
        std::wstring error;
        if (!persistSnapshot(profilecleanup::CleanupPhase::Released, error)) {
            profilecleanup::releaseOwnedProfileLease(profile);
            return;
        }
        profilecleanup::releaseOwnedProfileLease(profile);
        completionAttempted = true;
        helperLaunchAttempted = true;
#ifdef NPPTERMINAL_TESTS
        testHelperLaunchSucceeded = profilecleanup::launchCleanupHelper(
            moduleDirectory, profile, error);
#else
        (void)profilecleanup::launchCleanupHelper(moduleDirectory, profile, error);
#endif
    }

    void onBrowserProcessExited(ICoreWebView2BrowserProcessExitedEventArgs* args)
    {
        const auto keepAlive = shared_from_this();
        (void)keepAlive;
        if (!args) return;
        if (!closeRequested || !markerClosing) {
            // A notification received before Closing is persisted cannot prove
            // that the resources released by this close operation are gone.
            browserProcessExitedBeforeClose = true;
            return;
        }
        browserProcessExited = true;
        completeReleased();
    }

    void attachEnvironment(ICoreWebView2Environment* environment)
    {
        if (!environment || completionAttempted || observerAbandoned ||
            (closeRequested && !markerClosing)) return;
        environmentAttached = true;
        environment->QueryInterface(IID_PPV_ARGS(&environment8));
        environment->QueryInterface(IID_PPV_ARGS(&environment5));
        if (!environment5) return;
#ifdef NPPTERMINAL_TESTS
        // A late environment completion can arrive after close persisted an
        // incomplete marker. Capture and persist the exact process set before
        // starting the bounded observer so the integration fixture can prove
        // the family exited before its test-only teardown.
        if (closeRequested && markerClosing) (void)persistLateClosingSnapshot();
#endif
        // Keep the independent cleanup state alive after WebViewHost::close()
        // releases its own reference. The event is the release proof; every
        // terminal path below removes this handler and breaks the cycle.
        const auto state = shared_from_this();
        const HRESULT result = environment5->add_BrowserProcessExited(
            Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                [state](ICoreWebView2Environment*,
                    ICoreWebView2BrowserProcessExitedEventArgs* args) -> HRESULT {
#ifdef NPPTERMINAL_TESTS
                    if (state->testSuppressBrowserProcessExited) return S_OK;
#endif
                    state->onBrowserProcessExited(args);
                    return S_OK;
                }).Get(), &browserProcessExitedToken);
        browserProcessExitedRegistered = SUCCEEDED(result);
#ifdef NPPTERMINAL_TESTS
        if (browserProcessExitedRegistered) ++testBrowserProcessExitedAddCount;
#endif
        if (!browserProcessExitedRegistered) return;
        // Environment creation may complete after WebViewHost::close() has
        // already persisted Closing. Start the same bounded observer on this
        // STA before releasing the independent callback state.
        if (closeRequested && markerClosing && !startObserver(observerTimeoutForClose())) {
            observerAbandoned = true;
            detachBrowserProcessExitedHandler();
        }
    }

    void requestClose()
    {
        if (closeRequested) return;
        closeRequested = true;
        const bool snapshotAvailable =
#ifdef NPPTERMINAL_TESTS
            testEmptySnapshot ? (browserProcesses.clear(), processSnapshotComplete = true, true) :
#endif
            captureProcesses();
        // A BrowserProcessExited notification observed before this close is
        // never reused as release proof, even when the snapshot is empty.
        browserProcessExited = false;
        std::wstring error;
        markerClosing = persistSnapshot(profilecleanup::CleanupPhase::Closing, error);
        profilecleanup::releaseOwnedProfileLease(profile);
        if (!markerClosing) {
            observerAbandoned = true;
            detachBrowserProcessExitedHandler();
            return;
        }
        if (browserProcessExitedBeforeClose && snapshotAvailable &&
            processSnapshotComplete && browserProcesses.empty() && !browserProcessExited) {
            // There is no expected future browser-process exit after the
            // pre-close exit and empty snapshot. Leave the durable marker in
            // Closing and detach the observer instead of leaking its COM
            // cycle or deleting from PID-snapshot inference.
            observerAbandoned = true;
            detachBrowserProcessExitedHandler();
            return;
        }
        if (browserProcessExitedRegistered && !startObserver(observerTimeoutForClose())) {
            observerAbandoned = true;
            detachBrowserProcessExitedHandler();
            return;
        }
        completeReleased();
    }
};

struct WebViewHost::Impl {
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    ComPtr<ICoreWebView2_3> webView3;
    ComPtr<ICoreWebView2_4> webView4;
    ComPtr<ICoreWebView2_13> webView13;
    ComPtr<ICoreWebView2Profile8> profile8;
};

WebViewHost::WebViewHost()
    : impl_(std::make_unique<Impl>())
{
}

WebViewHost::~WebViewHost()
{
    close();
}

bool WebViewHost::create(HWND parent, HMODULE module, MessageCallback messageCallback,
    NavigationCallback navigationCallback, ErrorCallback errorCallback)
{
    if (!parent || !module || !impl_) return false;
    if (asyncState_ && asyncState_->alive.load()) return true;
    close();
    parent_ = parent;
    module_ = module;
    messageCallback_ = std::move(messageCallback);
    navigationCallback_ = std::move(navigationCallback);
    errorCallback_ = std::move(errorCallback);
    webFolder_ = moduleDirectory(module_) + L"\\web";
    if (webFolder_.empty() || ::GetFileAttributesW(webFolder_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fail(L"The packaged web folder is missing next to NppTerminal.dll.");
        return false;
    }

    const std::wstring appData = localAppData();
    if (appData.empty()) {
        fail(L"Unable to locate the local application-data folder for WebView2.");
        return false;
    }
    static std::once_flag recoveryOnce;
    std::call_once(recoveryOnce, [this] {
        std::wstring ignored;
        (void)profilecleanup::launchRecoveryHelper(moduleDirectory(module_), ignored);
    });
    profilecleanup::ProcessIdentity hostProcess;
    if (!profilecleanup::queryProcessIdentity(::GetCurrentProcessId(), hostProcess)) {
        fail(L"Unable to record the Notepad++ process identity for WebView2 cleanup.");
        return false;
    }
    profilecleanup::OwnedProfile ownedProfile;
    std::wstring profileError;
    const std::wstring profileParent = appData + L"\\NppTerminal\\WebView";
    if (!profilecleanup::createOwnedProfile(profileParent, hostProcess, ownedProfile,
        profileError)) {
        fail(profileError.empty() ? L"Unable to create the WebView2 user-data folder." : profileError);
        return false;
    }
    userDataFolder_ = ownedProfile.rootPath;
    profileName_ = ownedProfile.instanceId;
    cleanupState_ = std::make_shared<CleanupState>(std::move(ownedProfile),
        moduleDirectory(module_));

    // Pending environment/controller creation handlers cannot be revoked by
    // Controller::Close. Keep their DLL code available even after owner close;
    // this lazy renderer lifetime pin is independent of terminal shutdown.
    if (!pinPluginModule()) {
        if (cleanupState_) cleanupState_->completeWithoutEnvironment();
        fail(L"Unable to retain the plugin for asynchronous WebView2 callbacks.");
        return false;
    }
    asyncState_ = std::make_shared<AsyncState>();
    asyncState_->owner = this;
    asyncState_->cleanupState = cleanupState_;
    if (cleanupState_) cleanupState_->environmentCreationInitiated = true;
    const HRESULT result = ::CreateCoreWebView2EnvironmentWithOptions(nullptr,
        userDataFolder_.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [state = asyncState_](HRESULT hr, ICoreWebView2Environment* environment) -> HRESULT {
#ifdef NPPTERMINAL_TESTS
                state->environmentCallbackDelivered.store(true);
#endif
                if (!state->alive.load()) {
                    // The host may close while environment creation is still
                    // pending. Keep only the independent cleanup state alive
                    // long enough to register BrowserProcessExited; never
                    // call back through the destroyed WebViewHost here.
                    if (SUCCEEDED(hr) && environment && state->cleanupState) {
                        state->cleanupState->attachEnvironment(environment);
                    }
                    return S_OK;
                }
                state->owner->onEnvironment(hr, environment);
                return S_OK;
            }).Get());
    if (FAILED(result)) {
        // The API rejected the environment request synchronously, so no
        // browser-process lifetime was initiated. This is the one safe
        // no-environment path; an asynchronous callback failure remains
        // conservative and is handled as Closing during close().
        if (asyncState_) asyncState_->alive.store(false);
        if (cleanupState_) {
            cleanupState_->environmentCreationInitiated = false;
            cleanupState_->completeWithoutEnvironment();
        }
        fail(L"WebView2 runtime initialization failed: " + win32Error(static_cast<DWORD>(result)));
        return false;
    }
    return true;
}

void WebViewHost::onEnvironment(HRESULT result, ICoreWebView2Environment* environment)
{
    if (!asyncState_ || !asyncState_->alive.load()) return;
    if (FAILED(result) || !environment) {
        fail(L"WebView2 runtime is unavailable. Install the Evergreen WebView2 Runtime and retry.");
        return;
    }
    impl_->environment = environment;
    if (cleanupState_) cleanupState_->attachEnvironment(impl_->environment.Get());
    auto controllerCompleted = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [state = asyncState_](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
            if (!state->alive.load()) return S_OK;
            state->owner->onController(hr, controller);
            return S_OK;
        });

    HRESULT controllerResult = E_NOINTERFACE;
    ComPtr<ICoreWebView2Environment10> environment10;
    if (SUCCEEDED(impl_->environment.As(&environment10)) && environment10) {
        ComPtr<ICoreWebView2ControllerOptions> options;
        const HRESULT optionsResult = environment10->CreateCoreWebView2ControllerOptions(&options);
        if (FAILED(optionsResult) || !options ||
            FAILED(options->put_ProfileName(profileName_.c_str())) ||
            FAILED(options->put_IsInPrivateModeEnabled(FALSE))) {
            fail(L"WebView2 could not create its disposable named profile.");
            return;
        }
        controllerResult = environment10->CreateCoreWebView2ControllerWithOptions(
            parent_, options.Get(), controllerCompleted.Get());
    } else {
        // Older runtimes have no named-profile controller API. Keep the
        // existing startup path for compatibility; the exact-name/path guard
        // below intentionally prevents profile deletion in that case.
        controllerResult = impl_->environment->CreateCoreWebView2Controller(
            parent_, controllerCompleted.Get());
    }
    if (FAILED(controllerResult)) {
        fail(L"WebView2 controller creation failed: " + win32Error(static_cast<DWORD>(controllerResult)));
    }
}

void WebViewHost::onController(HRESULT result, ICoreWebView2Controller* controller)
{
    if (!asyncState_ || !asyncState_->alive.load()) return;
    if (FAILED(result) || !controller) {
        fail(L"WebView2 controller creation failed: " + win32Error(static_cast<DWORD>(result)));
        return;
    }
    impl_->controller = controller;
    if (FAILED(impl_->controller->get_CoreWebView2(&impl_->webView))) {
        fail(L"WebView2 did not provide a core view.");
        return;
    }
    impl_->controller->put_IsVisible(TRUE);
    impl_->webView.As(&impl_->webView3);
    impl_->webView.As(&impl_->webView4);
    impl_->webView.As(&impl_->webView13);

#ifdef NPPTERMINAL_TESTS
    if (forceMissingWebView4ForTest_) impl_->webView4.Reset();
#endif
    // FrameNavigationStarting and DownloadStarting are part of the required
    // security policy.  Never navigate a view when this capability is absent;
    // otherwise an older runtime would silently run without those denials.
    if (!impl_->webView4) {
        fail(kMissingSecurityCapability);
        return;
    }

    if (impl_->webView13) {
        ComPtr<ICoreWebView2Profile> profile;
        ComPtr<ICoreWebView2Profile8> profile8;
        if (SUCCEEDED(impl_->webView13->get_Profile(&profile)) && profile &&
            SUCCEEDED(profile.As(&profile8)) &&
            profileBelongsToUserDataFolder(profile.Get(), userDataFolder_, profileName_)) {
            // The user-data folder is unique to this WebViewHost instance. Keep
            // the profile handle so WebView2 can remove only this profile after
            // its browser process exits; never scan or delete shared folders.
            impl_->profile8 = profile8;
        }
    }
    if (cleanupState_) {
        cleanupState_->captureProcesses();
        std::wstring processError;
        (void)cleanupState_->persistSnapshot(profilecleanup::CleanupPhase::Active,
            processError);
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(impl_->webView->get_Settings(&settings))) {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);

        ComPtr<ICoreWebView2Settings4> settings4;
        if (SUCCEEDED(settings.As(&settings4))) {
            // The terminal page has no forms and does not need persisted
            // browser autofill state in its disposable profile.
            settings4->put_IsPasswordAutosaveEnabled(FALSE);
            settings4->put_IsGeneralAutofillEnabled(FALSE);
        }
    }

    auto state = asyncState_;
    EventRegistrationToken token{};
    impl_->webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onWebMessage(args);
                return S_OK;
            }).Get(), &token);
    impl_->webView->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onNavigationStarting(args);
                return S_OK;
            }).Get(), &token);
    impl_->webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (state->alive.load()) {
                    BOOL success = FALSE;
                    args->get_IsSuccess(&success);
                    state->owner->onNavigationCompleted(success ? S_OK : E_FAIL);
                }
                return S_OK;
            }).Get(), &token);
    impl_->webView->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onProcessFailed(args);
                return S_OK;
            }).Get(), &token);
    impl_->webView->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onNewWindow(args);
                return S_OK;
            }).Get(), &token);
    impl_->webView->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onPermission(args);
                return S_OK;
            }).Get(), &token);
    impl_->webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    impl_->webView->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onWebResource(args);
                return S_OK;
            }).Get(), &token);

    impl_->webView4->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onFrameNavigationStarting(args);
                return S_OK;
            }).Get(), &token);
    impl_->webView4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                if (state->alive.load()) state->owner->onDownload(args);
                return S_OK;
            }).Get(), &token);

    if (!impl_->webView3 || FAILED(impl_->webView3->SetVirtualHostNameToFolderMapping(
        L"nppterminal.invalid", webFolder_.c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))) {
        fail(L"WebView2 could not map the packaged web folder.");
        return;
    }
    if (!hasBounds_) {
        ::GetClientRect(parent_, &bounds_);
        hasBounds_ = true;
    }
    resize(bounds_);
    if (FAILED(impl_->webView->Navigate(kIndex))) {
        fail(L"WebView2 could not load the packaged terminal page.");
    }
}

void WebViewHost::onWebMessage(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2WebMessageReceivedEventArgs*>(rawArgs);
    LPWSTR source = nullptr;
    if (!args || FAILED(args->get_Source(&source)) || !source ||
        std::wstring(source) != kIndex) {
        if (source) ::CoTaskMemFree(source);
        return;
    }
    ::CoTaskMemFree(source);
    LPWSTR message = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&message)) || !message) return;
    constexpr std::size_t kMaxMessageCharacters = 512u * 1024u;
    if (wcsnlen_s(message, kMaxMessageCharacters + 1) > kMaxMessageCharacters) {
        ::CoTaskMemFree(message);
        return;
    }
    const std::wstring value(message);
    ::CoTaskMemFree(message);
    if (messageCallback_) messageCallback_(value);
}

void WebViewHost::onNavigationStarting(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2NavigationStartingEventArgs*>(rawArgs);
    LPWSTR uri = nullptr;
    if (!args || FAILED(args->get_Uri(&uri)) || !uri) {
        if (args) args->put_Cancel(TRUE);
        return;
    }
    const std::wstring value(uri);
    ::CoTaskMemFree(uri);
    if (value != kIndex) {
#ifdef NPPTERMINAL_TESTS
        ++securityObservation_.navigationCanceled;
#endif
        args->put_Cancel(TRUE);
    }
}

void WebViewHost::onFrameNavigationStarting(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2NavigationStartingEventArgs*>(rawArgs);
    if (!args) return;
#ifdef NPPTERMINAL_TESTS
    ++securityObservation_.frameNavigationCanceled;
#endif
    args->put_Cancel(TRUE);
}

void WebViewHost::onNavigationCompleted(HRESULT result)
{
    if (FAILED(result)) {
        fail(L"The packaged terminal page could not be loaded.");
        return;
    }
    ready_ = true;
    if (navigationCallback_) navigationCallback_();
}

void WebViewHost::onProcessFailed(void*)
{
    fail(L"The WebView2 renderer stopped unexpectedly. Retry the terminal.");
}

void WebViewHost::onNewWindow(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2NewWindowRequestedEventArgs*>(rawArgs);
    if (args) args->put_Handled(TRUE);
#ifdef NPPTERMINAL_TESTS
    if (args) ++securityObservation_.newWindowHandled;
#endif
}

void WebViewHost::onPermission(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2PermissionRequestedEventArgs*>(rawArgs);
    if (args) args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
#ifdef NPPTERMINAL_TESTS
    if (args) ++securityObservation_.permissionDenied;
#endif
}

void WebViewHost::onWebResource(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2WebResourceRequestedEventArgs*>(rawArgs);
    if (!args || !impl_->environment) return;
#ifdef NPPTERMINAL_TESTS
    ++securityObservation_.webResourceObserved;
#endif

    ComPtr<ICoreWebView2WebResourceRequest> request;
    LPWSTR uri = nullptr;
    if (FAILED(args->get_Request(&request)) || !request || FAILED(request->get_Uri(&uri)) || !uri) return;
    const std::wstring value(uri);
    ::CoTaskMemFree(uri);
    if (isAllowedUri(value)) return;

#ifdef NPPTERMINAL_TESTS
    ++securityObservation_.webResourceBlocked;
#endif

    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (SUCCEEDED(impl_->environment->CreateWebResourceResponse(nullptr, 403, L"Blocked",
        L"Content-Length: 0\r\n", &response))) {
        args->put_Response(response.Get());
    }
}

void WebViewHost::onDownload(void* rawArgs)
{
    auto* args = static_cast<ICoreWebView2DownloadStartingEventArgs*>(rawArgs);
    if (args) {
        args->put_Cancel(TRUE);
        args->put_Handled(TRUE);
#ifdef NPPTERMINAL_TESTS
        ++securityObservation_.downloadCanceled;
#endif
    }
}

void WebViewHost::resize(const RECT& bounds)
{
    bounds_ = bounds;
    hasBounds_ = true;
    if (impl_ && impl_->controller) impl_->controller->put_Bounds(bounds);
}

bool WebViewHost::postJson(const std::wstring& json)
{
    return impl_ && impl_->webView && SUCCEEDED(impl_->webView->PostWebMessageAsJson(json.c_str()));
}

#ifdef NPPTERMINAL_TESTS
bool WebViewHost::executeScriptForTest(const std::wstring& script,
    ScriptCallback callback)
{
    if (!impl_ || !impl_->webView || script.empty()) return false;
    auto callbackState = std::make_shared<ScriptCallback>(std::move(callback));
    const HRESULT result = impl_->webView->ExecuteScript(script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callbackState](HRESULT errorCode, LPCWSTR resultText) -> HRESULT {
                if (*callbackState) {
                    (*callbackState)(errorCode, resultText ? resultText : L"");
                }
                return S_OK;
            }).Get());
    return SUCCEEDED(result);
}

void WebViewHost::forceMissingWebView4ForTest()
{
    forceMissingWebView4ForTest_ = true;
}

WebViewHost::SecurityTestObservation WebViewHost::securityObservationForTest() const
{
    return securityObservation_;
}

std::wstring WebViewHost::userDataFolderForTest() const
{
    return userDataFolder_;
}
#endif

bool WebViewHost::isReady() const
{
    return ready_ && impl_ && impl_->webView;
}

void WebViewHost::fail(const std::wstring& message)
{
    ready_ = false;
    if (errorCallback_) errorCallback_(message);
}

void WebViewHost::close()
{
    ready_ = false;
    if (asyncState_) asyncState_->alive.store(false);
    if (cleanupState_) cleanupState_->requestClose();
    if (impl_) {
        if (impl_->profile8) {
            // Mark the exact owned profile before closing its controller. The
            // WebView2 API removes the profile at browser-process exit without
            // filesystem work on the UI thread. Failure is best effort only.
            (void)impl_->profile8->Delete();
        }
        if (impl_->controller) impl_->controller->Close();
        impl_->webView.Reset();
        impl_->webView4.Reset();
        impl_->webView3.Reset();
        impl_->webView13.Reset();
        impl_->profile8.Reset();
        impl_->controller.Reset();
        impl_->environment.Reset();
    }
    asyncState_.reset();
    cleanupState_.reset();
    parent_ = nullptr;
    module_ = nullptr;
}

#ifdef NPPTERMINAL_TESTS
bool WebViewHost::closeAndWaitForCleanupForTest(DWORD timeoutMs)
{
    const auto state = cleanupState_;
    close();
    if (!state) return false;
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    do {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                ::PostQuitMessage(static_cast<int>(message.wParam));
                return false;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        const DWORD attributes = ::GetFileAttributesW(state->profile.rootPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
        const bool absent = attributes == INVALID_FILE_ATTRIBUTES &&
            (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
        if (state->browserProcessExited && state->completionAttempted &&
            !state->browserProcessExitedRegistered && !state->observerWindow &&
            state->testHelperLaunchSucceeded && absent) return true;
        ::Sleep(5);
    } while (::GetTickCount64() < deadline);
    return false;
}

void WebViewHost::runCleanupObserverTests()
{
    if (!pinPluginModule()) {
        throw std::runtime_error("cleanup observer test could not pin its callback module");
    }

    const std::wstring appData = localAppData();
    if (appData.empty()) throw std::runtime_error("cleanup observer test has no LocalAppData");
    const std::wstring parent = appData + L"\\NppTerminal\\WebView";

    profilecleanup::ProcessIdentity hostProcess;
    if (!profilecleanup::queryProcessIdentity(::GetCurrentProcessId(), hostProcess)) {
        throw std::runtime_error("cleanup observer test could not query its process identity");
    }

    struct RootGuard final {
        std::wstring root;
    };

    const auto makeState = [&](RootGuard& rootGuard, std::wstring& instanceId,
        std::array<std::uint8_t, profilecleanup::kNonceBytes>& nonce) {
        profilecleanup::OwnedProfile profile;
        std::wstring error;
        if (!profilecleanup::createOwnedProfile(parent, hostProcess, profile, error)) {
            throw std::runtime_error("cleanup observer test could not create a profile");
        }
        rootGuard.root = profile.rootPath;
        instanceId = profile.instanceId;
        nonce = profile.nonce;
        return std::make_shared<CleanupState>(std::move(profile), std::wstring{});
    };
    const auto releaseAndClean = [&](const std::shared_ptr<CleanupState>& state) {
        std::wstring error;
        return profilecleanup::cleanupOwnedFixtureForTest(state->profile, error) == 0;
    };

    std::uint8_t dummyArgument = 0;
    auto* dummyExitArgs = reinterpret_cast<ICoreWebView2BrowserProcessExitedEventArgs*>(
        &dummyArgument);

    // A pre-close event plus a complete empty process snapshot does not prove
    // that the close operation released anything.  The real requestClose()
    // path must leave the marker Closing and release every observer reference.
    {
        RootGuard root;
        std::wstring instanceId;
        std::array<std::uint8_t, profilecleanup::kNonceBytes> nonce{};
        const auto state = makeState(root, instanceId, nonce);
        state->injectCompleteEmptySnapshotForTest();
        state->onBrowserProcessExited(dummyExitArgs);
        state->requestClose();
        if (!state->markerClosing || !state->observerAbandoned ||
            state->observerWindow || state->observerTimer || state->environment5 ||
            state->environment8) {
            throw std::runtime_error("pre-close cleanup proof was incorrectly promoted");
        }
        std::wstring error;
        if (profilecleanup::cleanupProfileForTest(parent, instanceId, nonce, error) == 0) {
            throw std::runtime_error("pre-close cleanup test accepted Closing as Released");
        }
        if (!releaseAndClean(state)) {
            throw std::runtime_error("pre-close cleanup fixture could not be retired safely");
        }
    }

    // A post-close event is the only release proof.  The actual completion path
    // writes Released and attempts the adjacent helper even when this unit test
    // supplies an empty helper directory.
    {
        RootGuard root;
        std::wstring instanceId;
        std::array<std::uint8_t, profilecleanup::kNonceBytes> nonce{};
        const auto state = makeState(root, instanceId, nonce);
        state->injectCompleteEmptySnapshotForTest();
        state->requestClose();
        state->onBrowserProcessExited(dummyExitArgs);
        if (!state->completionAttempted || !state->helperLaunchAttempted ||
            state->observerWindow || state->observerTimer) {
            throw std::runtime_error("post-close browser exit did not complete cleanup state");
        }
        std::wstring error;
        if (profilecleanup::cleanupProfileForTest(parent, instanceId, nonce, error) != 0) {
            throw std::runtime_error("post-close Released marker could not be cleaned");
        }
    }

    // A missing event expires through the real message-only window procedure;
    // the short test timer keeps this deterministic while production retains a
    // bounded 30-second STA observer.
    {
        RootGuard root;
        std::wstring instanceId;
        std::array<std::uint8_t, profilecleanup::kNonceBytes> nonce{};
        const auto state = makeState(root, instanceId, nonce);
        state->injectCompleteEmptySnapshotForTest();
        state->requestClose();
        if (!state->startObserver(1)) {
            throw std::runtime_error("cleanup observer test could not create message-only window");
        }
        const ULONGLONG deadline = ::GetTickCount64() + 2000;
        while (!state->observerExpired && ::GetTickCount64() < deadline) {
            MSG message{};
            if (::PeekMessageW(&message, state->observerWindow, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            } else {
                ::Sleep(1);
            }
        }
        if (!state->observerExpired || !state->observerAbandoned ||
            state->observerWindow || state->observerTimer || state->completionAttempted) {
            throw std::runtime_error("cleanup observer expiry did not detach safely");
        }
        std::wstring error;
        if (profilecleanup::cleanupProfileForTest(parent, instanceId, nonce, error) == 0) {
            throw std::runtime_error("observer expiry promoted Closing to Released");
        }
        if (!releaseAndClean(state)) {
            throw std::runtime_error("observer expiry fixture could not be retired safely");
        }
    }
}

void WebViewHost::runCleanupIntegrationTests()
{
    if (!pinPluginModule()) {
        throw std::runtime_error("WEBVIEW_INTEGRATION NOT_RUN: could not pin the test module");
    }

    const auto narrow = [](const std::wstring& value) {
        std::string result;
        result.reserve(value.size());
        for (const wchar_t character : value) {
            result.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
        }
        return result;
    };
    const auto notRun = [&](const std::wstring& reason) {
        throw std::runtime_error("WEBVIEW_INTEGRATION NOT_RUN: " + narrow(reason));
    };
    const auto pathExists = [](const std::wstring& path) {
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const auto isRuntimeUnavailable = [](const std::wstring& message) {
        return message.find(L"WebView2 runtime is unavailable") != std::wstring::npos ||
            message.find(L"WebView2 runtime initialization failed") != std::wstring::npos;
    };

    const HMODULE testModule = ::GetModuleHandleW(nullptr);
    const std::wstring testDirectory = moduleDirectory(testModule);
    if (!testModule || testDirectory.empty()) {
        notRun(L"the test executable directory could not be resolved");
    }
    const std::wstring webFolder = testDirectory + L"\\web";
    const std::wstring brokerPath = testDirectory + L"\\NppTerminalBroker.exe";
    if (!pathExists(webFolder)) {
        notRun(L"the packaged web folder is missing beside the test executable");
    }
    if (!pathExists(brokerPath)) {
        notRun(L"the production cleanup helper is missing beside the test executable");
    }

    const HRESULT apartmentResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (apartmentResult == RPC_E_CHANGED_MODE) {
        notRun(L"the native test thread is already initialized in a non-STA apartment");
    }
    if (FAILED(apartmentResult)) {
        notRun(L"CoInitializeEx(COINIT_APARTMENTTHREADED) failed");
    }
    struct ApartmentGuard final {
        ~ApartmentGuard()
        {
            ::CoUninitialize();
        }
    } apartmentGuard;

    constexpr wchar_t kWindowClass[] = L"NppTerminalWebViewIntegrationWindow-v1";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &::DefWindowProcW;
    windowClass.hInstance = testModule;
    windowClass.lpszClassName = kWindowClass;
    const ATOM registered = ::RegisterClassW(&windowClass);
    const bool registeredHere = registered != 0;
    if (!registeredHere && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("WEBVIEW_INTEGRATION failed to register its hidden window");
    }
    const HWND parent = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"NppTerminal WebView integration", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, testModule, nullptr);
    if (!parent) {
        if (registeredHere) ::UnregisterClassW(kWindowClass, testModule);
        throw std::runtime_error("WEBVIEW_INTEGRATION failed to create its hidden window");
    }
    struct HiddenWindowGuard final {
        HWND window = nullptr;
        HMODULE module = nullptr;
        const wchar_t* className = nullptr;
        bool unregister = false;
        ~HiddenWindowGuard()
        {
            if (window) ::DestroyWindow(window);
            if (unregister) ::UnregisterClassW(className, module);
        }
    } windowGuard{parent, testModule, kWindowClass, registeredHere};

    const auto pumpUntil = [](auto&& predicate, DWORD timeoutMs) {
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        for (;;) {
            MSG message{};
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    ::PostQuitMessage(static_cast<int>(message.wParam));
                    return false;
                }
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            if (predicate()) return true;
            if (::GetTickCount64() >= deadline) return predicate();
            ::Sleep(5);
        }
    };
    const auto processExited = [](const profilecleanup::ProcessIdentity& identity) {
        if (identity.processId == 0) return true;
        HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, identity.processId);
        if (!process) {
            const DWORD error = ::GetLastError();
            return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND;
        }
        const DWORD waitResult = ::WaitForSingleObject(process, 0);
        if (waitResult == WAIT_OBJECT_0) {
            ::CloseHandle(process);
            return true;
        }
        if (waitResult != WAIT_TIMEOUT) {
            ::CloseHandle(process);
            return false;
        }
        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        const bool queried = ::GetProcessTimes(process, &creationTime, &exitTime,
            &kernelTime, &userTime) != FALSE;
        const bool identityChanged = queried &&
            ::CompareFileTime(&creationTime, &identity.creationTime) != 0;
        ::CloseHandle(process);
        return identityChanged;
    };
    const auto familyExited = [&](const std::vector<profilecleanup::ProcessIdentity>& processes) {
        return std::all_of(processes.begin(), processes.end(), processExited);
    };
    const auto releaseAndClean = [&](const std::shared_ptr<CleanupState>& state) {
        // A late WebView2 process can leave a transient handle to the exact
        // owned profile after its process identity is inactive. Retry only
        // after the validated helper has promoted this profile to Released;
        // an incomplete Closing marker remains a hard proof failure.
        const ULONGLONG deadline = ::GetTickCount64() + 15000;
        std::wstring error;
        bool cleaned = false;
        pumpUntil([&] {
            error.clear();
            cleaned = profilecleanup::cleanupOwnedFixtureForTest(state->profile, error) == 0;
            if (cleaned || state->profile.phase != profilecleanup::CleanupPhase::Released) {
                return true;
            }
            return ::GetTickCount64() >= deadline;
        }, 15000);
        if (cleaned) return std::string{};
        if (error.empty()) error = L"the validated cleanup helper returned no diagnostic";
        return narrow(error);
    };

    const auto waitForReady = [&](bool& ready, std::wstring& callbackError) {
        return pumpUntil([&] {
            return ready || !callbackError.empty();
        }, 15000);
    };

    const auto createHost = [&](WebViewHost& host, bool& ready,
        std::wstring& callbackError) {
        return host.create(parent, testModule,
            [](const std::wstring&) {},
            [&] { ready = true; },
            [&](const std::wstring& message) { callbackError = message; });
    };

    // First establish that the packaged runtime and assets can execute one
    // normal navigation.  Later cases use the same actual COM lifecycle.
    {
        WebViewHost host;
        bool ready = false;
        std::wstring callbackError;
        if (!createHost(host, ready, callbackError)) {
            host.close();
            if (isRuntimeUnavailable(callbackError)) notRun(callbackError);
            throw std::runtime_error("WEBVIEW_INTEGRATION create failed: " +
                narrow(callbackError.empty() ? L"unknown startup error" : callbackError));
        }
        auto state = host.cleanupState_;
        if (!state) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION did not create cleanup state");
        }
        const std::wstring root = state->profile.rootPath;
        const auto nonce = state->profile.nonce;
        if (!waitForReady(ready, callbackError)) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION navigation timed out");
        }
        if (!ready) {
            host.close();
            if (isRuntimeUnavailable(callbackError)) notRun(callbackError);
            throw std::runtime_error("WEBVIEW_INTEGRATION navigation failed: " +
                narrow(callbackError));
        }
        if (state->testBrowserProcessExitedAddCount != 1 ||
            !state->browserProcessExitedRegistered) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION did not register BrowserProcessExited");
        }
        host.close();
        if (!pumpUntil([&] {
            return state->completionAttempted &&
                state->testBrowserProcessExitedRemoveCount == 1;
        }, 15000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION normal close did not detach");
        }
        if (!state->browserProcessExited || state->environment5 || state->environment8 ||
            state->observerWindow || !state->helperLaunchAttempted ||
            !state->testHelperLaunchSucceeded) {
            throw std::runtime_error("WEBVIEW_INTEGRATION normal close left cleanup resources");
        }
        if (!pumpUntil([&] { return !pathExists(root); }, 15000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION normal cleanup left the owned folder");
        }
        const std::weak_ptr<CleanupState> weak = state;
        state.reset();
        if (!pumpUntil([&] { return weak.expired(); }, 5000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION normal cleanup state did not expire");
        }
        (void)nonce;
        std::cout << "WEBVIEW_INTEGRATION normal PASS\n";
    }

    // Drop only the callback's completion action while keeping the actual
    // WebView2 registration.  The short test timeout must detach COM and leave
    // the exact fixture in Closing for conservative recovery.
    {
        WebViewHost host;
        bool ready = false;
        std::wstring callbackError;
        if (!createHost(host, ready, callbackError)) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION fault fixture create failed: " +
                narrow(callbackError));
        }
        auto state = host.cleanupState_;
        const std::wstring root = state->profile.rootPath;
        const std::wstring instanceId = state->profile.instanceId;
        const auto nonce = state->profile.nonce;
        if (!waitForReady(ready, callbackError)) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION fault fixture navigation timed out");
        }
        if (!ready) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION fault fixture navigation failed: " +
                narrow(callbackError));
        }
        state->testSuppressBrowserProcessExited = true;
        state->testObserverTimeoutMs = 250;
        host.close();
        if (!pumpUntil([&] { return state->observerExpired; }, 10000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION missing-event observer did not expire");
        }
        if (state->testBrowserProcessExitedAddCount != 1 ||
            state->testBrowserProcessExitedRemoveCount != 1 || state->completionAttempted ||
            state->helperLaunchAttempted || state->browserProcessExited ||
            state->environment5 || state->environment8 || state->observerWindow ||
            !state->markerClosing || !state->processSnapshotComplete || !pathExists(root)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION missing-event cleanup state was unsafe");
        }
        std::wstring cleanupError;
        if (profilecleanup::cleanupProfileForTest(state->profile.parentIdentity, instanceId,
            nonce, cleanupError) == 0) {
            throw std::runtime_error("WEBVIEW_INTEGRATION missing-event marker was not Closing");
        }
        if (!pumpUntil([&] { return familyExited(state->browserProcesses); }, 10000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION browser family did not exit after fault close");
        }
        const std::string cleanupFailure = releaseAndClean(state);
        if (!cleanupFailure.empty()) {
            throw std::runtime_error("WEBVIEW_INTEGRATION fault fixture could not be retired safely: " +
                cleanupFailure);
        }
        const std::weak_ptr<CleanupState> weak = state;
        state.reset();
        if (!pumpUntil([&] { return weak.expired(); }, 5000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION fault cleanup state did not expire");
        }
        if (pathExists(root)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION fault fixture cleanup failed");
        }
        std::cout << "WEBVIEW_INTEGRATION missing-event PASS\n";
    }

    // Close immediately after CreateCoreWebView2EnvironmentWithOptions is
    // requested.  The callback must never call back through WebViewHost after
    // alive is cleared; a late environment may still use the independent state.
    {
        WebViewHost host;
        bool ready = false;
        bool callbackAfterClose = false;
        bool closeStarted = false;
        std::wstring callbackError;
        if (!host.create(parent, testModule,
            [](const std::wstring&) {},
            [&] { if (closeStarted) callbackAfterClose = true; ready = true; },
            [&](const std::wstring& message) {
                if (closeStarted) callbackAfterClose = true;
                callbackError = message;
            })) {
            host.close();
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close create failed: " +
                narrow(callbackError));
        }
        auto state = host.cleanupState_;
        const std::wstring root = state->profile.rootPath;
        const std::wstring instanceId = state->profile.instanceId;
        auto asyncState = host.asyncState_;
        state->testObserverTimeoutMs = 250;
        const std::weak_ptr<CleanupState> weak = state;
        closeStarted = true;
        host.close();
        // Require the actual environment-completion callback to be delivered
        // before judging whether it attached a release observer.  This avoids
        // passing while the asynchronous request is merely still pending.
        if (!pumpUntil([&] {
            return asyncState->environmentCallbackDelivered.load();
        }, 10000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close callback did not complete");
        }
        // A successful late environment callback can register the real event
        // handler after the host has already closed.  Capture its process list
        // before waiting for either the release proof or observer expiry.
        if (state->environment8) (void)state->captureProcesses();
        if (!pumpUntil([&] {
            return !state->browserProcessExitedRegistered && !state->observerWindow &&
                (state->completionAttempted || state->observerExpired ||
                    state->observerAbandoned || state->testBrowserProcessExitedAddCount == 0);
        }, 10000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close cleanup did not reach a terminal state");
        }
        if (callbackAfterClose || state->browserProcessExitedRegistered ||
            state->observerWindow || state->environment5 || state->environment8) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate close retained owner or COM state");
        }
        if (state->testBrowserProcessExitedAddCount != 0 &&
            !state->processSnapshotComplete) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close lacked a complete late snapshot");
        }
        if (state->testBrowserProcessExitedRemoveCount !=
            state->testBrowserProcessExitedAddCount) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate close did not remove its event handler");
        }
        const auto processes = state->browserProcesses;
        const bool rootStillExists = pathExists(root);
        if (!pumpUntil([&] { return familyExited(processes); }, 10000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close browser family did not exit");
        }
        if (rootStillExists) {
            const std::string cleanupFailure = releaseAndClean(state);
            if (!cleanupFailure.empty()) {
                throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close fixture could not be retired safely: " +
                    cleanupFailure);
            }
        }
        state.reset();
        asyncState.reset();
        if (!pumpUntil([&] { return weak.expired(); }, 5000)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close state did not expire");
        }
        if (pathExists(root)) {
            throw std::runtime_error("WEBVIEW_INTEGRATION immediate-close fixture cleanup failed");
        }
        std::cout << "WEBVIEW_INTEGRATION immediate-close PASS\n";
    }
}
#endif

} // namespace nppterminal
