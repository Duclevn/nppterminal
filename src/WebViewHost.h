#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <cstdint>

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace nppterminal {

class WebViewHost final {
public:
    using MessageCallback = std::function<void(const std::wstring&)>;
    using NavigationCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::wstring&)>;

    WebViewHost();
    ~WebViewHost();

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    bool create(HWND parent, HMODULE module, MessageCallback messageCallback,
        NavigationCallback navigationCallback, ErrorCallback errorCallback);
    void close();
    void resize(const RECT& bounds);
    bool postJson(const std::wstring& json);
    bool isReady() const;

#ifdef NPPTERMINAL_TESTS
    using ScriptCallback = std::function<void(HRESULT, const std::wstring&)>;

    struct SecurityTestObservation final {
        std::uint32_t navigationCanceled = 0;
        std::uint32_t frameNavigationCanceled = 0;
        std::uint32_t newWindowHandled = 0;
        std::uint32_t permissionDenied = 0;
        std::uint32_t webResourceObserved = 0;
        std::uint32_t webResourceBlocked = 0;
        std::uint32_t downloadCanceled = 0;
    };

    // Test-only inspection seam.  The script is supplied by a fixture and is
    // never installed in production builds or persisted across navigation.
    bool executeScriptForTest(const std::wstring& script, ScriptCallback callback);
    bool closeAndWaitForCleanupForTest(DWORD timeoutMs = 15000);
    // Exercises the actual asynchronous cleanup observer state machine on the
    // test thread.  This is intentionally absent from the plugin ABI.
    static void runCleanupObserverTests();
    // Runs the real WebView2 environment/controller lifecycle against the
    // packaged test fixture.  Runtime/setup failures are reported as NOT_RUN.
    static void runCleanupIntegrationTests();
    // Causes the real controller callback to exercise the required-capability
    // failure path.  This exists only in the native test binary.
    void forceMissingWebView4ForTest();
    SecurityTestObservation securityObservationForTest() const;
    std::wstring userDataFolderForTest() const;
#endif

private:
    struct AsyncState;
    struct CleanupState;

    void onEnvironment(HRESULT result, ICoreWebView2Environment* environment);
    void onController(HRESULT result, ICoreWebView2Controller* controller);
    void onWebMessage(void* args);
    void onNavigationStarting(void* args);
    void onFrameNavigationStarting(void* args);
    void onNavigationCompleted(HRESULT result);
    void onProcessFailed(void* args);
    void onNewWindow(void* args);
    void onPermission(void* args);
    void onWebResource(void* args);
    void onDownload(void* args);
    void fail(const std::wstring& message);

    std::shared_ptr<AsyncState> asyncState_;
    std::shared_ptr<CleanupState> cleanupState_;
    HWND parent_ = nullptr;
    HMODULE module_ = nullptr;
    std::wstring webFolder_;
    std::wstring userDataFolder_;
    std::wstring profileName_;
    MessageCallback messageCallback_;
    NavigationCallback navigationCallback_;
    ErrorCallback errorCallback_;
    bool ready_ = false;
    RECT bounds_{};
    bool hasBounds_ = false;

#ifdef NPPTERMINAL_TESTS
    bool forceMissingWebView4ForTest_ = false;
    SecurityTestObservation securityObservation_{};
#endif

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nppterminal
