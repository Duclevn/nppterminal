#pragma once

#include <cstddef>
#include <string>

namespace nppterminal::settings {

inline constexpr wchar_t kDefaultShell[] = L"powershell7";
inline constexpr wchar_t kDefaultFontFamily[] = L"Cascadia Mono, Consolas, monospace";

inline constexpr int kMinFontSize = 6;
inline constexpr int kMaxFontSize = 48;
inline constexpr int kMinScrollback = 0;
inline constexpr int kMaxScrollback = 20000;
inline constexpr std::size_t kMaxStringCharacters = 4096;
inline constexpr std::size_t kMaxFontFamilyCharacters = 256;
inline constexpr std::size_t kMaxFileBytes = 64u * 1024u;

struct Settings final {
    std::wstring defaultShell = kDefaultShell;
    std::wstring defaultDirectory;
    std::wstring fontFamily = kDefaultFontFamily;
    int fontSize = 13;
    int scrollback = 5000;
    bool confirmBeforeKill = false;
};

struct SettingsLoadResult final {
    Settings value;
    bool malformed = false;
    std::wstring error;
};

// Validates values that are about to be persisted or applied by the dialog.
// Loading is intentionally more tolerant: invalid individual JSON fields are
// replaced with their corresponding defaults.
bool validateSettings(const Settings& value, std::wstring& error);

SettingsLoadResult loadSettings(const std::wstring& path);

// Writes only to the caller-supplied path.  The replacement is prepared in an
// exclusive random temporary file in the same directory and flushed before
// the target is replaced.
bool saveSettings(const std::wstring& path, const Settings& value,
    std::wstring& error);

} // namespace nppterminal::settings
