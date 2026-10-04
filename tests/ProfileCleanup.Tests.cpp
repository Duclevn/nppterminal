#include "ProfileCleanup.Tests.h"

#include "ProfileCleanup.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <objbase.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nppterminal::tests {

namespace {

using profilecleanup::CleanupPhase;
using profilecleanup::OwnedProfile;
using profilecleanup::ProcessIdentity;

constexpr DWORD kBrokerWaitMs = 10000;
constexpr DWORD kBrokerTerminateWaitMs = 2000;
std::wstring fixtureParent;

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
constexpr DWORD SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE = 0x2;
#endif

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

std::string win32Error(DWORD error = ::GetLastError())
{
    wchar_t buffer[512] = {};
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, buffer,
        static_cast<DWORD>(std::size(buffer)), nullptr);
    if (length == 0) return "Win32 error " + std::to_string(error);
    std::wstring message(buffer, length);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return narrow(message);
}

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message)
{
    if (!condition) fail(message);
}

std::wstring fullPath(const std::wstring& path)
{
    DWORD capacity = 512;
    for (;;) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD length = ::GetFullPathNameW(path.c_str(), capacity, buffer.data(), nullptr);
        if (length == 0) fail("GetFullPathNameW failed: " + win32Error());
        if (length < capacity - 1) return std::wstring(buffer.data(), length);
        if (capacity >= 32768) fail("Path exceeded the validation limit");
        capacity *= 2;
    }
}

bool samePath(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool isDirectChild(const std::wstring& path, const std::wstring& parent)
{
    const std::wstring child = fullPath(path);
    const std::wstring namedParent = fullPath(parent);
    const std::filesystem::path childPath(child);
    return !samePath(child, namedParent) &&
        samePath(childPath.parent_path().wstring(), namedParent);
}

bool pathExists(const std::wstring& path, bool directoryExpected = false)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return !directoryExpected || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void requirePathExists(const std::wstring& path, const std::string& description,
    bool directoryExpected = false)
{
    require(pathExists(path, directoryExpected), description + " is missing");
}

void requirePathAbsent(const std::wstring& path, const std::string& description)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    require(attributes == INVALID_FILE_ATTRIBUTES &&
        (::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND),
        description + " was not removed");
}

bool removeTreeEntry(const std::wstring& path, std::wstring& error)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD lastError = ::GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) return true;
        error = L"GetFileAttributesW failed: " + std::to_wstring(lastError);
        return false;
    }

    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ?
            ::RemoveDirectoryW(path.c_str()) : ::DeleteFileW(path.c_str());
        if (!removed) error = L"Unable to remove fixture reparse point: " +
            std::to_wstring(::GetLastError());
        return removed != FALSE;
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const std::wstring pattern = path + L"\\*";
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            bool success = true;
            do {
                const std::wstring name(data.cFileName);
                if (name == L"." || name == L"..") continue;
                if (!removeTreeEntry(path + L"\\" + name, error)) {
                    success = false;
                    break;
                }
            } while (::FindNextFileW(find, &data));
            const DWORD findError = ::GetLastError();
            ::FindClose(find);
            if (success && findError != ERROR_NO_MORE_FILES) {
                error = L"FindNextFileW failed: " + std::to_wstring(findError);
                success = false;
            }
            if (!success) return false;
        } else if (::GetLastError() != ERROR_FILE_NOT_FOUND &&
            ::GetLastError() != ERROR_PATH_NOT_FOUND) {
            error = L"FindFirstFileW failed: " + std::to_wstring(::GetLastError());
            return false;
        }
        if (!::RemoveDirectoryW(path.c_str())) {
            error = L"RemoveDirectoryW failed: " + std::to_wstring(::GetLastError());
            return false;
        }
        return true;
    }

    (void)::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!::DeleteFileW(path.c_str())) {
        error = L"DeleteFileW failed: " + std::to_wstring(::GetLastError());
        return false;
    }
    return true;
}

class FixtureSet final {
public:
    struct Root final {
        std::wstring path;
        std::wstring parent;
    };

    ~FixtureSet()
    {
        for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
            std::wstring error;
            if (!removeVerifiedRoot(*it, error)) {
                std::wcerr << L"PROFILE_CLEANUP FIXTURE_LEAK " << it->path << L": " <<
                    error << L"\n";
            }
        }
    }

    void trackRoot(const std::wstring& path, const std::wstring& parent)
    {
        require(isDirectChild(path, parent), "fixture root is not a direct child of its named parent");
        roots_.push_back(Root{path, parent});
    }

    void renameTrackedRoot(const std::wstring& oldPath, const std::wstring& newPath,
        const std::wstring& parent)
    {
        require(isDirectChild(newPath, parent), "renamed fixture root escaped its named parent");
        for (Root& root : roots_) {
            if (samePath(root.path, oldPath)) {
                root.path = newPath;
                root.parent = parent;
                return;
            }
        }
        fail("renamed fixture root was not tracked");
    }

private:
    static bool removeVerifiedRoot(const Root& root, std::wstring& error)
    {
        if (!isDirectChild(root.path, root.parent)) {
            error = L"fixture root failed the exact-parent check";
            return false;
        }
        return removeTreeEntry(root.path, error);
    }

    std::vector<Root> roots_;
};

struct ProfileLeaseGuard final {
    explicit ProfileLeaseGuard(OwnedProfile& profileValue) : profile(&profileValue) {}
    ~ProfileLeaseGuard()
    {
        release();
    }

    void release()
    {
        if (profile) {
            profilecleanup::releaseOwnedProfileLease(*profile);
            profile = nullptr;
        }
    }

    OwnedProfile* profile;
};

struct LockedFileGuard final {
    HANDLE handle = nullptr;

    ~LockedFileGuard()
    {
        release();
    }

    void release()
    {
        if (handle) {
            ::CloseHandle(handle);
            handle = nullptr;
        }
    }
};

std::wstring localAppDataRoot()
{
    PWSTR value = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
        nullptr, &value);
    if (FAILED(result) || !value) {
        if (value) ::CoTaskMemFree(value);
        fail("SHGetKnownFolderPath(LocalAppData) failed");
    }
    std::wstring localData(value);
    ::CoTaskMemFree(value);
    require(!localData.empty(), "LocalAppData path was empty");
    return fullPath(localData);
}

std::wstring localAppDataProfileParent()
{
    const std::wstring localData = localAppDataRoot();
    return fullPath(localData + L"\\NppTerminal\\WebView");
}

std::wstring uniqueGuidToken(const std::wstring& prefix)
{
    GUID id{};
    require(SUCCEEDED(::CoCreateGuid(&id)), "CoCreateGuid failed");
    wchar_t text[64] = {};
    require(::StringFromGUID2(id, text, static_cast<int>(std::size(text))) > 0,
        "StringFromGUID2 failed");
    std::wstring guid(text);
    if (guid.size() >= 2 && guid.front() == L'{' && guid.back() == L'}') {
        guid = guid.substr(1, guid.size() - 2);
    }
    return prefix + guid;
}

void createProfile(const std::wstring& parent, FixtureSet& fixtures, OwnedProfile& profile)
{
    ProcessIdentity host;
    require(profilecleanup::queryProcessIdentity(::GetCurrentProcessId(), host),
        "queryProcessIdentity(current process) failed");
    std::wstring error;
    require(profilecleanup::createOwnedProfile(parent, host, profile, error),
        "createOwnedProfile failed: " + narrow(error));
    fixtures.trackRoot(profile.rootPath, parent);
}

void releaseProfileAs(OwnedProfile& profile, ProfileLeaseGuard& lease, CleanupPhase phase,
    bool processSnapshotComplete)
{
    std::wstring error;
    require(profilecleanup::updateOwnedProfile(profile, phase, {}, processSnapshotComplete, error),
        "updateOwnedProfile failed: " + narrow(error));
    lease.release();
}

void writeFixtureFile(const std::wstring& path, const std::string& contents)
{
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE,
        "CreateFileW fixture failed: " + win32Error());
    DWORD written = 0;
    const BOOL result = ::WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
        &written, nullptr);
    const BOOL flushed = result && written == static_cast<DWORD>(contents.size()) &&
        ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    require(flushed != FALSE, "WriteFile fixture failed: " + win32Error());
}

std::string readFixtureFile(const std::wstring& path)
{
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "unable to open fixture sentinel: " + win32Error());
    LARGE_INTEGER size{};
    require(::GetFileSizeEx(handle, &size) != FALSE && size.QuadPart >= 0 &&
        size.QuadPart <= 1024 * 1024, "invalid fixture sentinel size");
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL result = contents.empty() || ::ReadFile(handle, contents.data(),
        static_cast<DWORD>(contents.size()), &read, nullptr);
    ::CloseHandle(handle);
    require(result != FALSE && read == static_cast<DWORD>(contents.size()),
        "ReadFile fixture failed");
    return contents;
}

std::wstring nonceHex(const std::array<std::uint8_t, profilecleanup::kNonceBytes>& nonce)
{
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text;
    text.reserve(nonce.size() * 2);
    for (const std::uint8_t value : nonce) {
        text.push_back(digits[value >> 4]);
        text.push_back(digits[value & 0x0fu]);
    }
    return text;
}

std::wstring moduleDirectory()
{
    DWORD capacity = 512;
    for (;;) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (length == 0) fail("GetModuleFileNameW failed: " + win32Error());
        if (length < capacity - 1) {
            return fullPath(std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().wstring());
        }
        if (capacity >= 32768) fail("test executable path exceeded the validation limit");
        capacity *= 2;
    }
}

std::wstring quoteArgument(const std::wstring& argument)
{
    std::wstring result(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

struct BrokerResult final {
    DWORD processId = 0;
    DWORD exitCode = STILL_ACTIVE;
    bool timedOut = false;
};

struct BrokerProcess final {
    HANDLE process = nullptr;
    DWORD processId = 0;

    BrokerProcess() = default;
    BrokerProcess(const BrokerProcess&) = delete;
    BrokerProcess& operator=(const BrokerProcess&) = delete;

    BrokerProcess(BrokerProcess&& other) noexcept
        : process(other.process), processId(other.processId)
    {
        other.process = nullptr;
        other.processId = 0;
    }

    BrokerProcess& operator=(BrokerProcess&& other) noexcept
    {
        if (this != &other) {
            terminateAndClose();
            process = other.process;
            processId = other.processId;
            other.process = nullptr;
            other.processId = 0;
        }
        return *this;
    }

    ~BrokerProcess()
    {
        terminateAndClose();
    }

    void terminateAndClose() noexcept
    {
        if (!process) return;
        DWORD exitCode = STILL_ACTIVE;
        if (::GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE) {
            (void)::TerminateProcess(process, ERROR_TIMEOUT);
        }
        (void)::WaitForSingleObject(process, kBrokerTerminateWaitMs);
        ::CloseHandle(process);
        process = nullptr;
        processId = 0;
    }

    void close()
    {
        if (process) ::CloseHandle(process);
        process = nullptr;
        processId = 0;
    }
};

std::string brokerResultText(const BrokerResult& result)
{
    return "exitCode=" + std::to_string(result.exitCode) +
        ", timedOut=" + (result.timedOut ? "true" : "false") +
        ", ownedPid=" + std::to_string(result.processId);
}

std::string fixturePathText(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return narrow(path) + " [missing, error=" + std::to_string(::GetLastError()) + "]";
    }
    std::ostringstream attributesText;
    attributesText << std::hex << static_cast<unsigned long>(attributes);
    return narrow(path) + " [attributes=0x" + attributesText.str() + "]";
}

std::string diagnoseCleanupFailure(const std::wstring& parent, const OwnedProfile& profile,
    const BrokerResult& broker)
{
    if (!broker.timedOut && broker.exitCode == 0) return {};
#ifdef NPPTERMINAL_TESTS
    std::wstring directError;
    const int directResult = profilecleanup::cleanupProfileForTest(parent,
        profile.instanceId, profile.nonce, directError);
    return "; directResult=" + std::to_string(directResult) +
        ", directError=" + (directError.empty() ? "<empty>" : narrow(directError));
#else
    return "; directResult=<test-api-unavailable>";
#endif
}

BrokerProcess startBroker(const std::vector<std::wstring>& arguments)
{
    const std::wstring module = moduleDirectory();
    const std::wstring helper = module + L"\\NppTerminalBroker.Tests.exe";
    const DWORD helperAttributes = ::GetFileAttributesW(helper.c_str());
    require(helperAttributes != INVALID_FILE_ATTRIBUTES &&
        (helperAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (helperAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
        "adjacent NppTerminalBroker.Tests.exe is unavailable");

    std::wstring command = quoteArgument(helper);
    require(!fixtureParent.empty(), "isolated cleanup parent was not established");
    command += L" --cleanup-test-parent " + quoteArgument(fixtureParent);
    for (const std::wstring& argument : arguments) {
        command += L" ";
        command += quoteArgument(argument);
    }
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = ::CreateProcessW(helper.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, module.c_str(), &startup, &processInfo);
    require(created != FALSE, "CreateProcessW broker failed: " + win32Error());
    ::CloseHandle(processInfo.hThread);

    BrokerProcess child;
    child.process = processInfo.hProcess;
    child.processId = processInfo.dwProcessId;
    return child;
}

BrokerResult waitBroker(BrokerProcess& child)
{
    require(child.process != nullptr, "waitBroker received an empty broker process");
    BrokerResult result;
    result.processId = child.processId;
    const DWORD waitResult = ::WaitForSingleObject(child.process, kBrokerWaitMs);
    if (waitResult == WAIT_TIMEOUT) {
        result.timedOut = true;
        if (!::TerminateProcess(child.process, ERROR_TIMEOUT)) {
            DWORD exitCode = STILL_ACTIVE;
            if (!::GetExitCodeProcess(child.process, &exitCode) ||
                exitCode == STILL_ACTIVE) {
                const std::string message = "TerminateProcess(broker PID " +
                    std::to_string(result.processId) + ") failed: " + win32Error();
                fail(message);
            }
        }
        require(::WaitForSingleObject(child.process, kBrokerTerminateWaitMs) == WAIT_OBJECT_0,
            "owned broker PID " + std::to_string(result.processId) + " did not terminate");
    } else {
        require(waitResult == WAIT_OBJECT_0, "WaitForSingleObject broker failed: " + win32Error());
    }
    require(::GetExitCodeProcess(child.process, &result.exitCode) != FALSE,
        "GetExitCodeProcess broker failed: " + win32Error());
    child.close();
    return result;
}

BrokerResult runBroker(const std::vector<std::wstring>& arguments)
{
    BrokerProcess child = startBroker(arguments);
    return waitBroker(child);
}

BrokerResult runCleanup(const OwnedProfile& profile,
    const std::array<std::uint8_t, profilecleanup::kNonceBytes>& nonce)
{
    return runBroker({L"--cleanup", profile.instanceId, nonceHex(nonce)});
}

BrokerResult runRecovery()
{
    return runBroker({L"--cleanup-recover"});
}

std::vector<std::wstring> recoveryCandidates(const std::wstring& parent)
{
    std::vector<std::wstring> result;
    const std::wstring pattern = parent + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
            "FindFirstFileW recovery candidate scan failed: " + std::to_string(error));
        return result;
    }
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        if (name.rfind(L"instance-v1-", 0) == 0 ||
            name.rfind(L"cleanup-v1-", 0) == 0) {
            result.push_back(name);
        }
    } while (::FindNextFileW(find, &data));
    const DWORD error = ::GetLastError();
    ::FindClose(find);
    require(error == ERROR_NO_MORE_FILES, "FindNextFileW recovery candidate scan failed: " +
        std::to_string(error));
    return result;
}

void requireRecoveryBaselineUntouched(const std::wstring& parent,
    const std::vector<std::wstring>& baseline)
{
    for (const std::wstring& name : baseline) {
        requirePathExists(parent + L"\\" + name, "pre-existing recovery candidate");
    }
}

void createTrackedTarget(const std::wstring& namedParent, FixtureSet& fixtures,
    std::wstring& targetPath, std::wstring& sentinelPath)
{
    targetPath = namedParent + L"\\" + uniqueGuidToken(L"ProfileCleanupTarget-");
    require(::CreateDirectoryW(targetPath.c_str(), nullptr) != FALSE,
        "CreateDirectoryW target fixture failed: " + win32Error());
    fixtures.trackRoot(targetPath, namedParent);
    sentinelPath = targetPath + L"\\sentinel.txt";
    writeFixtureFile(sentinelPath, "profile-cleanup-target");
}

void testValidReleasedCleanup(const std::wstring& parent)
{
    FixtureSet fixtures;
    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring asset = profile.rootPath + L"\\fake-asset.bin";
    writeFixtureFile(asset, "owned fixture asset");
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);

    const BrokerResult result = runCleanup(profile, profile.nonce);
    require(!result.timedOut && result.exitCode == 0,
        "valid Released cleanup helper did not succeed (" + brokerResultText(result) +
        diagnoseCleanupFailure(parent, profile, result) +
        "; parent=" + narrow(parent) + "; root=" + fixturePathText(profile.rootPath) +
        "; owner=" + fixturePathText(profile.rootPath + L"\\" + profilecleanup::kOwnerMarkerName) +
        "; lease=" + fixturePathText(profile.rootPath + L"\\" + profilecleanup::kLeaseName) +
        "; asset=" + fixturePathText(asset) + ")");
    requirePathAbsent(profile.rootPath, "valid Released profile root");
    requirePathAbsent(asset, "valid Released profile asset");
    std::cout << "PROFILE_CLEANUP valid-released PASS\n";
}

void testWrongNonce(const std::wstring& parent)
{
    FixtureSet fixtures;
    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring asset = profile.rootPath + L"\\fake-asset.bin";
    writeFixtureFile(asset, "wrong nonce fixture");
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);

    auto wrongNonce = profile.nonce;
    wrongNonce[0] ^= 0x01u;
    const BrokerResult rejected = runCleanup(profile, wrongNonce);
    require(!rejected.timedOut && rejected.exitCode != 0,
        "wrong nonce cleanup was accepted");
    requirePathExists(profile.rootPath, "wrong nonce profile root", true);
    requirePathExists(asset, "wrong nonce profile asset");

    const BrokerResult repaired = runCleanup(profile, profile.nonce);
    require(!repaired.timedOut && repaired.exitCode == 0,
        "wrong nonce fixture could not be cleaned with its real nonce");
    requirePathAbsent(profile.rootPath, "wrong nonce fixture root");
    std::cout << "PROFILE_CLEANUP wrong-nonce PASS\n";
}

void testHeldLease(const std::wstring& parent)
{
    FixtureSet fixtures;
    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring asset = profile.rootPath + L"\\fake-asset.bin";
    writeFixtureFile(asset, "held lease fixture");
    std::wstring error;
    require(profilecleanup::updateOwnedProfile(profile, CleanupPhase::Released, {}, true, error),
        "updateOwnedProfile held lease failed: " + narrow(error));

    const BrokerResult blocked = runCleanup(profile, profile.nonce);
    require(!blocked.timedOut && blocked.exitCode != 0,
        "held exclusive lease cleanup was accepted");
    requirePathExists(profile.rootPath, "held lease profile root", true);
    requirePathExists(asset, "held lease profile asset");

    leaseGuard.release();
    const BrokerResult released = runCleanup(profile, profile.nonce);
    require(!released.timedOut && released.exitCode == 0,
        "held lease fixture could not be cleaned after release");
    requirePathAbsent(profile.rootPath, "held lease fixture root");
    std::cout << "PROFILE_CLEANUP held-lease PASS\n";
}

void testConcurrentCleanup(const std::wstring& parent, std::vector<std::string>& notRun)
{
    const std::vector<std::wstring> baseline = recoveryCandidates(parent);
    if (!baseline.empty()) {
        notRun.push_back("concurrent cleanup: pre-existing recovery candidates under " + narrow(parent));
        std::cout << "PROFILE_CLEANUP NOT_RUN concurrent cleanup: pre-existing candidates\n";
        return;
    }

    FixtureSet fixtures;
    std::wstring targetPath;
    std::wstring sentinelPath;
    createTrackedTarget(localAppDataRoot(), fixtures, targetPath, sentinelPath);
    const std::string sentinelBefore = readFixtureFile(sentinelPath);

    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring asset = profile.rootPath + L"\\concurrent.asset";
    writeFixtureFile(asset, "concurrent cleanup fixture");
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);

    const std::vector<std::wstring> arguments{L"--cleanup", profile.instanceId,
        nonceHex(profile.nonce)};
    BrokerProcess first = startBroker(arguments);
    BrokerProcess second = startBroker(arguments);
    const BrokerResult firstResult = waitBroker(first);
    const BrokerResult secondResult = waitBroker(second);
    require(!firstResult.timedOut && !secondResult.timedOut,
        "concurrent cleanup helper timed out (first=" + brokerResultText(firstResult) +
        ", second=" + brokerResultText(secondResult) + ")");
    require((firstResult.exitCode == 0) != (secondResult.exitCode == 0),
        "concurrent cleanup did not produce exactly one successful claimant (first=" +
        brokerResultText(firstResult) + ", second=" + brokerResultText(secondResult) + ")");
    requirePathAbsent(profile.rootPath, "concurrent cleanup owned root");
    requirePathAbsent(asset, "concurrent cleanup asset");
    require(recoveryCandidates(parent).empty(),
        "concurrent cleanup left an owned recovery candidate");
    requirePathExists(sentinelPath, "concurrent cleanup unrelated sentinel");
    require(readFixtureFile(sentinelPath) == sentinelBefore,
        "concurrent cleanup changed unrelated sentinel");
    std::cout << "PROFILE_CLEANUP concurrent-claim PASS (first=" <<
        brokerResultText(firstResult) << ", second=" << brokerResultText(secondResult) << ")\n";
}

void testTransientClaimHandle(const std::wstring& parent)
{
    FixtureSet fixtures;
    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring asset = profile.rootPath + L"\\transient.asset";
    writeFixtureFile(asset, "transient cleanup reader");
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);

    LockedFileGuard reader;
    reader.handle = ::CreateFileW(asset.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader.handle == INVALID_HANDLE_VALUE) reader.handle = nullptr;
    require(reader.handle != nullptr, "unable to open transient cleanup reader");
    // A contained reader without delete sharing blocks MoveFileEx. Keep it
    // open longer than the historical four-attempt reservation window so this
    // remains a deterministic regression for the claim retry path.
    // The future joins before reader or fixtures are destroyed, even on failure.
    auto releaseReader = std::async(std::launch::async, [&reader] {
        ::Sleep(300);
        reader.release();
    });
    std::wstring error;
    const int result = profilecleanup::cleanupProfileForTest(parent,
        profile.instanceId, profile.nonce, error);
    releaseReader.get();
    require(result == 0, "transient cleanup reader prevented the claim: " + narrow(error));
    requirePathAbsent(profile.rootPath, "transient reader profile root");
    requirePathAbsent(asset, "transient reader asset");
    require(recoveryCandidates(parent).empty(), "transient reader left cleanup metadata");
    std::cout << "PROFILE_CLEANUP transient-claim-reader PASS\n";
}

void testLockedAssetRetry(const std::wstring& parent, std::vector<std::string>& notRun)
{
    const std::vector<std::wstring> baseline = recoveryCandidates(parent);
    if (!baseline.empty()) {
        notRun.push_back("locked asset: pre-existing recovery candidates under " + narrow(parent));
        std::cout << "PROFILE_CLEANUP NOT_RUN locked asset: pre-existing candidates\n";
        return;
    }

    FixtureSet fixtures;
    std::wstring targetPath;
    std::wstring sentinelPath;
    createTrackedTarget(localAppDataRoot(), fixtures, targetPath, sentinelPath);
    const std::string sentinelBefore = readFixtureFile(sentinelPath);

    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring lockedAsset = profile.rootPath + L"\\locked.asset";
    const std::wstring otherAsset = profile.rootPath + L"\\other.asset";
    writeFixtureFile(lockedAsset, "locked cleanup fixture");
    writeFixtureFile(otherAsset, "other cleanup fixture");
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);

    LockedFileGuard locked;
    locked.handle = ::CreateFileW(lockedAsset.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked.handle == INVALID_HANDLE_VALUE) locked.handle = nullptr;
    require(locked.handle != nullptr,
        "unable to hold locked cleanup asset without FILE_SHARE_DELETE: " + win32Error());

    const BrokerResult blocked = runCleanup(profile, profile.nonce);
    require(!blocked.timedOut && blocked.exitCode != 0,
        "locked asset cleanup unexpectedly succeeded (" + brokerResultText(blocked) + ")");
    requirePathExists(profile.rootPath, "locked asset restored root", true);
    requirePathExists(profile.rootPath + L"\\" + profilecleanup::kOwnerMarkerName,
        "locked asset restored owner marker");
    requirePathExists(profile.rootPath + L"\\" + profilecleanup::kLeaseName,
        "locked asset restored lease");
    requirePathExists(lockedAsset, "locked asset after bounded cleanup failure");
    requirePathExists(otherAsset, "other asset after bounded cleanup failure");

    locked.release();
    const BrokerResult retry = runCleanup(profile, profile.nonce);
    require(!retry.timedOut && retry.exitCode == 0,
        "locked asset fixture could not be cleaned after handle release (" +
        brokerResultText(retry) + ")");
    requirePathAbsent(profile.rootPath, "locked asset retry root");
    requirePathExists(sentinelPath, "locked asset unrelated sentinel");
    require(readFixtureFile(sentinelPath) == sentinelBefore,
        "locked asset cleanup changed unrelated sentinel");
    requireRecoveryBaselineUntouched(parent, baseline);
    std::cout << "PROFILE_CLEANUP locked-asset-retry PASS\n";
}

void testRecoveryIgnoresLegacyUnmarkedClosing(const std::wstring& parent,
    std::vector<std::string>& notRun)
{
    const std::vector<std::wstring> baseline = recoveryCandidates(parent);
    if (!baseline.empty()) {
        notRun.push_back("recovery fixture: pre-existing cleanup candidates under " + narrow(parent));
        std::cout << "PROFILE_CLEANUP NOT_RUN recovery fixture: pre-existing candidates\n";
        return;
    }

    FixtureSet fixtures;
    OwnedProfile unmarked;
    createProfile(parent, fixtures, unmarked);
    ProfileLeaseGuard unmarkedGuard(unmarked);
    const std::wstring unmarkedAsset = unmarked.rootPath + L"\\unmarked.asset";
    writeFixtureFile(unmarkedAsset, "unmarked fixture");
    releaseProfileAs(unmarked, unmarkedGuard, CleanupPhase::Released, true);
    require(::DeleteFileW((unmarked.rootPath + L"\\" + profilecleanup::kOwnerMarkerName).c_str()) != FALSE,
        "failed to remove only the unmarked fixture marker");

    OwnedProfile legacy;
    createProfile(parent, fixtures, legacy);
    ProfileLeaseGuard legacyGuard(legacy);
    std::wstring legacyAsset = legacy.rootPath + L"\\legacy.asset";
    writeFixtureFile(legacyAsset, "legacy fixture");
    releaseProfileAs(legacy, legacyGuard, CleanupPhase::Released, true);
    const std::wstring legacyPath = parent + L"\\" + uniqueGuidToken(L"legacy-profile-");
    require(::MoveFileExW(legacy.rootPath.c_str(), legacyPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
        "failed to rename only the legacy fixture");
    fixtures.renameTrackedRoot(legacy.rootPath, legacyPath, parent);
    legacy.rootPath = legacyPath;
    legacyAsset = legacy.rootPath + L"\\legacy.asset";

    OwnedProfile closing;
    createProfile(parent, fixtures, closing);
    ProfileLeaseGuard closingGuard(closing);
    const std::wstring closingAsset = closing.rootPath + L"\\closing.asset";
    writeFixtureFile(closingAsset, "closing fixture");
    ProcessIdentity deadRecordedProcess{};
    require(profilecleanup::queryProcessIdentity(::GetCurrentProcessId(), deadRecordedProcess),
        "queryProcessIdentity for Closing fixture failed");
    // Keep a schema-valid creation time but use an impossible process id. The
    // recovery proof must therefore reject this Closing marker without
    // treating an invalid marker as evidence about cleanup safety.
    deadRecordedProcess.processId = 0xfffffffeu;
    std::wstring closingError;
    require(profilecleanup::updateOwnedProfile(closing, CleanupPhase::Closing,
        {deadRecordedProcess}, false, closingError),
        "updateOwnedProfile Closing fixture failed: " + narrow(closingError));
    closingGuard.release();

    const BrokerResult result = runRecovery();
    require(!result.timedOut && result.exitCode == 0,
        "recovery helper did not complete for conservative-skip fixtures");
    requireRecoveryBaselineUntouched(parent, baseline);
    requirePathExists(unmarked.rootPath, "unmarked recovery fixture", true);
    requirePathExists(unmarkedAsset, "unmarked recovery asset");
    requirePathExists(legacy.rootPath, "legacy recovery fixture", true);
    requirePathExists(legacyAsset, "legacy recovery asset");
    requirePathExists(closing.rootPath, "Closing recovery fixture", true);
    requirePathExists(closingAsset, "Closing recovery asset");
    std::cout << "PROFILE_CLEANUP recovery-ignores PASS\n";
}

void tamperMarker(const OwnedProfile& profile)
{
    const std::wstring markerPath = profile.rootPath + L"\\" + profilecleanup::kOwnerMarkerName;
    HANDLE handle = ::CreateFileW(markerPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "unable to open only owned marker for tampering");
    LARGE_INTEGER offset{};
    const std::uint8_t malformed = 0;
    DWORD written = 0;
    const BOOL positioned = ::SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN);
    const BOOL wrote = positioned && ::WriteFile(handle, &malformed, sizeof(malformed), &written, nullptr);
    const BOOL flushed = wrote && written == sizeof(malformed) && ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    require(flushed != FALSE, "tampering only the owned marker failed");
}

void testMalformedMarker(const std::wstring& parent)
{
    FixtureSet fixtures;
    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    const std::wstring asset = profile.rootPath + L"\\malformed.asset";
    writeFixtureFile(asset, "malformed marker fixture");
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);
    tamperMarker(profile);

    const BrokerResult rejected = runCleanup(profile, profile.nonce);
    require(!rejected.timedOut && rejected.exitCode != 0,
        "malformed marker cleanup was accepted");
    requirePathExists(profile.rootPath, "malformed marker profile root", true);
    requirePathExists(asset, "malformed marker asset");
    std::cout << "PROFILE_CLEANUP malformed-marker PASS\n";
}

void testReparseRefusal(const std::wstring& parent, std::vector<std::string>& notRun)
{
    FixtureSet fixtures;
    std::wstring targetPath;
    std::wstring sentinelPath;

    OwnedProfile profile;
    createProfile(parent, fixtures, profile);
    ProfileLeaseGuard leaseGuard(profile);
    releaseProfileAs(profile, leaseGuard, CleanupPhase::Released, true);
    createTrackedTarget(localAppDataRoot(), fixtures, targetPath, sentinelPath);
    const std::string sentinelBefore = readFixtureFile(sentinelPath);
    const std::wstring linkPath = profile.rootPath + L"\\linked-assets";
    BOOL linked = ::CreateSymbolicLinkW(linkPath.c_str(), targetPath.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    DWORD error = linked ? ERROR_SUCCESS : ::GetLastError();
    if (!linked && error == ERROR_INVALID_PARAMETER) {
        linked = ::CreateSymbolicLinkW(linkPath.c_str(), targetPath.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY);
        error = linked ? ERROR_SUCCESS : ::GetLastError();
    }
    if (!linked) {
        notRun.push_back("reparse fixture: CreateSymbolicLinkW unavailable (" +
            win32Error(error) + ")");
        std::cout << "PROFILE_CLEANUP NOT_RUN reparse fixture: " << win32Error(error) << "\n";
        return;
    }

    const BrokerResult rejected = runCleanup(profile, profile.nonce);
    require(!rejected.timedOut && rejected.exitCode != 0,
        "cleanup accepted a profile containing a child reparse point");
    requirePathExists(profile.rootPath, "reparse profile root", true);
    requirePathExists(sentinelPath, "reparse target sentinel");
    require(readFixtureFile(sentinelPath) == sentinelBefore,
        "reparse cleanup changed the unrelated target sentinel");

    require(::RemoveDirectoryW(linkPath.c_str()) != FALSE,
        "failed to remove only the owned reparse fixture");
    const BrokerResult repaired = runCleanup(profile, profile.nonce);
    require(!repaired.timedOut && repaired.exitCode == 0,
        "reparse fixture could not be cleaned after link removal");
    std::cout << "PROFILE_CLEANUP reparse-refusal PASS\n";
}

void testAbsoluteInstanceRejected()
{
    FixtureSet fixtures;
    std::wstring targetPath;
    std::wstring sentinelPath;
    createTrackedTarget(localAppDataRoot(), fixtures, targetPath, sentinelPath);
    const std::string sentinelBefore = readFixtureFile(sentinelPath);
    std::array<std::uint8_t, profilecleanup::kNonceBytes> nonce{};
    nonce[0] = 0xa5;

    const BrokerResult rejected = runBroker({L"--cleanup", targetPath, nonceHex(nonce)});
    require(!rejected.timedOut && rejected.exitCode != 0,
        "absolute instance path was accepted by cleanup CLI");
    requirePathExists(targetPath, "absolute path sentinel target", true);
    requirePathExists(sentinelPath, "absolute path sentinel");
    require(readFixtureFile(sentinelPath) == sentinelBefore,
        "absolute cleanup argument changed unrelated sentinel");
    std::cout << "PROFILE_CLEANUP absolute-path-rejection PASS\n";
}

} // namespace

void runProfileCleanupTests()
{
    const std::wstring localData = localAppDataRoot();
    const std::wstring parent = localData + L"\\" +
        uniqueGuidToken(L"NppTerminal.ProfileCleanup.Tests-");
    require(::CreateDirectoryW(parent.c_str(), nullptr) != FALSE,
        "isolated cleanup parent creation failed");
    struct ParentGuard final {
        std::wstring path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~ParentGuard()
        {
            fixtureParent.clear();
            if (handle != INVALID_HANDLE_VALUE) {
                ::CloseHandle(handle);
                // Only an empty directory created by this invocation. No
                // recursion or cleanup of the production WebView parent.
                ::RemoveDirectoryW(path.c_str());
            }
        }
    } guard{parent};
    guard.handle = ::CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    require(guard.handle != INVALID_HANDLE_VALUE &&
        ::GetFileInformationByHandle(guard.handle, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "isolated parent identity unavailable");
    fixtureParent = parent;
    std::vector<std::string> notRun;
    testValidReleasedCleanup(parent);
    testWrongNonce(parent);
    testHeldLease(parent);
    for (int attempt = 0; attempt != 20; ++attempt) {
        testConcurrentCleanup(parent, notRun);
    }
    testTransientClaimHandle(parent);
    testLockedAssetRetry(parent, notRun);
    testRecoveryIgnoresLegacyUnmarkedClosing(parent, notRun);
    testMalformedMarker(parent);
    testReparseRefusal(parent, notRun);
    testAbsoluteInstanceRejected();
    if (!notRun.empty()) {
        std::string message = "Profile cleanup required matrix has NotRun cases:";
        for (const std::string& item : notRun) message += " " + item + ";";
        fail(message);
    }
    std::cout << "PROFILE_CLEANUP PASS\n";
}

} // namespace nppterminal::tests
