#include "Settings.h"

#include <windows.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace nppterminal::settings {

namespace {

using json = nlohmann::json;

constexpr char kUtf8Bom[] = "\xef\xbb\xbf";

std::wstring win32Error(DWORD code = ::GetLastError())
{
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

bool containsNull(const std::wstring& value)
{
    return value.find(L'\0') != std::wstring::npos;
}

bool isShellId(const std::wstring& value)
{
    return value == L"powershell7" || value == L"cmd" ||
        value == L"gitbash" || value == L"wsl";
}

bool isBoundedString(const std::wstring& value, bool allowEmpty)
{
    return (allowEmpty || !value.empty()) && value.size() <= kMaxStringCharacters &&
        !containsNull(value);
}

bool validFontFamily(const std::wstring& value)
{
    if (value.empty() || value.size() > kMaxFontFamilyCharacters) return false;
    for (wchar_t ch : value) {
        if (ch < 0x20 || ch == 0x7f || ch == L'{' || ch == L'}' ||
            ch == L';' || ch == L'<' || ch == L'>' || ch == L'`') return false;
    }
    return true;
}

bool wideToUtf8(const std::wstring& value, std::string& result, std::wstring& error)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = L"A settings string is too long.";
        return false;
    }
    if (value.empty()) {
        result.clear();
        return true;
    }
    const int sourceLength = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), sourceLength, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        error = L"A settings string is not valid Unicode.";
        return false;
    }
    result.assign(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
        result.data(), required, nullptr, nullptr) != required) {
        error = L"A settings string could not be encoded as UTF-8.";
        return false;
    }
    return true;
}

bool utf8ToWide(const std::string& value, std::wstring& result)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    if (value.empty()) {
        result.clear();
        return true;
    }
    const int sourceLength = static_cast<int>(value.size());
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), sourceLength, nullptr, 0);
    if (required <= 0) return false;
    result.assign(static_cast<std::size_t>(required), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength,
        result.data(), required) == required;
}

bool fullPath(const std::wstring& path, std::wstring& result, std::wstring& error)
{
    if (path.empty()) {
        error = L"The settings path is empty.";
        return false;
    }
    DWORD capacity = 512;
    for (;;) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD length = ::GetFullPathNameW(path.c_str(), capacity, buffer.data(), nullptr);
        if (length == 0) {
            error = L"Unable to resolve the settings path: " + win32Error();
            return false;
        }
        if (length < capacity - 1) {
            result.assign(buffer.data(), length);
            return true;
        }
        if (capacity >= 32768) {
            error = L"The settings path is too long.";
            return false;
        }
        capacity *= 2;
    }
}

bool readFile(const std::wstring& path, std::string& bytes, bool& missing, std::wstring& error)
{
    missing = false;
    bytes.clear();
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD lastError = ::GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
            missing = true;
            return true;
        }
        error = L"Unable to inspect the settings file: " + win32Error(lastError);
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = L"The settings path is a directory.";
        return false;
    }

    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Unable to open the settings file: " + win32Error();
        return false;
    }
    LARGE_INTEGER size{};
    bool success = ::GetFileSizeEx(file, &size) != FALSE;
    if (!success || size.QuadPart < 0 ||
        static_cast<ULONGLONG>(size.QuadPart) > kMaxFileBytes) {
        error = success ? L"The settings file is larger than the supported limit." :
            L"Unable to determine the settings file size: " + win32Error();
        ::CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!bytes.empty()) {
        success = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
            nullptr) != FALSE && read == bytes.size();
    }
    if (!success) error = L"Unable to read the settings file: " + win32Error();
    ::CloseHandle(file);
    return success;
}

bool getString(const json& object, const char* name, std::wstring& value)
{
    if (!object.contains(name) || !object[name].is_string()) return false;
    std::string encoded;
    try {
        encoded = object[name].get<std::string>();
    } catch (const json::exception&) {
        return false;
    }
    return utf8ToWide(encoded, value);
}

bool getInteger(const json& object, const char* name, std::int64_t& value)
{
    if (!object.contains(name)) return false;
    try {
        if (object[name].is_number_integer()) {
            value = object[name].get<std::int64_t>();
            return true;
        }
        if (object[name].is_number_unsigned()) {
            const auto unsignedValue = object[name].get<std::uint64_t>();
            if (unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            value = static_cast<std::int64_t>(unsignedValue);
            return true;
        }
    } catch (const json::exception&) {
        return false;
    }
    return false;
}

bool createTempPath(const std::wstring& parent, const std::wstring& targetName,
    std::wstring& result, std::wstring& error)
{
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) {
        error = L"Unable to allocate a temporary settings file name.";
        return false;
    }
    wchar_t guidText[64] = {};
    if (::StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) == 0) {
        error = L"Unable to format a temporary settings file name.";
        return false;
    }
    result = (std::filesystem::path(parent) /
        (targetName + L".tmp-" + guidText)).wstring();
    if (result.size() >= 32768) {
        error = L"The settings path is too long.";
        return false;
    }
    return true;
}

bool writeAtomic(const std::wstring& target, const std::string& bytes, std::wstring& error)
{
    const std::filesystem::path targetPath(target);
    const std::wstring parent = targetPath.parent_path().empty() ?
        std::filesystem::current_path().wstring() : targetPath.parent_path().wstring();
    const DWORD parentAttributes = ::GetFileAttributesW(parent.c_str());
    if (parentAttributes == INVALID_FILE_ATTRIBUTES ||
        (parentAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = L"The settings directory is unavailable.";
        return false;
    }

    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt != 4; ++attempt) {
        if (!createTempPath(parent, targetPath.filename().wstring(), temporary, error)) return false;
        file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (::GetLastError() != ERROR_FILE_EXISTS) {
            error = L"Unable to create the temporary settings file: " + win32Error();
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Unable to allocate an exclusive temporary settings file.";
        return false;
    }

    DWORD written = 0;
    bool success = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
        nullptr) != FALSE && written == bytes.size();
    if (success) success = ::FlushFileBuffers(file) != FALSE;
    const DWORD writeError = success ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(file);
    if (!success) {
        ::DeleteFileW(temporary.c_str());
        error = L"Unable to flush the temporary settings file: " + win32Error(writeError);
        return false;
    }

    const DWORD targetAttributes = ::GetFileAttributesW(target.c_str());
    if (targetAttributes != INVALID_FILE_ATTRIBUTES &&
        (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ::DeleteFileW(temporary.c_str());
        error = L"The settings path is a directory.";
        return false;
    }

    BOOL replaced = FALSE;
    if (targetAttributes != INVALID_FILE_ATTRIBUTES) {
        replaced = ::ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
            REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    } else {
        replaced = ::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    }
    if (!replaced) {
        const DWORD replaceError = ::GetLastError();
        ::DeleteFileW(temporary.c_str());
        error = L"Unable to replace the settings file: " + win32Error(replaceError);
        return false;
    }
    return true;
}

bool encodeSettings(const Settings& value, std::string& bytes, std::wstring& error)
{
    std::string shell;
    std::string directory;
    std::string font;
    if (!wideToUtf8(value.defaultShell, shell, error) ||
        !wideToUtf8(value.defaultDirectory, directory, error) ||
        !wideToUtf8(value.fontFamily, font, error)) return false;
    const json object = {
        {"defaultShell", shell},
        {"defaultDirectory", directory},
        {"fontFamily", font},
        {"fontSize", value.fontSize},
        {"scrollback", value.scrollback},
        {"confirmBeforeKill", value.confirmBeforeKill}
    };
    bytes = object.dump(2);
    bytes.push_back('\n');
    if (bytes.size() > kMaxFileBytes) {
        error = L"The serialized settings exceed the supported file limit.";
        return false;
    }
    return true;
}

} // namespace

bool validateSettings(const Settings& value, std::wstring& error)
{
    error.clear();
    if (!isShellId(value.defaultShell)) {
        error = L"Choose a supported default shell.";
        return false;
    }
    if (!isBoundedString(value.defaultDirectory, true)) {
        error = L"The default directory is empty or too long.";
        return false;
    }
    if (!validFontFamily(value.fontFamily)) {
        error = L"Use a font family of 1\u2013256 characters without control or CSS syntax characters.";
        return false;
    }
    if (value.fontSize < kMinFontSize || value.fontSize > kMaxFontSize) {
        error = L"Font size must be between 6 and 48.";
        return false;
    }
    if (value.scrollback < kMinScrollback || value.scrollback > kMaxScrollback) {
        error = L"Scrollback must be between 0 and 20000 lines.";
        return false;
    }
    return true;
}

SettingsLoadResult loadSettings(const std::wstring& path)
{
    SettingsLoadResult result;
    std::wstring resolvedPath;
    if (!fullPath(path, resolvedPath, result.error)) {
        result.malformed = true;
        return result;
    }
    std::string bytes;
    bool missing = false;
    if (!readFile(resolvedPath, bytes, missing, result.error)) {
        result.malformed = true;
        return result;
    }
    if (missing) return result;
    if (bytes.empty()) {
        result.malformed = true;
        result.error = L"The settings file is empty.";
        return result;
    }
    if (bytes.size() >= 3 && bytes.compare(0, 3, kUtf8Bom, 3) == 0) bytes.erase(0, 3);

    json object;
    try {
        object = json::parse(bytes);
    } catch (const json::exception&) {
        result.malformed = true;
        result.error = L"The settings file contains malformed JSON.";
        return result;
    }
    if (!object.is_object()) {
        result.malformed = true;
        result.error = L"The settings file must contain a JSON object.";
        return result;
    }

    Settings value;
    std::wstring stringValue;
    if (getString(object, "defaultShell", stringValue) && isShellId(stringValue)) {
        value.defaultShell = std::move(stringValue);
    }
    if (getString(object, "defaultDirectory", stringValue) &&
        isBoundedString(stringValue, true)) {
        value.defaultDirectory = std::move(stringValue);
    }
    if (getString(object, "fontFamily", stringValue) &&
        validFontFamily(stringValue)) {
        value.fontFamily = std::move(stringValue);
    }
    std::int64_t integerValue = 0;
    if (getInteger(object, "fontSize", integerValue) &&
        integerValue >= kMinFontSize && integerValue <= kMaxFontSize) {
        value.fontSize = static_cast<int>(integerValue);
    }
    if (getInteger(object, "scrollback", integerValue) &&
        integerValue >= kMinScrollback && integerValue <= kMaxScrollback) {
        value.scrollback = static_cast<int>(integerValue);
    }
    if (object.contains("confirmBeforeKill") && object["confirmBeforeKill"].is_boolean()) {
        value.confirmBeforeKill = object["confirmBeforeKill"].get<bool>();
    }
    result.value = std::move(value);
    return result;
}

bool saveSettings(const std::wstring& path, const Settings& value, std::wstring& error)
{
    error.clear();
    if (!validateSettings(value, error)) return false;
    std::string bytes;
    if (!encodeSettings(value, bytes, error)) return false;
    std::wstring resolvedPath;
    if (!fullPath(path, resolvedPath, error)) return false;
    return writeAtomic(resolvedPath, bytes, error);
}

} // namespace nppterminal::settings
