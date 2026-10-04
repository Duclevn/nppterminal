#include "Settings.Tests.h"

#include "Settings.h"

#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace nppterminal::tests {

namespace {

using settings::Settings;

struct DirectoryIdentity final {
    DWORD volumeSerialNumber = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;
};

[[noreturn]] void fail(const char* message)
{
    throw std::runtime_error(message);
}

void require(bool condition, const char* message)
{
    if (!condition) fail(message);
}

std::wstring tempDirectory()
{
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (length == 0 || length >= std::size(tempPath)) fail("GetTempPathW failed");
    std::wstring result(tempPath, length);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

std::wstring makeFixtureRoot(const std::wstring& parent)
{
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) fail("CoCreateGuid failed");
    wchar_t guidText[64] = {};
    if (::StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) == 0) {
        fail("StringFromGUID2 failed");
    }
    const std::wstring separator = !parent.empty() &&
        (parent.back() == L'\\' || parent.back() == L'/') ? L"" : L"\\";
    const std::wstring root = parent + separator + L"NppTerminal.Settings.Tests-" + guidText;
    if (!::CreateDirectoryW(root.c_str(), nullptr)) fail("CreateDirectoryW failed");
    return root;
}

bool readDirectoryIdentity(HANDLE directory, DirectoryIdentity& identity)
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(directory, &information)) return false;
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    identity.volumeSerialNumber = information.dwVolumeSerialNumber;
    identity.fileIndexHigh = information.nFileIndexHigh;
    identity.fileIndexLow = information.nFileIndexLow;
    return true;
}

bool sameDirectoryIdentity(const DirectoryIdentity& left, const DirectoryIdentity& right)
{
    return left.volumeSerialNumber == right.volumeSerialNumber &&
        left.fileIndexHigh == right.fileIndexHigh && left.fileIndexLow == right.fileIndexLow;
}

HANDLE openFixtureRoot(const std::wstring& root, DirectoryIdentity& identity)
{
    const HANDLE directory = ::CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE || !readDirectoryIdentity(directory, identity)) {
        if (directory != INVALID_HANDLE_VALUE) ::CloseHandle(directory);
        return INVALID_HANDLE_VALUE;
    }
    return directory;
}

struct Fixture final {
    const std::wstring parent;
    const std::wstring root;
    const std::wstring path;
    HANDLE rootHandle = INVALID_HANDLE_VALUE;
    DirectoryIdentity rootIdentity{};

    Fixture()
        : parent(tempDirectory())
        , root(makeFixtureRoot(parent))
        , path(root + L"\\settings.json")
    {
        rootHandle = openFixtureRoot(root, rootIdentity);
        if (rootHandle == INVALID_HANDLE_VALUE) {
            fail("could not retain the settings fixture directory");
        }
    }

    ~Fixture()
    {
        const std::filesystem::path parentPath(parent);
        const std::filesystem::path rootPath(root);
        const std::filesystem::path filePath(path);
        const bool owned = rootPath.parent_path() == parentPath &&
            rootPath.filename().wstring().rfind(L"NppTerminal.Settings.Tests-", 0) == 0 &&
            filePath.parent_path() == rootPath && filePath.filename() == L"settings.json";
        bool safe = owned && rootHandle != INVALID_HANDLE_VALUE;
        if (safe) {
            const DWORD rootAttributes = ::GetFileAttributesW(root.c_str());
            safe = rootAttributes != INVALID_FILE_ATTRIBUTES &&
                (rootAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        }
        if (safe) {
            DirectoryIdentity currentIdentity{};
            const HANDLE currentRoot = openFixtureRoot(root, currentIdentity);
            safe = currentRoot != INVALID_HANDLE_VALUE &&
                sameDirectoryIdentity(rootIdentity, currentIdentity);
            if (currentRoot != INVALID_HANDLE_VALUE) ::CloseHandle(currentRoot);
        }
        if (safe) {
            const DWORD fileAttributes = ::GetFileAttributesW(path.c_str());
            if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD lastError = ::GetLastError();
                safe = lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND;
            } else {
                safe = (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                    (fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
                if (safe) safe = ::DeleteFileW(path.c_str()) != FALSE;
            }
        }
        if (rootHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(rootHandle);
            rootHandle = INVALID_HANDLE_VALUE;
        }
        if (safe) ::RemoveDirectoryW(root.c_str());
    }
};

bool writeRaw(const std::wstring& path, const std::string& bytes)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr) != FALSE && written == bytes.size() &&
        ::FlushFileBuffers(file) != FALSE;
    ::CloseHandle(file);
    return success;
}

bool readRaw(const std::wstring& path, std::string& bytes)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool success = ::GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
        size.QuadPart <= 1024 * 1024;
    if (success) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        success = bytes.empty() || (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
            &read, nullptr) != FALSE && read == bytes.size());
    }
    ::CloseHandle(file);
    return success;
}

bool sameSettings(const Settings& left, const Settings& right)
{
    return left.defaultShell == right.defaultShell &&
        left.defaultDirectory == right.defaultDirectory &&
        left.fontFamily == right.fontFamily && left.fontSize == right.fontSize &&
        left.scrollback == right.scrollback &&
        left.confirmBeforeKill == right.confirmBeforeKill;
}

void testMissingDefaults()
{
    Fixture fixture;
    const settings::SettingsLoadResult result = settings::loadSettings(fixture.path);
    require(!result.malformed && result.error.empty(), "missing settings file was not a clean default");
    require(sameSettings(result.value, Settings{}), "missing settings file did not use defaults");
}

void testInvalidFieldsUseDefaults()
{
    Fixture fixture;
    require(writeRaw(fixture.path,
        R"({"defaultShell":"zsh","defaultDirectory":42,"fontFamily":"","fontSize":5,"scrollback":30000,"confirmBeforeKill":"yes"})"),
        "could not write invalid-field fixture");
    const settings::SettingsLoadResult result = settings::loadSettings(fixture.path);
    require(!result.malformed, "invalid fields made a valid JSON object malformed");
    require(sameSettings(result.value, Settings{}), "invalid fields were not replaced by defaults");
}

void testMalformedPreservedUntilSave()
{
    Fixture fixture;
    const std::string malformed = "{\"fontSize\": ";
    require(writeRaw(fixture.path, malformed), "could not write malformed fixture");
    std::string before;
    require(readRaw(fixture.path, before) && before == malformed, "malformed fixture was not written");
    const settings::SettingsLoadResult result = settings::loadSettings(fixture.path);
    require(result.malformed && !result.error.empty(), "malformed JSON was not reported");
    std::string afterLoad;
    require(readRaw(fixture.path, afterLoad) && afterLoad == before,
        "loading malformed JSON modified the original file");
    std::wstring error;
    require(settings::saveSettings(fixture.path, Settings{}, error),
        "explicit save could not replace malformed settings");
    const settings::SettingsLoadResult saved = settings::loadSettings(fixture.path);
    require(!saved.malformed && sameSettings(saved.value, Settings{}),
        "explicit save did not replace malformed settings");
}

void testUnicodeRoundTrip()
{
    Fixture fixture;
    Settings expected;
    expected.defaultShell = L"wsl";
    expected.defaultDirectory = L"C:\\工作\\终端";
    expected.fontFamily = L"霞鹜等宽";
    expected.fontSize = 19;
    expected.scrollback = 0;
    expected.confirmBeforeKill = true;
    std::wstring error;
    require(settings::saveSettings(fixture.path, expected, error),
        "Unicode settings could not be saved");
    const settings::SettingsLoadResult result = settings::loadSettings(fixture.path);
    require(!result.malformed && sameSettings(result.value, expected),
        "Unicode settings did not round-trip");
}

void testAtomicSaveAndLimits()
{
    Fixture fixture;
    std::wstring error;
    require(settings::saveSettings(fixture.path, Settings{}, error),
        "initial atomic settings save failed");
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.root)) {
        ++entries;
        require(entry.path().filename() == L"settings.json",
            "atomic save left a temporary file in the settings directory");
    }
    require(entries == 1, "atomic save did not leave exactly one settings file");
    std::string before;
    require(readRaw(fixture.path, before), "could not read initial atomic settings file");

    Settings tooLong = Settings{};
    tooLong.defaultDirectory.assign(settings::kMaxStringCharacters + 1, L'x');
    require(!settings::saveSettings(fixture.path, tooLong, error) && !error.empty(),
        "oversized setting was accepted for save");
    std::string afterRejectedSave;
    require(readRaw(fixture.path, afterRejectedSave) && afterRejectedSave == before,
        "rejected settings save modified the existing file");

    const std::string oversized(settings::kMaxFileBytes + 1, 'x');
    require(writeRaw(fixture.path, oversized), "could not write oversized settings fixture");
    const settings::SettingsLoadResult result = settings::loadSettings(fixture.path);
    require(result.malformed && !result.error.empty(), "oversized settings file was accepted");
    std::string afterOversizedLoad;
    require(readRaw(fixture.path, afterOversizedLoad) && afterOversizedLoad == oversized,
        "loading oversized settings modified the original file");
}

} // namespace

void runSettingsTests()
{
    testMissingDefaults();
    testInvalidFieldsUseDefaults();
    testMalformedPreservedUntilSave();
    testUnicodeRoundTrip();
    testAtomicSaveAndLimits();
}

} // namespace nppterminal::tests
