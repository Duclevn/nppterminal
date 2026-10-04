#include "SettingsDialog.h"

#include "SettingsDialogResource.h"

#include <array>
#include <string>
#include <utility>

namespace nppterminal::settings {

namespace {

struct ShellChoice final {
    const wchar_t* id;
    const wchar_t* label;
};

constexpr std::array<ShellChoice, 4> kShellChoices{{
    {L"powershell7", L"PowerShell 7"},
    {L"cmd", L"Command Prompt"},
    {L"gitbash", L"Git Bash"},
    {L"wsl", L"WSL"},
}};

struct DialogState final {
    Settings value;
    Settings& updated;
    SaveCallback saveCallback;
    bool accepted = false;
};

void setForm(HWND dialog, const Settings& value)
{
    const HWND shell = ::GetDlgItem(dialog, IDC_SETTINGS_SHELL);
    ::SendMessageW(shell, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (int index = 0; index != static_cast<int>(kShellChoices.size()); ++index) {
        ::SendMessageW(shell, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(kShellChoices[static_cast<std::size_t>(index)].label));
        if (value.defaultShell == kShellChoices[static_cast<std::size_t>(index)].id) {
            selected = index;
        }
    }
    ::SendMessageW(shell, CB_SETCURSEL, selected, 0);
    ::SetDlgItemTextW(dialog, IDC_SETTINGS_DIRECTORY, value.defaultDirectory.c_str());
    ::SetDlgItemTextW(dialog, IDC_SETTINGS_FONT_FAMILY, value.fontFamily.c_str());
    ::SetDlgItemInt(dialog, IDC_SETTINGS_FONT_SIZE, static_cast<UINT>(value.fontSize), FALSE);
    ::SetDlgItemInt(dialog, IDC_SETTINGS_SCROLLBACK, static_cast<UINT>(value.scrollback), FALSE);
    ::CheckDlgButton(dialog, IDC_SETTINGS_CONFIRM_KILL,
        value.confirmBeforeKill ? BST_CHECKED : BST_UNCHECKED);
}

bool readEdit(HWND dialog, int controlId, std::wstring& value, std::wstring& error)
{
    const int length = ::GetWindowTextLengthW(::GetDlgItem(dialog, controlId));
    if (length < 0 || static_cast<std::size_t>(length) > kMaxStringCharacters) {
        error = L"A settings text field is too long.";
        return false;
    }
    value.assign(static_cast<std::size_t>(length) + 1, L'\0');
    if (length != 0 && static_cast<int>(::GetDlgItemTextW(
        dialog, controlId, value.data(), length + 1)) != length) {
        error = L"A settings text field could not be read.";
        return false;
    }
    value.resize(static_cast<std::size_t>(length));
    return true;
}

bool readForm(HWND dialog, Settings& value, std::wstring& error)
{
    const HWND shell = ::GetDlgItem(dialog, IDC_SETTINGS_SHELL);
    const LRESULT selected = ::SendMessageW(shell, CB_GETCURSEL, 0, 0);
    if (selected < 0 || selected >= static_cast<LRESULT>(kShellChoices.size())) {
        error = L"Choose a default shell.";
        return false;
    }
    value.defaultShell = kShellChoices[static_cast<std::size_t>(selected)].id;
    if (!readEdit(dialog, IDC_SETTINGS_DIRECTORY, value.defaultDirectory, error) ||
        !readEdit(dialog, IDC_SETTINGS_FONT_FAMILY, value.fontFamily, error)) return false;

    BOOL translated = FALSE;
    const UINT fontSize = ::GetDlgItemInt(dialog, IDC_SETTINGS_FONT_SIZE, &translated, FALSE);
    if (!translated || fontSize < static_cast<UINT>(kMinFontSize) ||
        fontSize > static_cast<UINT>(kMaxFontSize)) {
        error = L"Font size must be between 6 and 48.";
        return false;
    }
    value.fontSize = static_cast<int>(fontSize);
    translated = FALSE;
    const UINT scrollback = ::GetDlgItemInt(dialog, IDC_SETTINGS_SCROLLBACK, &translated, FALSE);
    if (!translated || scrollback > static_cast<UINT>(kMaxScrollback)) {
        error = L"Scrollback must be between 0 and 20000 lines.";
        return false;
    }
    value.scrollback = static_cast<int>(scrollback);
    value.confirmBeforeKill = ::IsDlgButtonChecked(dialog, IDC_SETTINGS_CONFIRM_KILL) == BST_CHECKED;
    return validateSettings(value, error);
}

void showError(HWND dialog, const std::wstring& error)
{
    ::MessageBoxW(dialog, error.empty() ? L"Unable to save settings." : error.c_str(),
        L"NppTerminal Settings", MB_OK | MB_ICONERROR);
}

INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<DialogState*>(lParam);
        ::SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        Settings initial = state->value;
        std::wstring ignored;
        if (!validateSettings(initial, ignored)) initial = Settings{};
        setForm(dialog, initial);
        return TRUE;
    }
    if (!state) return FALSE;

    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            Settings candidate = state->value;
            std::wstring error;
            if (!readForm(dialog, candidate, error)) {
                showError(dialog, error);
                return TRUE;
            }
            if (state->saveCallback && !state->saveCallback(candidate, error)) {
                showError(dialog, error);
                return TRUE;
            }
            state->updated = candidate;
            state->accepted = true;
            ::EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            ::EndDialog(dialog, IDCANCEL);
            return TRUE;
        case IDC_SETTINGS_RESET:
            setForm(dialog, Settings{});
            return TRUE;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        ::EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

} // namespace

bool showSettingsDialog(HMODULE module, HWND parent, const Settings& current,
    Settings& updated, SaveCallback saveCallback)
{
    if (!module) return false;
    DialogState state{current, updated, std::move(saveCallback)};
    const INT_PTR result = ::DialogBoxParamW(module,
        MAKEINTRESOURCEW(IDD_SETTINGS_DIALOG), parent, &dialogProc,
        reinterpret_cast<LPARAM>(&state));
    return result == IDOK && state.accepted;
}

} // namespace nppterminal::settings
