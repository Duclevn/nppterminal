#include "ShellCatalog.h"

#include "Protocol.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

namespace nppterminal {

namespace {

constexpr DWORD kProbePollMs = 20;
constexpr std::size_t kProbeMaxOutputBytes = 32u * 1024u;
// Windows supports extended paths up to 32,767 characters.  Registry values
// longer than that cannot name a supported executable, so reject them before
// sizing a buffer from the reported byte count.
constexpr std::size_t kMaxRegistryStringChars = 32767;

bool boundedRegistryStringBytes(DWORD bytes)
{
    return bytes >= sizeof(wchar_t) &&
        bytes % sizeof(wchar_t) == 0 &&
        static_cast<std::size_t>(bytes / sizeof(wchar_t)) <= kMaxRegistryStringChars;
}

void closeHandle(HANDLE& handle)
{
    if (handle) {
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring environmentValue(const wchar_t* name)
{
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required + 1, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(name, value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return {};
    value.resize(length);
    return value;
}

std::wstring expandEnvironment(const std::wstring& value)
{
    const DWORD required = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) return {};
    std::wstring expanded(required, L'\0');
    const DWORD length = ::ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (length == 0 || length > required) return {};
    expanded.resize(length - 1);
    return expanded;
}

std::wstring joinPath(const std::wstring& directory, const wchar_t* name)
{
    if (directory.empty()) return {};
    std::wstring result(directory);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    if (result.back() != L'\\' && result.back() != L'/') result += L'\\';
    result += name;
    return result;
}

bool executableFile(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool sameInsensitive(const std::wstring& left, const std::wstring& right)
{
    return left.size() == right.size() &&
        ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

void addShell(std::vector<ShellInfo>& catalog, const wchar_t* id,
    const wchar_t* displayName, const std::wstring& applicationName)
{
    if (!isAbsoluteWindowsPath(applicationName) || !executableFile(applicationName)) return;
    for (const ShellInfo& existing : catalog) {
        if (sameInsensitive(existing.id, id) ||
            sameInsensitive(existing.applicationName, applicationName)) return;
    }
    catalog.push_back(ShellInfo{std::wstring(id), std::wstring(displayName), applicationName});
}

void addKnownShell(std::vector<ShellInfo>& catalog, const wchar_t* id,
    const wchar_t* displayName, const std::wstring& root, const wchar_t* relativeExecutable)
{
    if (!isAbsoluteWindowsPath(root)) return;
    addShell(catalog, id, displayName, joinPath(root, relativeExecutable));
}

void addPathCandidates(std::vector<ShellInfo>& catalog, const std::wstring& path,
    const wchar_t* id, const wchar_t* displayName, const wchar_t* executableName)
{
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find(L';', begin);
        std::wstring directory = path.substr(begin,
            end == std::wstring::npos ? std::wstring::npos : end - begin);
        while (!directory.empty() && std::iswspace(directory.front())) directory.erase(0, 1);
        while (!directory.empty() && std::iswspace(directory.back())) directory.pop_back();
        if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"') {
            directory = directory.substr(1, directory.size() - 2);
        }
        // An empty or relative PATH entry means the current directory.  It is
        // intentionally ignored; discovery has no implicit CWD search.
        if (isAbsoluteWindowsPath(directory)) {
            bool supportedCandidate = true;
            if (std::wstring_view(id) == L"gitbash") {
                // A PATH bash.exe can be a WSL alias or another Bash build.
                // Recognize Git for Windows' bin or usr/bin layout instead.
                std::filesystem::path root(directory);
                if (sameInsensitive(root.filename().wstring(), L"bin")) root = root.parent_path();
                if (sameInsensitive(root.filename().wstring(), L"usr")) root = root.parent_path();
                supportedCandidate = executableFile((root / L"cmd" / L"git.exe").wstring()) &&
                    executableFile((root / L"usr" / L"bin" / L"msys-2.0.dll").wstring());
            }
            if (supportedCandidate) addShell(catalog, id, displayName, joinPath(directory, executableName));
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
}

bool readRegistryString(HKEY root, const wchar_t* keyPath, const wchar_t* valueName,
    REGSAM view, std::wstring& value)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, keyPath, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = ::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || !boundedRegistryStringBytes(bytes)) {
        ::RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    result = ::RegQueryValueExW(key, valueName, nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    ::RegCloseKey(key);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || !boundedRegistryStringBytes(bytes)) {
        return false;
    }
    value.assign(buffer.data());
    if (type == REG_EXPAND_SZ) value = expandEnvironment(value);
    return !value.empty();
}

void addGitRegistryCandidates(std::vector<ShellInfo>& catalog)
{
    constexpr const wchar_t* keys[] = {L"SOFTWARE\\GitForWindows"};
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    constexpr REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY, 0};
    for (HKEY root : roots) {
        for (const wchar_t* key : keys) {
            for (REGSAM view : views) {
                std::wstring install;
                if (readRegistryString(root, key, L"InstallPath", view, install)) {
                    addShell(catalog, L"gitbash", L"Git Bash", joinPath(install, L"bin\\bash.exe"));
                    addShell(catalog, L"gitbash", L"Git Bash", joinPath(install, L"usr\\bin\\bash.exe"));
                }
            }
        }
    }
}

void addPowerShellRegistryCandidates(std::vector<ShellInfo>& catalog)
{
    constexpr const wchar_t* keyPath =
        L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions";
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    constexpr REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY, 0};
    for (HKEY root : roots) {
        for (REGSAM view : views) {
            HKEY key = nullptr;
            if (::RegOpenKeyExW(root, keyPath, 0, KEY_ENUMERATE_SUB_KEYS | KEY_READ | view,
                &key) != ERROR_SUCCESS) continue;
            for (DWORD index = 0;; ++index) {
                wchar_t name[256] = {};
                DWORD nameLength = static_cast<DWORD>(std::size(name));
                const LONG result = ::RegEnumKeyExW(key, index, name, &nameLength,
                    nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS) continue;
                HKEY versionKey = nullptr;
                if (::RegOpenKeyExW(key, name, 0, KEY_QUERY_VALUE, &versionKey) != ERROR_SUCCESS) {
                    continue;
                }
                DWORD type = 0;
                DWORD bytes = 0;
                std::wstring install;
                if (::RegQueryValueExW(versionKey, L"InstallLocation", nullptr, &type,
                    nullptr, &bytes) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ) &&
                    boundedRegistryStringBytes(bytes)) {
                    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
                    if (::RegQueryValueExW(versionKey, L"InstallLocation", nullptr, &type,
                        reinterpret_cast<LPBYTE>(buffer.data()), &bytes) == ERROR_SUCCESS &&
                        (type == REG_SZ || type == REG_EXPAND_SZ) &&
                        boundedRegistryStringBytes(bytes)) {
                        install.assign(buffer.data());
                        if (type == REG_EXPAND_SZ) install = expandEnvironment(install);
                    }
                }
                ::RegCloseKey(versionKey);
                if (!install.empty()) {
                    addShell(catalog, L"powershell7", L"PowerShell 7",
                        joinPath(install, L"pwsh.exe"));
                }
            }
            ::RegCloseKey(key);
        }
    }
}

std::wstring decodeProcessOutput(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) return {};
    std::size_t utf16Offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) utf16Offset = 2;
    std::size_t zeroHighBytes = 0;
    for (std::size_t index = utf16Offset + 1; index < bytes.size(); index += 2) {
        if (bytes[index] == 0) ++zeroHighBytes;
    }
    const bool looksUtf16 = utf16Offset != 0 ||
        (bytes.size() >= 4 && bytes.size() % 2 == 0 && zeroHighBytes > bytes.size() / 4);
    if (looksUtf16) {
        if ((bytes.size() - utf16Offset) % 2 != 0) return {};
        std::wstring result;
        result.reserve((bytes.size() - utf16Offset) / sizeof(wchar_t));
        for (std::size_t index = utf16Offset; index + 1 < bytes.size(); index += 2) {
            result.push_back(static_cast<wchar_t>(
                static_cast<std::uint16_t>(bytes[index]) |
                static_cast<std::uint16_t>(bytes[index + 1]) << 8));
        }
        return result;
    }
    return utf8ToWide(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool readPipeOutput(HANDLE pipe, std::vector<std::uint8_t>& output, bool& closed,
    std::wstring& error)
{
    if (closed) return true;
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_NO_DATA) {
            closed = true;
            return true;
        }
        error = L"The WSL capability probe output pipe failed.";
        return false;
    }
    if (available == 0) return true;
    if (output.size() + available > kProbeMaxOutputBytes) {
        error = L"The WSL capability probe output exceeded its bound.";
        return false;
    }
    const std::size_t oldSize = output.size();
    output.resize(oldSize + available);
    DWORD received = 0;
    if (!::ReadFile(pipe, output.data() + oldSize, available, &received, nullptr) ||
        received == 0) {
        error = L"The WSL capability probe output could not be read.";
        return false;
    }
    output.resize(oldSize + received);
    return true;
}

bool runProbeCommand(const std::wstring& applicationName, const std::wstring& commandLine,
    DWORD timeoutMs, std::vector<std::uint8_t>& output, DWORD& exitCode, std::wstring& error)
{
    output.clear();
    exitCode = ERROR_PROCESS_ABORTED;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!::CreatePipe(&read, &write, &security, 64u * 1024u) ||
        !::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) {
        closeHandle(read);
        closeHandle(write);
        error = L"The WSL capability probe pipe could not be created.";
        return false;
    }
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        closeHandle(read);
        closeHandle(write);
        error = L"The WSL capability probe job could not be created.";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The WSL capability probe job could not be configured.";
        return false;
    }
    HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The WSL capability probe input handle could not be created.";
        return false;
    }
    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    if (attributeSize == 0) {
        closeHandle(nullInput);
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The WSL capability probe handle list could not be allocated.";
        return false;
    }
    std::vector<std::uint8_t> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    HANDLE inherited[] = {write, nullInput};
    const BOOL attributesInitialized = ::InitializeProcThreadAttributeList(
        attributes, 1, 0, &attributeSize);
    const BOOL handlesConfigured = attributesInitialized &&
        ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr);
    if (!handlesConfigured) {
        if (attributesInitialized) ::DeleteProcThreadAttributeList(attributes);
        closeHandle(nullInput);
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The WSL capability probe handle list could not be configured.";
        return false;
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = write;
    startup.StartupInfo.hStdError = write;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    const BOOL created = ::CreateProcessW(applicationName.c_str(), mutableCommand.data(),
        nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED |
            CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup.StartupInfo,
        &processInfo);
    ::DeleteProcThreadAttributeList(attributes);
    closeHandle(nullInput);
    closeHandle(write);
    if (!created) {
        closeHandle(read);
        closeHandle(job);
        error = L"The WSL capability probe could not start.";
        return false;
    }
    if (!::AssignProcessToJobObject(job, processInfo.hProcess) ||
        ::ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        ::TerminateProcess(processInfo.hProcess, ERROR_PROCESS_ABORTED);
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(read);
        closeHandle(job);
        error = L"The WSL capability probe could not be contained.";
        return false;
    }
    closeHandle(processInfo.hThread);
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    bool closed = false;
    bool timedOut = false;
    for (;;) {
        if (!readPipeOutput(read, output, closed, error)) {
            ::TerminateJobObject(job, ERROR_READ_FAULT);
            break;
        }
        const DWORD wait = ::WaitForSingleObject(processInfo.hProcess, 0);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) {
            error = L"The WSL capability probe process wait failed.";
            ::TerminateJobObject(job, ERROR_WAIT_NO_CHILDREN);
            break;
        }
        if (::GetTickCount64() >= deadline) {
            timedOut = true;
            ::TerminateJobObject(job, ERROR_TIMEOUT);
            break;
        }
        ::Sleep(kProbePollMs);
    }
    // A timed out or failed probe is disposable.  Closing the job after the
    // termination request releases the owned process without extending the
    // five-second probe deadline with a synchronous UI-side wait.
    if (!timedOut && error.empty()) {
        if (!readPipeOutput(read, output, closed, error)) {
            // Keep the concrete pipe diagnostic.
        }
        if (!::GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
            error = L"The WSL capability probe exit status could not be read.";
        }
    }
    closeHandle(read);
    closeHandle(processInfo.hProcess);
    closeHandle(job);
    if (timedOut) {
        error = L"The WSL capability probe timed out.";
        return false;
    }
    return error.empty();
}

bool hasDistroOutput(const std::wstring& output)
{
    std::size_t begin = 0;
    while (begin <= output.size()) {
        const std::size_t end = output.find_first_of(L"\r\n", begin);
        std::wstring line = output.substr(begin,
            end == std::wstring::npos ? std::wstring::npos : end - begin);
        while (!line.empty() && (line.front() == 0xfeff || std::iswspace(line.front()))) {
            line.erase(0, 1);
        }
        while (!line.empty() && std::iswspace(line.back())) line.pop_back();
        std::wstring lower(line);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        if (!line.empty() && lower.find(L"no distribution") == std::wstring::npos &&
            lower.find(L"not installed") == std::wstring::npos &&
            lower.find(L"no installed") == std::wstring::npos) {
            return true;
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return false;
}

} // namespace

#ifdef NPPTERMINAL_TESTS
bool readRegistryStringForTest(HKEY root, const wchar_t* keyPath,
    const wchar_t* valueName, REGSAM view, std::wstring& value)
{
    return readRegistryString(root, keyPath, valueName, view, value);
}
#endif

bool isAbsoluteWindowsPath(const std::wstring& path)
{
    if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) return true;
    return path.size() >= 3 &&
        ((path[0] == L'\\' && path[1] == L'\\') ||
            (path[0] == L'/' && path[1] == L'/'));
}

bool isUncWindowsPath(const std::wstring& path)
{
    return path.size() >= 2 &&
        ((path[0] == L'\\' && path[1] == L'\\') ||
            (path[0] == L'/' && path[1] == L'/'));
}

bool verifyWslCapability(const std::wstring& applicationName, std::wstring& error)
{
    error.clear();
    if (!executableFile(applicationName)) {
        error = L"The WSL launcher is unavailable.";
        return false;
    }
    constexpr ULONGLONG kWslCapabilityTimeoutMs = 5000;
    const ULONGLONG deadline = ::GetTickCount64() + kWslCapabilityTimeoutMs;
    std::vector<std::uint8_t> bytes;
    DWORD exitCode = ERROR_PROCESS_ABORTED;
    std::wstring probeError;
    const std::wstring quoted = L"\"" + applicationName + L"\"";
    const DWORD helpTimeout = static_cast<DWORD>(std::max<ULONGLONG>(1,
        deadline > ::GetTickCount64() ? deadline - ::GetTickCount64() : 1));
    if (!runProbeCommand(applicationName, quoted + L" --help", helpTimeout,
        bytes, exitCode, probeError) || (exitCode != 0 && exitCode != 0xffffffffu)) {
        error = probeError.empty() ? L"The WSL help probe failed." : probeError;
        return false;
    }
    const std::wstring help = decodeProcessOutput(bytes);
    if (help.find(L"--cd") == std::wstring::npos) {
        error = L"This WSL launcher does not support --cd.";
        return false;
    }
    bytes.clear();
    probeError.clear();
    const DWORD listTimeout = static_cast<DWORD>(std::max<ULONGLONG>(1,
        deadline > ::GetTickCount64() ? deadline - ::GetTickCount64() : 1));
    if (!runProbeCommand(applicationName, quoted + L" --list --quiet", listTimeout,
        bytes, exitCode, probeError) || exitCode != 0) {
        error = probeError.empty() ? L"The WSL installed-distribution probe failed." : probeError;
        return false;
    }
    if (!hasDistroOutput(decodeProcessOutput(bytes))) {
        error = L"WSL is installed but has no registered Linux distribution.";
        return false;
    }
    return true;
}

bool discoverShellCatalog(std::vector<ShellInfo>& catalog, std::wstring& error)
{
    catalog.clear();
    error.clear();

    addPowerShellRegistryCandidates(catalog);
    const std::wstring programFiles = environmentValue(L"ProgramFiles");
    const std::wstring programFilesX86 = environmentValue(L"ProgramFiles(x86)");
    const std::wstring programW6432 = environmentValue(L"ProgramW6432");
    for (const std::wstring& root : {programW6432, programFiles, programFilesX86}) {
        addKnownShell(catalog, L"powershell7", L"PowerShell 7", root,
            L"PowerShell\\7\\pwsh.exe");
    }
    const std::wstring path = environmentValue(L"PATH");
    addPathCandidates(catalog, path, L"powershell7", L"PowerShell 7", L"pwsh.exe");

    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT systemLength = ::GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (systemLength == 0 || systemLength >= MAX_PATH) {
        error = L"Unable to resolve the Windows system directory.";
        return false;
    }
    addShell(catalog, L"cmd", L"Command Prompt",
        std::wstring(systemDirectory, systemLength) + L"\\cmd.exe");

    addGitRegistryCandidates(catalog);
    for (const std::wstring& root : {programW6432, programFiles, programFilesX86}) {
        addKnownShell(catalog, L"gitbash", L"Git Bash", root, L"Git\\bin\\bash.exe");
        addKnownShell(catalog, L"gitbash", L"Git Bash", root, L"Git\\usr\\bin\\bash.exe");
    }
    addPathCandidates(catalog, path, L"gitbash", L"Git Bash", L"bash.exe");

    std::wstring wsl = std::wstring(systemDirectory, systemLength) + L"\\wsl.exe";
    bool wslVerified = false;
    if (!executableFile(wsl)) {
        std::vector<ShellInfo> pathCatalog;
        addPathCandidates(pathCatalog, path, L"wsl", L"WSL", L"wsl.exe");
        for (const ShellInfo& candidate : pathCatalog) {
            if (verifyWslCapability(candidate.applicationName, error)) {
                wsl = candidate.applicationName;
                wslVerified = true;
                break;
            }
            error.clear();
        }
    }
    if (!wslVerified && executableFile(wsl) && verifyWslCapability(wsl, error)) {
        wslVerified = true;
    }
    if (wslVerified) {
        addShell(catalog, L"wsl", L"WSL", wsl);
    } else {
        error.clear();
    }

    if (catalog.empty()) {
        error = L"No supported shell was detected on this Windows host.";
        return false;
    }
    return true;
}

} // namespace nppterminal
