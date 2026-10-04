#include "WebViewSecurity.Tests.h"

#include "WebViewHost.h"

#include <windows.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace nppterminal::tests {

namespace {

using json = nlohmann::json;
using namespace nppterminal;

constexpr wchar_t kWindowClass[] = L"NppTerminalWebViewSecurityWindow-v1";
constexpr DWORD kStartupTimeoutMs = 15000;
constexpr DWORD kScriptTimeoutMs = 5000;
constexpr wchar_t kLoopbackHttp[] = L"http://127.0.0.1:1/nppterminal-security";
constexpr wchar_t kLoopbackHttps[] = L"https://127.0.0.1:1/nppterminal-security";

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error("WEBVIEW_SECURITY FAIL: " + message);
}

[[noreturn]] void notRun(const std::string& message)
{
    throw std::runtime_error("WEBVIEW_SECURITY NOT_RUN: " + message);
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

std::wstring moduleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(module, path,
        static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    std::wstring value(path, length);
    const std::size_t separator = value.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : value.substr(0, separator);
}

bool pathExists(const std::wstring& path)
{
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isRuntimeUnavailable(const std::wstring& message)
{
    return message.find(L"WebView2 runtime is unavailable") != std::wstring::npos ||
        message.find(L"WebView2 runtime initialization failed") != std::wstring::npos;
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

struct ScriptResult final {
    bool done = false;
    HRESULT result = E_FAIL;
    std::wstring text;
};

std::wstring runScript(WebViewHost& host, const std::wstring& script)
{
    auto result = std::make_shared<ScriptResult>();
    require(host.executeScriptForTest(script,
        [result](HRESULT status, const std::wstring& text) {
            result->result = status;
            result->text = text;
            result->done = true;
        }), "WebView rejected a security inspection script");
    require(waitUntil([&] { return result->done; }, kScriptTimeoutMs),
        "security inspection script did not complete");
    require(SUCCEEDED(result->result), "security inspection script failed");
    return result->text;
}

json readJson(WebViewHost& host, const std::wstring& script)
{
    const std::wstring text = runScript(host, script);
    try {
        return json::parse(narrow(text));
    } catch (const std::exception&) {
        fail("security inspection script returned invalid JSON: " + narrow(text));
    }
}

void dispatchScript(WebViewHost& host, const std::wstring& script)
{
    auto result = std::make_shared<ScriptResult>();
    require(host.executeScriptForTest(script,
        [result](HRESULT status, const std::wstring&) {
            result->result = status;
            result->done = true;
        }),
        "WebView rejected a security action script");
    require(waitUntil([&] { return result->done; }, kScriptTimeoutMs),
        "security action script did not complete");
    require(SUCCEEDED(result->result), "security action script failed");
}

json waitForProbe(WebViewHost& host, const std::wstring& script,
    const std::string& field, const std::string& pendingValue)
{
    json value;
    require(waitUntil([&] {
        value = readJson(host, script);
        return value.is_object() && value.contains(field) &&
            value[field].is_string() && value[field].get<std::string>() != pendingValue;
    }, kStartupTimeoutMs), "security policy probe did not settle");
    return value;
}

void verifyCapabilityFailure(HWND parent, HMODULE module)
{
    WebViewHost host;
    host.forceMissingWebView4ForTest();
    bool ready = false;
    std::wstring callbackError;
    const bool created = host.create(parent, module, [](const std::wstring&) {},
        [&] { ready = true; },
        [&](const std::wstring& message) { callbackError = message; });
    if (!created) {
        host.close();
        if (isRuntimeUnavailable(callbackError)) {
            notRun("WebView2 runtime startup failed: " + narrow(callbackError));
        }
        fail("capability fixture create failed: " + narrow(callbackError));
    }
    const std::wstring profileRoot = host.userDataFolderForTest();
    require(!profileRoot.empty(), "capability fixture did not create an owned profile");
    require(waitUntil([&] { return ready || !callbackError.empty(); }, kStartupTimeoutMs),
        "capability fixture did not report startup success or failure");
    if (isRuntimeUnavailable(callbackError)) {
        host.close();
        notRun("WebView2 runtime startup failed: " + narrow(callbackError));
    }
    require(!ready && !host.isReady(),
        "missing WebView2_4 capability reached navigation instead of failing startup");
    require(callbackError.find(L"too old") != std::wstring::npos &&
        callbackError.find(L"Evergreen WebView2 Runtime") != std::wstring::npos &&
        callbackError.find(L"retry") != std::wstring::npos,
        "missing capability error did not provide an actionable upgrade/retry message");
    require(host.closeAndWaitForCleanupForTest(),
        "missing capability fixture did not complete owned WebView cleanup");
    require(!pathExists(profileRoot),
        "missing capability fixture left its owned profile behind");
}

} // namespace

void runWebViewSecurityTests()
{
    const HMODULE module = ::GetModuleHandleW(nullptr);
    if (!module) notRun("the test executable module could not be resolved");
    const std::wstring directory = moduleDirectory(module);
    if (directory.empty() || !pathExists(directory + L"\\web")) {
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
    windowClass.hInstance = module;
    windowClass.lpszClassName = kWindowClass;
    const ATOM registered = ::RegisterClassW(&windowClass);
    const bool registeredHere = registered != 0;
    if (!registeredHere && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fail("failed to register the hidden WebView security window class");
    }
    const HWND parent = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"NppTerminal WebView security", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, module, nullptr);
    if (!parent) {
        if (registeredHere) ::UnregisterClassW(kWindowClass, module);
        fail("failed to create the hidden WebView security window");
    }
    WindowGuard windowGuard{parent, module, registeredHere};

    // This uses a real environment/controller callback.  The test-only seam
    // removes only the queried v4 interface immediately before the production
    // gate, so a v3-only runtime cannot accidentally pass through Navigate.
    verifyCapabilityFailure(parent, module);

    WebViewHost host;
    bool ready = false;
    std::wstring callbackError;
    if (!host.create(parent, module, [](const std::wstring&) {},
        [&] { ready = true; },
        [&](const std::wstring& message) { callbackError = message; })) {
        host.close();
        if (isRuntimeUnavailable(callbackError)) {
            notRun("WebView2 runtime startup failed: " + narrow(callbackError));
        }
        fail("WebViewHost::create failed: " + narrow(callbackError));
    }
    host.resize(RECT{0, 0, 1200, 800});
    require(waitUntil([&] { return ready || !callbackError.empty(); }, kStartupTimeoutMs),
        "WebView navigation did not complete");
    if (!callbackError.empty()) {
        if (isRuntimeUnavailable(callbackError)) {
            notRun("WebView2 runtime is unavailable: " + narrow(callbackError));
        }
        fail("WebView navigation failed: " + narrow(callbackError));
    }
    require(ready && host.isReady(), "WebViewHost did not report a ready shipping page");

    const json csp = readJson(host, LR"JS(
(() => {
  const value = document.querySelector('meta[http-equiv="Content-Security-Policy"]')?.content || '';
  return {
    defaultNone: value.includes("default-src 'none'"),
    connectNone: value.includes("connect-src 'none'"),
    frameNone: value.includes("frame-src 'none'")
  };
})()
)JS");
    require(csp.value("defaultNone", false) && csp.value("connectNone", false) &&
        csp.value("frameNone", false),
        "shipping page did not expose the required restrictive CSP");

    dispatchScript(host, LR"JS(
(() => {
  window.__nppterminalSecurityProbe = {
    network: 'pending', networkCsp: false,
    frame: 'pending', frameCsp: false,
    permission: 'pending'
  };
  window.addEventListener('securitypolicyviolation', event => {
    const blocked = String(event.blockedURI || '');
    if (!blocked.includes('127.0.0.1')) return;
    const directive = String(event.effectiveDirective || event.violatedDirective || '');
    if (directive === 'connect-src' || directive === 'default-src') {
      window.__nppterminalSecurityProbe.networkCsp = true;
    }
    if (directive === 'frame-src' || directive === 'default-src') {
      window.__nppterminalSecurityProbe.frameCsp = true;
    }
  });
  return true;
})()
)JS");

    // The pinned WebView2 documentation states that WebResourceRequested is
    // not raised for resources served through SetVirtualHostNameToFolderMapping.
    // Do not use the shipping page's mapped CSS/JS as evidence of direct event
    // coverage.  The network case below records the event only if its request
    // actually reaches the handler; CSP denial remains an independent proof.
    const auto beforeNavigation = host.securityObservationForTest();
    dispatchScript(host, std::wstring(L"location.href = '") + kLoopbackHttps + L"'; true;");
    require(waitUntil([&] {
        return host.securityObservationForTest().navigationCanceled >
            beforeNavigation.navigationCanceled;
    }, kScriptTimeoutMs), "off-origin top-level navigation was not canceled by the handler");
    const json location = readJson(host, LR"JS(
(() => ({ location: window.location.href }))()
)JS");
    require(location.value("location", "") == "https://nppterminal.invalid/index.html",
        "canceling off-origin navigation did not preserve the shipping page");

    const auto beforePopup = host.securityObservationForTest();
    dispatchScript(host, std::wstring(L"(() => { const url = '") + kLoopbackHttps +
        L"/popup'; const opened = window.open(url, '_blank'); if (!opened) { "
        L"const a = document.createElement('a'); a.href = url; a.target = '_blank'; "
        L"a.rel = 'noreferrer'; document.body.append(a); a.click(); a.remove(); } return true; })();");
    require(waitUntil([&] {
        return host.securityObservationForTest().newWindowHandled > beforePopup.newWindowHandled;
    }, kScriptTimeoutMs), "window.open/new-window request was not handled and denied");

    const auto beforeNetwork = host.securityObservationForTest();
    dispatchScript(host, std::wstring(L"(() => { const p = window.__nppterminalSecurityProbe;") +
        L"fetch('" + kLoopbackHttp + L"/network').then(() => p.network = 'loaded', "
        L"() => p.network = 'rejected'); return true; })();");
    const json network = waitForProbe(host,
        L"window.__nppterminalSecurityProbe || null", "network", "pending");
    require(network["network"].get<std::string>() == "rejected",
        "off-origin network request was not rejected");
    const auto afterNetwork = host.securityObservationForTest();
    const bool networkCsp = network.value("networkCsp", false);
    const bool networkHandler = afterNetwork.webResourceBlocked > beforeNetwork.webResourceBlocked;
    require(networkCsp || networkHandler,
        "network denial had neither CSP evidence nor WebResourceRequested handler coverage");

    const auto beforeFrame = host.securityObservationForTest();
    dispatchScript(host, std::wstring(L"(() => { const p = window.__nppterminalSecurityProbe;") +
        L"const f = document.createElement('iframe'); f.id = 'nppterminal-security-frame'; "
        L"f.onload = () => p.frame = 'loaded'; f.onerror = () => p.frame = 'error'; "
        L"f.src = '" + kLoopbackHttps + L"/frame'; document.body.append(f); "
        L"setTimeout(() => { if (p.frame === 'pending') p.frame = 'timeout'; }, 2000); return true; })();");
    const json frame = waitForProbe(host,
        L"window.__nppterminalSecurityProbe || null", "frame", "pending");
    const auto afterFrame = host.securityObservationForTest();
    const bool frameCsp = frame.value("frameCsp", false);
    const bool frameHandler = afterFrame.frameNavigationCanceled >
        beforeFrame.frameNavigationCanceled;
    require(frameCsp || frameHandler,
        "frame denial had neither CSP evidence nor FrameNavigationStarting handler coverage");
    dispatchScript(host, L"document.getElementById('nppterminal-security-frame')?.remove(); true;");

    const auto beforePermission = host.securityObservationForTest();
    dispatchScript(host, LR"JS(
(() => {
  const p = window.__nppterminalSecurityProbe;
  if (typeof Notification !== 'function') { p.permission = 'unsupported'; return true; }
  Notification.requestPermission().then(value => { p.permission = value; },
    () => { p.permission = 'rejected'; });
  return true;
})()
)JS");
    const json permission = waitForProbe(host,
        L"window.__nppterminalSecurityProbe || null", "permission", "pending");
    require(permission["permission"].get<std::string>() == "denied",
        "notification permission was not denied");
    require(host.securityObservationForTest().permissionDenied >
        beforePermission.permissionDenied,
        "notification denial did not reach PermissionRequested handler");

    const auto beforeDownload = host.securityObservationForTest();
    dispatchScript(host, LR"JS(
(() => {
  const blob = new Blob(['NppTerminal security fixture'], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'nppterminal-security-fixture.txt';
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  return true;
})()
)JS");
    require(waitUntil([&] {
        return host.securityObservationForTest().downloadCanceled >
            beforeDownload.downloadCanceled;
    }, kScriptTimeoutMs), "download was not canceled by DownloadStarting handler");

    const std::wstring profileRoot = host.userDataFolderForTest();
    require(!profileRoot.empty(), "security fixture did not expose its owned profile root");
    require(host.closeAndWaitForCleanupForTest(),
        "security fixture did not complete owned WebView cleanup");
    require(!pathExists(profileRoot),
        "security fixture left an owned profile or downloaded file behind");

    const auto final = host.securityObservationForTest();
    const std::string networkMode = networkCsp && networkHandler ? "CSP+handler" :
        networkCsp ? "CSP" : "handler";
    const std::string frameMode = frameCsp && frameHandler ? "CSP+handler" :
        frameCsp ? "CSP" : "handler";
    const std::string webResourceMode = final.webResourceObserved == 0
        ? "unobserved-virtual-host" : "observed";
    std::cout << "WEBVIEW_SECURITY PASS navigation=" << final.navigationCanceled
        << " popup=" << final.newWindowHandled
        << " network=" << networkMode
        << " frame=" << frameMode
        << " permissions=denied download=" << final.downloadCanceled
        << " web_resources=" << webResourceMode
        << " count=" << final.webResourceObserved << "\n";
}

} // namespace nppterminal::tests
