#include "PluginDefinition.h"

#include "TerminalPanel.h"
#include "menuCmdID.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

FuncItem funcItem[nbFunc]{};
NppData nppData{};

namespace {

HMODULE g_module = nullptr;
// Deliberately raw: a static smart pointer would run its destructor from
// DllMain during an abnormal unload. The normal NPPN_SHUTDOWN path owns the
// only delete; process teardown reclaims the remaining OS resources.
nppterminal::TerminalPanel* g_panel = nullptr;
ShortcutKey g_toggleShortcut{true, true, false, 'T'};

constexpr wchar_t kAuthorWebsite[] = L"https://ducle.uk";

void openAuthorWebsite(HWND dialog)
{
    (void)::ShellExecuteW(dialog, L"open", kAuthorWebsite, nullptr, nullptr, SW_SHOWNORMAL);
}

INT_PTR CALLBACK aboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            ::EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
        if (notification && notification->idFrom == IDC_ABOUT_WEBSITE &&
            (notification->code == NM_CLICK || notification->code == NM_RETURN)) {
            openAuthorWebsite(dialog);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        ::EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

} // namespace

void pluginInit(HANDLE module)
{
    g_module = static_cast<HMODULE>(module);
}

void pluginCleanUp()
{
    // DllMain must not stop workers or enter COM. Notepad++ sends NPPN_SHUTDOWN
    // on its UI thread, where commandMenuCleanUp owns the panel teardown.
}

bool setCommand(std::size_t index, const wchar_t* name, PFUNCPLUGINCMD function,
    ShortcutKey* shortcut, bool checkOnInit)
{
    if (index >= nbFunc || !name || !function) return false;
    lstrcpynW(funcItem[index]._itemName, name, menuItemSize);
    funcItem[index]._pFunc = function;
    funcItem[index]._pShKey = shortcut;
    funcItem[index]._init2Check = checkOnInit;
    return true;
}

void commandMenuInit()
{
    setCommand(0, L"Toggle Terminal", toggleTerminal, &g_toggleShortcut, false);
    setCommand(1, L"Terminal Settings...", terminalSettings, nullptr, false);
    setCommand(2, L"Open Terminal Here", openTerminalHere, nullptr, false);
    setCommand(3, L"About...", aboutPlugin, nullptr, false);
}

void commandMenuCleanUp()
{
    if (g_panel) {
        // The session owns only the bounded broker boundary. Host shutdown
        // therefore always releases the panel before the DLL returns.
        (void)g_panel->shutdownForHost();
        delete g_panel;
        g_panel = nullptr;
    }
}

void toggleTerminal()
{
    if (!g_panel) {
        g_panel = new nppterminal::TerminalPanel(g_module, nppData._nppHandle,
            funcItem[0]._cmdID);
    }
    g_panel->toggleByUser();
}

void terminalSettings()
{
    if (!g_panel) {
        g_panel = new nppterminal::TerminalPanel(g_module, nppData._nppHandle,
            funcItem[0]._cmdID);
    }
    g_panel->showSettings();
}

void openTerminalHere()
{
    if (!g_panel) {
        g_panel = new nppterminal::TerminalPanel(g_module, nppData._nppHandle,
            funcItem[0]._cmdID);
    }
    g_panel->openTerminalHere();
}

void aboutPlugin()
{
    if (!g_module) return;
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_LINK_CLASS};
    (void)::InitCommonControlsEx(&commonControls);
    (void)::DialogBoxParamW(g_module, MAKEINTRESOURCEW(IDD_ABOUT_DIALOG),
        nppData._nppHandle, &aboutDialogProc, 0);
}

void ensureTerminalPanel()
{
    if (!g_panel) {
        g_panel = new nppterminal::TerminalPanel(g_module, nppData._nppHandle,
            funcItem[0]._cmdID);
    }
    g_panel->createDock();
}

extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    nppData = data;
    commandMenuInit();
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return NPP_PLUGIN_NAME;
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count)
{
    if (count) *count = nbFunc;
    return funcItem;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notification)
{
    if (!notification) return;
    switch (notification->nmhdr.code) {
        case NPPN_READY:
            // Register the lightweight dock at startup so Notepad++ can restore
            // its saved position/visibility. The shell and WebView remain lazy.
            ensureTerminalPanel();
            break;
        case NPPN_SHUTDOWN:
            commandMenuCleanUp();
            break;
        case NPPN_BEFORESHUTDOWN:
            if (g_panel) g_panel->prepareForHostShutdown();
            break;
        case NPPN_CANCELSHUTDOWN:
            if (g_panel) g_panel->cancelHostShutdown();
            break;
        case NPPN_DARKMODECHANGED:
            if (g_panel) g_panel->onHostThemeChanged();
            break;
        default:
            break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM)
{
    return TRUE;
}

#ifdef UNICODE
extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}
#endif
