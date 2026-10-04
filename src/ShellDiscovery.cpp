#include "ShellDiscovery.h"

#include "Protocol.h"
#include "Version.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nppterminal {

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxFieldChars = 8192;
constexpr std::size_t kMaxCatalogEntries = 8;
constexpr std::size_t kMaxCommandArgumentChars = 30000;

void closeHandle(HANDLE& handle)
{
    if (handle) {
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring narrowError(DWORD code)
{
    wchar_t buffer[256] = {};
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer,
        static_cast<DWORD>(std::size(buffer)), nullptr);
    if (length == 0) return L"Windows error " + std::to_wstring(code);
    std::wstring result(buffer, length);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

bool validField(const std::wstring& value, bool required)
{
    return value.size() <= kMaxFieldChars && (!required || !value.empty()) &&
        value.find(L'\0') == std::wstring::npos;
}

bool getString(const json& object, const char* name, std::wstring& value, bool required)
{
    if (!object.contains(name) || !object[name].is_string()) return false;
    const std::string text = object[name].get<std::string>();
    value = utf8ToWide(text);
    return validField(value, required) && (!required || !value.empty());
}

bool knownShellId(const std::wstring& id)
{
    return id == L"powershell7" || id == L"cmd" || id == L"gitbash" || id == L"wsl";
}

bool executablePath(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool parseShellInfo(const json& value, ShellInfo& shell)
{
    return value.is_object() && getString(value, "id", shell.id, true) &&
        getString(value, "displayName", shell.displayName, true) &&
        getString(value, "applicationName", shell.applicationName, true) &&
        knownShellId(shell.id) && isAbsoluteWindowsPath(shell.applicationName);
}

json shellInfoJson(const ShellInfo& shell)
{
    return json{{"id", wideToUtf8(shell.id)},
        {"displayName", wideToUtf8(shell.displayName)},
        {"applicationName", wideToUtf8(shell.applicationName)}};
}

bool serializeRequest(const ShellDiscoveryRequest& request, std::string& text,
    std::wstring& error)
{
    if (request.cachedCatalog.size() > kMaxCatalogEntries ||
        !validField(request.requestedShellId, false)) {
        error = L"The shell discovery request is too large or malformed.";
        return false;
    }
    json cached = json::array();
    for (const ShellInfo& shell : request.cachedCatalog) {
        if (!validField(shell.id, true) || !validField(shell.displayName, true) ||
            !validField(shell.applicationName, true) ||
            !isAbsoluteWindowsPath(shell.applicationName)) {
            error = L"The cached shell catalog contains an invalid entry.";
            return false;
        }
        cached.push_back(shellInfoJson(shell));
    }
    const DirectoryCandidates& candidates = request.candidates;
    for (const std::wstring& value : {candidates.explicitDirectory,
        candidates.activeFileDirectory, candidates.defaultDirectory,
        candidates.workingDirectory, candidates.homeDirectory}) {
        if (!validField(value, false)) {
            error = L"The shell discovery directory candidates are malformed.";
            return false;
        }
    }
    json value = {
        {"version", 1},
        {"refreshCatalog", request.refreshCatalog},
        {"cachedCatalog", std::move(cached)},
        {"requestedShellId", wideToUtf8(request.requestedShellId)},
        {"explicitShell", request.explicitShell},
        {"candidates", {
            {"explicitDirectory", wideToUtf8(candidates.explicitDirectory)},
            {"activeFileDirectory", wideToUtf8(candidates.activeFileDirectory)},
            {"defaultDirectory", wideToUtf8(candidates.defaultDirectory)},
            {"workingDirectory", wideToUtf8(candidates.workingDirectory)},
            {"homeDirectory", wideToUtf8(candidates.homeDirectory)},
        }},
    };
    text = value.dump();
    if (text.empty() || text.size() > kShellDiscoveryMaxJsonBytes) {
        error = L"The shell discovery request exceeds its 64 KiB bound.";
        return false;
    }
    return true;
}

bool parseRequest(const std::string& text, ShellDiscoveryRequest& request,
    std::wstring& error)
{
    try {
        const json value = json::parse(text);
        if (!value.is_object() || !value.contains("version") ||
            !value["version"].is_number_unsigned() || value["version"].get<unsigned>() != 1 ||
            !value.contains("refreshCatalog") || !value["refreshCatalog"].is_boolean() ||
            !value.contains("explicitShell") || !value["explicitShell"].is_boolean() ||
            !value.contains("cachedCatalog") || !value["cachedCatalog"].is_array() ||
            value["cachedCatalog"].size() > kMaxCatalogEntries ||
            !getString(value, "requestedShellId", request.requestedShellId, false)) {
            error = L"The shell discovery request schema is invalid.";
            return false;
        }
        request.refreshCatalog = value["refreshCatalog"].get<bool>();
        request.explicitShell = value["explicitShell"].get<bool>();
        request.cachedCatalog.clear();
        for (const json& entry : value["cachedCatalog"]) {
            ShellInfo shell;
            if (!parseShellInfo(entry, shell)) {
                error = L"The cached shell catalog entry is invalid.";
                return false;
            }
            request.cachedCatalog.push_back(std::move(shell));
        }
        if (!value.contains("candidates") || !value["candidates"].is_object()) {
            error = L"The shell discovery directory candidates are missing.";
            return false;
        }
        const json& candidates = value["candidates"];
        if (!getString(candidates, "explicitDirectory", request.candidates.explicitDirectory, false) ||
            !getString(candidates, "activeFileDirectory", request.candidates.activeFileDirectory, false) ||
            !getString(candidates, "defaultDirectory", request.candidates.defaultDirectory, false) ||
            !getString(candidates, "workingDirectory", request.candidates.workingDirectory, false) ||
            !getString(candidates, "homeDirectory", request.candidates.homeDirectory, false)) {
            error = L"The shell discovery directory candidates are invalid.";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        error = L"The shell discovery request is not valid JSON.";
        return false;
    }
}

std::wstring quoteWindowsArgument(const std::wstring& value)
{
    if (value.empty()) return L"\"\"";
    bool needsQuotes = false;
    for (const wchar_t character : value) {
        if (character == L' ' || character == L'\t' || character == L'"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return value;
    }
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool selectShell(const ShellDiscoveryRequest& request, const std::vector<ShellInfo>& catalog,
    ShellInfo& selected, std::wstring& error)
{
    if (!request.requestedShellId.empty()) {
        for (const ShellInfo& shell : catalog) {
            if (shell.id == request.requestedShellId && executablePath(shell.applicationName)) {
                selected = shell;
                return true;
            }
        }
        if (request.explicitShell) {
            error = L"The selected shell is no longer installed or its executable is unavailable: " +
                request.requestedShellId;
            return false;
        }
    } else if (request.explicitShell) {
        error = L"An explicit shell selection did not identify a supported shell.";
        return false;
    }
    constexpr const wchar_t* preference[] = {L"powershell7", L"cmd"};
    for (const wchar_t* preferred : preference) {
        for (const ShellInfo& shell : catalog) {
            if (shell.id == preferred && executablePath(shell.applicationName)) {
                selected = shell;
                return true;
            }
        }
    }
    for (const ShellInfo& shell : catalog) {
        if (executablePath(shell.applicationName)) {
            selected = shell;
            return true;
        }
    }
    error = L"No supported shell was detected on this Windows host.";
    return false;
}

bool resolveRequest(const ShellDiscoveryRequest& request, ShellDiscoveryResult& result)
{
    result = {};
    if (request.refreshCatalog || request.cachedCatalog.empty()) {
        if (!discoverShellCatalog(result.catalog, result.error)) return false;
    } else {
        result.catalog = request.cachedCatalog;
    }
    ShellInfo selected;
    if (!selectShell(request, result.catalog, selected, result.error)) return false;
    const bool rejectUncForCmd = selected.id == L"cmd";
    if (!resolveWorkingDirectory(request.candidates, rejectUncForCmd,
        result.workingDirectory, result.error)) return false;
    return buildShellLaunchCommand(selected, result.workingDirectory,
        result.commandLine, result.gitBashPreserveDirectory, result.error) &&
        ((result.applicationName = selected.applicationName), true);
}

json resultJson(const ShellDiscoveryResult& result)
{
    json catalog = json::array();
    for (const ShellInfo& shell : result.catalog) catalog.push_back(shellInfoJson(shell));
    return json{
        {"version", 1},
        {"catalog", std::move(catalog)},
        {"applicationName", wideToUtf8(result.applicationName)},
        {"commandLine", wideToUtf8(result.commandLine)},
        {"workingDirectory", wideToUtf8(result.workingDirectory)},
        {"gitBashPreserveDirectory", result.gitBashPreserveDirectory},
        {"error", wideToUtf8(result.error)},
    };
}

bool parseResult(const std::vector<std::uint8_t>& bytes, ShellDiscoveryResult& result,
    std::wstring& error)
{
    if (bytes.empty() || bytes.size() > kShellDiscoveryMaxJsonBytes) {
        error = L"The shell discovery helper returned an empty or oversized response.";
        return false;
    }
    const std::wstring wide = utf8ToWide(std::string(reinterpret_cast<const char*>(bytes.data()),
        bytes.size()));
    if (wide.empty()) {
        error = L"The shell discovery helper returned invalid UTF-8.";
        return false;
    }
    try {
        const json value = json::parse(wideToUtf8(wide));
        if (!value.is_object() || !value.contains("version") ||
            !value["version"].is_number_unsigned() || value["version"].get<unsigned>() != 1 ||
            !value.contains("catalog") || !value["catalog"].is_array() ||
            value["catalog"].size() > kMaxCatalogEntries ||
            !getString(value, "applicationName", result.applicationName, false) ||
            !getString(value, "commandLine", result.commandLine, false) ||
            !getString(value, "workingDirectory", result.workingDirectory, false) ||
            !value.contains("gitBashPreserveDirectory") ||
            !value["gitBashPreserveDirectory"].is_boolean() ||
            !getString(value, "error", result.error, false)) {
            error = L"The shell discovery helper response schema is invalid.";
            return false;
        }
        result.catalog.clear();
        for (const json& entry : value["catalog"]) {
            ShellInfo shell;
            if (!parseShellInfo(entry, shell)) {
                error = L"The shell discovery helper returned an invalid catalog entry.";
                return false;
            }
            result.catalog.push_back(std::move(shell));
        }
        result.gitBashPreserveDirectory = value["gitBashPreserveDirectory"].get<bool>();
        return true;
    } catch (const std::exception&) {
        error = L"The shell discovery helper response is not valid JSON.";
        return false;
    }
}

void discoveryModuleAnchor() {}

std::wstring helperPath()
{
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&discoveryModuleAnchor), &module)) {
        return {};
    }
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    result.resize(slash + 1);
#ifdef NPPTERMINAL_TESTS
    result += L"NppTerminalBroker.Tests.exe";
#else
    result += L"NppTerminalBroker.exe";
#endif
    return result;
}

} // namespace

bool buildShellLaunchCommand(const ShellInfo& shell, const std::wstring& workingDirectory,
    std::wstring& commandLine, bool& gitBashPreserveDirectory, std::wstring& error)
{
    commandLine.clear();
    error.clear();
    gitBashPreserveDirectory = false;
    if (shell.applicationName.empty() || !isAbsoluteWindowsPath(shell.applicationName) ||
        workingDirectory.empty() || !isAbsoluteWindowsPath(workingDirectory)) {
        error = L"The selected shell or working directory is not absolute.";
        return false;
    }
    const std::wstring executable = quoteWindowsArgument(shell.applicationName);
    if (executable.empty()) {
        error = L"The selected shell path cannot be quoted safely.";
        return false;
    }
    if (shell.id == L"powershell7") {
        commandLine = executable + L" -NoLogo";
    } else if (shell.id == L"cmd") {
        commandLine = executable + L" /Q";
    } else if (shell.id == L"gitbash") {
        commandLine = executable + L" --login -i";
        gitBashPreserveDirectory = true;
    } else if (shell.id == L"wsl") {
        commandLine = executable + L" --cd " + quoteWindowsArgument(workingDirectory);
    } else {
        error = L"The selected shell is not one of the supported shell IDs.";
        return false;
    }
    if (commandLine.size() > 32767) {
        error = L"The selected shell command line exceeds the Windows limit.";
        return false;
    }
    return true;
}

struct ShellDiscoveryClient::Impl final {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE outputRead = nullptr;
    ULONGLONG deadline = 0;
    bool finished = true;
    bool outputClosed = false;
    std::vector<std::uint8_t> output;
    ShellDiscoveryResult result;

    ~Impl()
    {
        cancel();
    }

    void closeResources()
    {
        closeHandle(outputRead);
        closeHandle(process);
        closeHandle(job);
    }

    void cancel()
    {
        if (process && job) {
            ::TerminateJobObject(job, ERROR_PROCESS_ABORTED);
        }
        closeResources();
        finished = true;
    }
};

ShellDiscoveryClient::ShellDiscoveryClient()
    : impl_(std::make_unique<Impl>())
{
}

ShellDiscoveryClient::~ShellDiscoveryClient() = default;

bool ShellDiscoveryClient::start(const ShellDiscoveryRequest& request, std::wstring& error)
{
    error.clear();
    if (!impl_->finished) {
        error = L"Shell discovery is already running.";
        return false;
    }
    impl_->closeResources();
    impl_->finished = true;
    impl_->output.clear();
    impl_->outputClosed = false;
    impl_->result = {};

    std::string requestText;
    if (!serializeRequest(request, requestText, error)) return false;
    const std::vector<std::uint8_t> requestBytes(requestText.begin(), requestText.end());
    const std::string encoded = base64Encode(requestBytes);
    if (encoded.empty() || encoded.size() > kMaxCommandArgumentChars) {
        error = L"The shell discovery request is too large for the helper command line.";
        return false;
    }
    const std::wstring executable = helperPath();
    if (executable.empty() || ::GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"The NppTerminal discovery helper is unavailable.";
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!::CreatePipe(&read, &write, &security, 64u * 1024u) ||
        !::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) {
        closeHandle(read);
        closeHandle(write);
        error = L"The shell discovery output pipe could not be created.";
        return false;
    }
    HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        closeHandle(read);
        closeHandle(write);
        error = L"The shell discovery input handle could not be created.";
        return false;
    }
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        closeHandle(nullInput);
        closeHandle(read);
        closeHandle(write);
        error = L"The shell discovery job could not be created.";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        closeHandle(nullInput);
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The shell discovery job could not be configured.";
        return false;
    }
    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    if (attributeSize == 0) {
        closeHandle(nullInput);
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The shell discovery handle list could not be allocated.";
        return false;
    }
    std::vector<std::uint8_t> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
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
        error = L"The shell discovery handle list could not be configured.";
        return false;
    }
    const std::wstring command = quoteWindowsArgument(executable) + L" --discover " +
        utf8ToWide(encoded);
    if (command.size() > 32767) {
        ::DeleteProcThreadAttributeList(attributes);
        closeHandle(nullInput);
        closeHandle(read);
        closeHandle(write);
        closeHandle(job);
        error = L"The shell discovery helper command line exceeds the Windows limit.";
        return false;
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = write;
    startup.StartupInfo.hStdError = write;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    const BOOL created = ::CreateProcessW(executable.c_str(), mutableCommand.data(),
        nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
        &startup.StartupInfo, &processInfo);
    ::DeleteProcThreadAttributeList(attributes);
    closeHandle(nullInput);
    closeHandle(write);
    if (!created) {
        closeHandle(read);
        closeHandle(job);
        error = L"The shell discovery helper could not start: " + narrowError(::GetLastError());
        return false;
    }
    if (!::AssignProcessToJobObject(job, processInfo.hProcess) ||
        ::ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        ::TerminateProcess(processInfo.hProcess, ERROR_PROCESS_ABORTED);
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(read);
        closeHandle(job);
        error = L"The shell discovery helper could not be contained.";
        return false;
    }
    closeHandle(processInfo.hThread);
    impl_->process = processInfo.hProcess;
    impl_->job = job;
    impl_->outputRead = read;
    impl_->deadline = ::GetTickCount64() + kShellDiscoveryOverallTimeoutMs;
    impl_->finished = false;
    return true;
}

void ShellDiscoveryClient::poll()
{
    if (impl_->finished) return;
    DWORD available = 0;
    if (!impl_->outputClosed && !::PeekNamedPipe(impl_->outputRead, nullptr, 0, nullptr,
        &available, nullptr)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_NO_DATA) {
            impl_->outputClosed = true;
        } else {
            impl_->result.error = L"The shell discovery output pipe failed: " + narrowError(code);
            cancel();
            return;
        }
    }
    if (!impl_->outputClosed && available != 0) {
        if (impl_->output.size() + available > kShellDiscoveryMaxJsonBytes) {
            impl_->result.error = L"The shell discovery helper response exceeded its 64 KiB bound.";
            cancel();
            return;
        }
        const std::size_t oldSize = impl_->output.size();
        impl_->output.resize(oldSize + available);
        DWORD received = 0;
        if (!::ReadFile(impl_->outputRead, impl_->output.data() + oldSize, available,
            &received, nullptr) || received == 0) {
            impl_->result.error = L"The shell discovery helper response could not be read.";
            cancel();
            return;
        }
        impl_->output.resize(oldSize + received);
    }
    const DWORD wait = ::WaitForSingleObject(impl_->process, 0);
    if (wait == WAIT_FAILED) {
        impl_->result.error = L"The shell discovery helper wait failed: " + narrowError(::GetLastError());
        cancel();
        return;
    }
    if (wait == WAIT_OBJECT_0) {
        DWORD exitCode = 1;
        (void)::GetExitCodeProcess(impl_->process, &exitCode);
        // The process owns the output write handle, so once it exits all
        // remaining bytes are available to drain before parsing.
        if (!impl_->outputClosed && ::PeekNamedPipe(impl_->outputRead, nullptr, 0, nullptr,
            &available, nullptr) && available != 0) {
            const std::size_t oldSize = impl_->output.size();
            if (oldSize + available <= kShellDiscoveryMaxJsonBytes) {
                impl_->output.resize(oldSize + available);
                DWORD received = 0;
                if (::ReadFile(impl_->outputRead, impl_->output.data() + oldSize, available,
                    &received, nullptr)) {
                    impl_->output.resize(oldSize + received);
                }
            }
        }
        std::wstring parseError;
        if (!parseResult(impl_->output, impl_->result, parseError)) {
            impl_->result.error = parseError;
        } else if (exitCode != 0 && impl_->result.error.empty()) {
            impl_->result.error = L"The shell discovery helper failed.";
        }
        impl_->closeResources();
        impl_->finished = true;
        return;
    }
    if (wait != WAIT_TIMEOUT) {
        impl_->result.error = L"The shell discovery helper wait returned an invalid result.";
        cancel();
        return;
    }
    if (::GetTickCount64() >= impl_->deadline) {
        impl_->result.error = L"Shell discovery exceeded its 10 second timeout.";
        cancel();
        return;
    }
}

bool ShellDiscoveryClient::finished() const
{
    return impl_->finished;
}

const ShellDiscoveryResult& ShellDiscoveryClient::result() const
{
    return impl_->result;
}

void ShellDiscoveryClient::cancel()
{
    if (impl_->finished) return;
    if (impl_->result.error.empty()) impl_->result.error = L"Shell discovery was canceled.";
    impl_->cancel();
}

int runShellDiscoveryMode(int argc, wchar_t** argv, bool& handled)
{
    handled = false;
    if (argc < 2 || std::wstring(argv[1]) != L"--discover") return 0;
    handled = true;
    ShellDiscoveryResult result;
    std::wstring error;
    if (argc != 3) {
        result.error = L"The shell discovery helper arguments are invalid.";
    } else {
        const std::wstring encoded(argv[2]);
        std::vector<std::uint8_t> bytes;
        if (encoded.size() > kMaxCommandArgumentChars ||
            !base64Decode(wideToUtf8(encoded), bytes) ||
            bytes.empty() || bytes.size() > kShellDiscoveryMaxJsonBytes) {
            result.error = L"The shell discovery helper request encoding is invalid.";
        } else {
            ShellDiscoveryRequest request;
            if (!parseRequest(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                request, error)) {
                result.error = error.empty() ? L"The shell discovery request is invalid." : error;
            } else {
                if (!resolveRequest(request, result) && result.error.empty()) {
                    result.error = error.empty() ? L"The shell discovery request could not be resolved." : error;
                }
            }
        }
    }
    const std::string output = resultJson(result).dump();
    if (output.empty() || output.size() > kShellDiscoveryMaxJsonBytes) return 2;
    HANDLE stdoutHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdoutHandle || stdoutHandle == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0;
    if (!::WriteFile(stdoutHandle, output.data(), static_cast<DWORD>(output.size()), &written, nullptr) ||
        written != output.size()) return 2;
    return result.error.empty() ? 0 : 1;
}

} // namespace nppterminal
