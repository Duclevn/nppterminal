#include "TerminalSession.h"

#include "BrokerProtocol.h"
#include "Protocol.h"
#include "Version.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <thread>
#include <utility>

namespace nppterminal {

namespace {

constexpr std::size_t kMaxQueuedInputBytes = kMaxInputBytes;
constexpr DWORD kHardStopBudgetMs = 2000;
constexpr DWORD kStopForceReserveMs = 200;
constexpr DWORD kPollSleepMs = 1;
#ifndef PIPE_REJECT_REMOTE_CLIENTS
constexpr DWORD PIPE_REJECT_REMOTE_CLIENTS = 0x00000008u;
#endif
#ifndef FILE_FLAG_FIRST_PIPE_INSTANCE
constexpr DWORD FILE_FLAG_FIRST_PIPE_INSTANCE = 0x00080000u;
#endif
void brokerModuleAnchor()
{
}

void closeNativeHandle(HANDLE& handle)
{
    if (handle != nullptr) {
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

bool secureRandom(std::array<std::uint8_t, 16>& bytes)
{
    using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    HMODULE advapi = ::LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!advapi) return false;
    const auto random = reinterpret_cast<RtlGenRandomFn>(
        ::GetProcAddress(advapi, "SystemFunction036"));
    const BOOLEAN generated = random ? random(bytes.data(), static_cast<ULONG>(bytes.size())) : FALSE;
    ::FreeLibrary(advapi);
    return generated != FALSE;
}

std::wstring pipeName(const wchar_t* channel)
{
    std::array<std::uint8_t, 16> nonce{};
    if (!secureRandom(nonce)) return {};
    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring token;
    token.reserve(nonce.size() * 2);
    for (const std::uint8_t value : nonce) {
        token.push_back(kHex[(value >> 4) & 0xf]);
        token.push_back(kHex[value & 0xf]);
    }
    return L"\\\\.\\pipe\\NppTerminal." + token + L"." + channel;
}

bool isPipeClosedError(DWORD error)
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_NO_DATA;
}

bool validateHelperVersion(const std::wstring& path, std::wstring& error)
{
    HMODULE versionModule = ::LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!versionModule) {
        error = L"The Windows version-information API is unavailable.";
        return false;
    }
    using GetSizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using GetInfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto getSize = reinterpret_cast<GetSizeFn>(
        ::GetProcAddress(versionModule, "GetFileVersionInfoSizeW"));
    const auto getInfo = reinterpret_cast<GetInfoFn>(
        ::GetProcAddress(versionModule, "GetFileVersionInfoW"));
    const auto query = reinterpret_cast<QueryFn>(
        ::GetProcAddress(versionModule, "VerQueryValueW"));
    DWORD ignored = 0;
    const DWORD size = getSize ? getSize(path.c_str(), &ignored) : 0;
    std::vector<std::uint8_t> bytes(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedBytes = 0;
    const bool valid = size != 0 && getInfo && query &&
        getInfo(path.c_str(), 0, size, bytes.data()) &&
        query(bytes.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedBytes) &&
        fixed != nullptr && fixedBytes >= sizeof(VS_FIXEDFILEINFO) &&
        HIWORD(fixed->dwFileVersionMS) == NPPTERMINAL_VERSION_MAJOR &&
        LOWORD(fixed->dwFileVersionMS) == NPPTERMINAL_VERSION_MINOR &&
        HIWORD(fixed->dwFileVersionLS) == NPPTERMINAL_VERSION_PATCH;
    ::FreeLibrary(versionModule);
    if (!valid) {
        error = L"The NppTerminal broker version does not match the plugin.";
        return false;
    }
    return true;
}

} // namespace

TerminalSession::TerminalSession(SessionCallbacks callbacks)
    : callbacks_(std::move(callbacks))
{
}

TerminalSession::~TerminalSession()
{
    stop(StopReason::PanelDestroy);
    // A cancellation that Windows has not completed yet owns raw storage and
    // its event by design.  Reclaim it when the completion is already visible;
    // otherwise leave the single orphan alive until the kernel signals it.
    reclaimOrphanedCommandWrite();
}

void TerminalSession::closeHandle(HANDLE& handle)
{
    closeNativeHandle(handle);
}

std::wstring TerminalSession::win32Error(DWORD code)
{
    if (code == ERROR_SUCCESS) code = ::GetLastError();
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

std::wstring TerminalSession::helperPath()
{
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&brokerModuleAnchor), &module)) {
        return {};
    }
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(module, path,
        static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
#ifdef NPPTERMINAL_TESTS
    result.resize(slash + 1);
    result += L"NppTerminalBroker.Tests.exe";
#else
    result.resize(slash + 1);
    result += L"NppTerminalBroker.exe";
#endif
    return result;
}

SessionState TerminalSession::state() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

std::uint64_t TerminalSession::generation() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return generation_;
}

std::uint64_t TerminalSession::nextGeneration() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return generation_ + 1;
}

bool TerminalSession::hasProcess() const
{
    std::lock_guard<std::mutex> lock(resourceMutex_);
    return brokerProcess_ != nullptr;
}

DWORD TerminalSession::processId() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return shellProcessId_;
}

DWORD TerminalSession::brokerProcessId() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return brokerProcessId_;
}

bool TerminalSession::processSignaled() const
{
    std::lock_guard<std::mutex> lock(resourceMutex_);
    return brokerProcess_ != nullptr &&
        ::WaitForSingleObject(brokerProcess_, 0) == WAIT_OBJECT_0;
}

bool TerminalSession::hasPendingWork() const
{
    std::lock_guard<std::mutex> resourceLock(resourceMutex_);
    if (brokerProcess_ || pendingCommandWrite_ || orphanedCommandWrite_ ||
        orphanedCommandWriteEvent_ ||
        !commandQueue_.empty() ||
        !outputBuffer_.empty() || !eventBuffer_.empty() || pendingOutputFrame_ ||
        !stateNotifications_.empty()) return true;
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return state_ == SessionState::Starting || state_ == SessionState::Running ||
        state_ == SessionState::Stopping;
}

#ifdef NPPTERMINAL_TESTS
bool TerminalSession::hasPendingCommandWrite() const
{
    return pendingCommandWrite_ != nullptr && pendingCommandWrite_->overlappedPending;
}
#endif

bool TerminalSession::createPipePair(bool parentWrites, HANDLE& parent, HANDLE& child,
    std::wstring& error, DWORD bufferBytes)
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    using ConvertSddlFn = BOOL(WINAPI*)(LPCWSTR, DWORD, PSECURITY_DESCRIPTOR*, PULONG);
    using ConvertSidFn = BOOL(WINAPI*)(PSID, LPWSTR*);
    using OpenTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using GetTokenInfoFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    HMODULE advapi = ::LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto convertSddl = advapi ? reinterpret_cast<ConvertSddlFn>(
        ::GetProcAddress(advapi, "ConvertStringSecurityDescriptorToSecurityDescriptorW")) : nullptr;
    const auto convertSid = advapi ? reinterpret_cast<ConvertSidFn>(
        ::GetProcAddress(advapi, "ConvertSidToStringSidW")) : nullptr;
    const auto openToken = advapi ? reinterpret_cast<OpenTokenFn>(
        ::GetProcAddress(advapi, "OpenProcessToken")) : nullptr;
    const auto getTokenInfo = advapi ? reinterpret_cast<GetTokenInfoFn>(
        ::GetProcAddress(advapi, "GetTokenInformation")) : nullptr;
    HANDLE token = nullptr;
    DWORD tokenBytes = 0;
    std::vector<std::uint8_t> tokenBuffer;
    LPWSTR sidString = nullptr;
    if (!convertSddl || !convertSid || !openToken || !getTokenInfo ||
        !openToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = L"Unable to create the broker pipe security descriptor.";
        if (advapi) ::FreeLibrary(advapi);
        return false;
    }
    getTokenInfo(token, TokenUser, nullptr, 0, &tokenBytes);
    tokenBuffer.resize(tokenBytes);
    const bool tokenReady = tokenBytes != 0 &&
        getTokenInfo(token, TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes) &&
        convertSid(reinterpret_cast<PSID>(reinterpret_cast<TOKEN_USER*>(tokenBuffer.data())->User.Sid),
            &sidString);
    closeNativeHandle(token);
    if (!tokenReady || !sidString) {
        error = L"Unable to resolve the current user for the broker pipe ACL.";
        if (sidString) ::LocalFree(sidString);
        if (advapi) ::FreeLibrary(advapi);
        return false;
    }
    const std::wstring securityString = L"D:(A;;GA;;;" + std::wstring(sidString) +
        L")(A;;GA;;;SY)";
    ::LocalFree(sidString);
    if (!convertSddl(securityString.c_str(), 1, &descriptor, nullptr)) {
        error = L"Unable to create the broker pipe security descriptor.";
        if (advapi) ::FreeLibrary(advapi);
        return false;
    }
    attributes.lpSecurityDescriptor = descriptor;
    const auto cleanupSecurity = [&] {
        if (descriptor) ::LocalFree(descriptor);
        if (advapi) ::FreeLibrary(advapi);
    };
    const std::wstring name = pipeName(parentWrites ? L"command" : L"stream");
    if (name.empty()) {
        error = L"The operating system did not provide a secure random pipe nonce.";
        cleanupSecurity();
        return false;
    }
    const DWORD openMode = (parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
        (parentWrites ? FILE_FLAG_OVERLAPPED : 0) | FILE_FLAG_FIRST_PIPE_INSTANCE;
    if (bufferBytes == 0) bufferBytes = static_cast<DWORD>(kBrokerMaxPayloadBytes + 4096);
    parent = ::CreateNamedPipeW(name.c_str(), openMode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        bufferBytes, bufferBytes, 0, &attributes);
    if (parent == INVALID_HANDLE_VALUE) {
        parent = nullptr;
        error = L"CreateNamedPipe failed: " + win32Error(0);
        cleanupSecurity();
        return false;
    }

    const DWORD desiredAccess = parentWrites ? GENERIC_READ : GENERIC_WRITE;
    child = ::CreateFileW(name.c_str(), desiredAccess, 0, &attributes, OPEN_EXISTING,
        0, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        child = nullptr;
        error = L"Opening the broker pipe failed: " + win32Error(0);
        closeHandle(parent);
        cleanupSecurity();
        return false;
    }
    if (!::SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
        error = L"Unable to make the broker pipe host-owned: " + win32Error(0);
        closeHandle(parent);
        closeHandle(child);
        cleanupSecurity();
        return false;
    }
    cleanupSecurity();
    return true;
}

bool TerminalSession::createBroker(const SessionStartOptions& options,
    std::uint64_t generation, std::wstring& error)
{
    const std::wstring executable = helperPath();
    if (executable.empty() || ::GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"The NppTerminal broker helper is missing beside the plugin: " + executable;
        return false;
    }
    if (!validateHelperVersion(executable, error)) return false;

    HANDLE commandPipe = nullptr;
    HANDLE commandChild = nullptr;
    HANDLE outputPipe = nullptr;
    HANDLE outputChild = nullptr;
    HANDLE eventPipe = nullptr;
    HANDLE eventChild = nullptr;
    DWORD commandBufferBytes = 0;
#ifdef NPPTERMINAL_TESTS
    if (options.testPauseCommandReader) commandBufferBytes = 4096;
#endif
    if (!createPipePair(true, commandPipe, commandChild, error, commandBufferBytes) ||
        !createPipePair(false, outputPipe, outputChild, error) ||
        !createPipePair(false, eventPipe, eventChild, error)) {
        closeHandle(commandPipe);
        closeHandle(commandChild);
        closeHandle(outputPipe);
        closeHandle(outputChild);
        closeHandle(eventPipe);
        closeHandle(eventChild);
        return false;
    }

    HANDLE outerJob = ::CreateJobObjectW(nullptr, nullptr);
    if (!outerJob) {
        error = L"CreateJobObject failed: " + win32Error(0);
        closeHandle(commandPipe);
        closeHandle(commandChild);
        closeHandle(outputPipe);
        closeHandle(outputChild);
        closeHandle(eventPipe);
        closeHandle(eventChild);
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(outerJob, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        error = L"SetInformationJobObject failed: " + win32Error(0);
        closeHandle(outerJob);
        closeHandle(commandPipe);
        closeHandle(commandChild);
        closeHandle(outputPipe);
        closeHandle(outputChild);
        closeHandle(eventPipe);
        closeHandle(eventChild);
        return false;
    }

    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    if (attributeSize == 0) {
        error = L"Unable to allocate the broker process attribute list.";
        closeHandle(outerJob);
        closeHandle(commandPipe);
        closeHandle(commandChild);
        closeHandle(outputPipe);
        closeHandle(outputChild);
        closeHandle(eventPipe);
        closeHandle(eventChild);
        return false;
    }
    std::vector<std::uint8_t> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    HANDLE inherited[] = {commandChild, outputChild, eventChild};
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr)) {
        error = L"Unable to configure broker handle inheritance: " + win32Error(0);
        ::DeleteProcThreadAttributeList(attributes);
        closeHandle(outerJob);
        closeHandle(commandPipe);
        closeHandle(commandChild);
        closeHandle(outputPipe);
        closeHandle(outputChild);
        closeHandle(eventPipe);
        closeHandle(eventChild);
        return false;
    }

    const std::wstring commandLine = L"\"" + executable + L"\" --command-handle=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(commandChild)) +
        L" --output-handle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(outputChild)) +
        L" --event-handle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(eventChild));
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    const BOOL created = ::CreateProcessW(executable.c_str(), mutableCommandLine.data(),
        nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
        &startupInfo.StartupInfo, &processInfo);
    ::DeleteProcThreadAttributeList(attributes);
    closeHandle(commandChild);
    closeHandle(outputChild);
    closeHandle(eventChild);
    if (!created) {
        error = L"CreateProcessW for the broker failed: " + win32Error(0);
        closeHandle(outerJob);
        closeHandle(commandPipe);
        closeHandle(outputPipe);
        closeHandle(eventPipe);
        return false;
    }
    if (!::AssignProcessToJobObject(outerJob, processInfo.hProcess)) {
        error = L"AssignProcessToJobObject failed for the broker: " + win32Error(0);
        ::TerminateProcess(processInfo.hProcess, ERROR_ACCESS_DENIED);
        ::WaitForSingleObject(processInfo.hProcess, 2000);
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(outerJob);
        closeHandle(commandPipe);
        closeHandle(outputPipe);
        closeHandle(eventPipe);
        return false;
    }
    if (::ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        error = L"ResumeThread for the broker failed: " + win32Error(0);
        ::TerminateJobObject(outerJob, ERROR_PROCESS_ABORTED);
        ::WaitForSingleObject(processInfo.hProcess, 2000);
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(outerJob);
        closeHandle(commandPipe);
        closeHandle(outputPipe);
        closeHandle(eventPipe);
        return false;
    }
    closeHandle(processInfo.hThread);
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        brokerProcess_ = processInfo.hProcess;
        brokerProcessId_ = processInfo.dwProcessId;
        outerJob_ = outerJob;
        commandPipe_ = commandPipe;
        outputPipe_ = outputPipe;
        eventPipe_ = eventPipe;
        commandWriteEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (commandWriteEvent_) {
            std::memset(&commandWriteOverlapped_, 0, sizeof(commandWriteOverlapped_));
            commandWriteOverlapped_.hEvent = commandWriteEvent_;
        }
    }
    if (!commandWriteEvent_) {
        error = L"Unable to create broker command completion event: " + win32Error(0);
        releaseResources(true);
        return false;
    }

    BrokerStartData startData;
    startData.applicationName = options.applicationName;
    startData.commandLine = options.commandLine;
    startData.workingDirectory = options.workingDirectory;
    startData.columns = options.columns;
    startData.rows = options.rows;
    startData.gitBashPreserveDirectory = options.gitBashPreserveDirectory;
#ifdef NPPTERMINAL_TESTS
    startData.testStallOnStop = options.testStallOnStop;
    startData.testPauseCommandReader = options.testPauseCommandReader;
    startData.testBlockInputWrite = options.testBlockInputWrite;
    startData.testFailThreadStartIndex = static_cast<std::uint32_t>(
        options.testFailThreadStartIndex);
#endif
    std::vector<std::uint8_t> payload;
    if (!encodeBrokerStart(startData, payload, error) ||
        !queueFrame(static_cast<std::uint16_t>(BrokerFrameType::Start), generation, 0, 0,
            payload)) {
        if (error.empty()) error = L"Unable to queue the broker start frame.";
        releaseResources(true);
        return false;
    }
    startCommandQueued_ = true;
    return true;
}

bool TerminalSession::queueFrame(std::uint16_t type, std::uint64_t frameGeneration,
    std::uint64_t id, std::uint32_t flags, const std::vector<std::uint8_t>& payload,
    std::size_t inputBytes)
{
    std::vector<std::uint8_t> encoded;
    if (!encodeBrokerFrame(static_cast<BrokerFrameType>(type), frameGeneration, id, flags,
        payload, encoded)) return false;
    commandQueue_.push_back(QueuedCommand{std::move(encoded), inputBytes});
    return true;
}

bool TerminalSession::beginCommandWrite()
{
    if (pendingCommandWrite_ || orphanedCommandWrite_ || commandQueue_.empty() ||
        !commandPipe_) return false;
    QueuedCommand command = std::move(commandQueue_.front());
    commandQueue_.pop_front();
    auto* allocation = static_cast<BrokerWriteAllocation*>(::VirtualAlloc(nullptr,
        sizeof(BrokerWriteAllocation), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!allocation) {
        markTransportFailure(L"Unable to allocate the broker command buffer.");
        return false;
    }
    std::memset(allocation, 0, sizeof(BrokerWriteAllocation));
    allocation->size = command.encoded.size();
    allocation->inputBytes = command.inputBytes;
    if (command.encoded.size() < kBrokerFrameHeaderBytes) {
        ::VirtualFree(allocation, 0, MEM_RELEASE);
        markTransportFailure(L"The broker command frame is too small.");
        return false;
    }
    std::memcpy(allocation->bytes, command.encoded.data(), command.encoded.size());
    // Decode just the fixed header fields needed for cancellation accounting.
    std::memcpy(&allocation->type, allocation->bytes + 6, sizeof(allocation->type));
    std::memcpy(&allocation->generation, allocation->bytes + 12, sizeof(allocation->generation));
    std::memcpy(&allocation->id, allocation->bytes + 20, sizeof(allocation->id));
    allocation->overlapped = commandWriteOverlapped_;
    allocation->overlapped.Offset = 0;
    allocation->overlapped.OffsetHigh = 0;
    allocation->overlapped.hEvent = commandWriteEvent_;
    ::ResetEvent(commandWriteEvent_);
    pendingCommandWrite_ = allocation;
    return advanceCommandWrite();
}

bool TerminalSession::advanceCommandWrite()
{
    auto* allocation = pendingCommandWrite_;
    if (!allocation || !commandPipe_) return true;
    if (allocation->overlappedPending) {
        DWORD transferred = 0;
        if (!::GetOverlappedResult(commandPipe_, &allocation->overlapped, &transferred, FALSE)) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_IO_INCOMPLETE) return true;
            const bool expectedCancel = stopRequested_.load() &&
                (error == ERROR_OPERATION_ABORTED || error == ERROR_BROKEN_PIPE);
            if (!expectedCancel) markTransportFailure(L"The broker command pipe write failed: " +
                win32Error(error));
            if (allocation->inputBytes != 0 && allocation->id == 0) {
                acceptedInputBytes_ -= std::min(acceptedInputBytes_, allocation->inputBytes);
            }
            ::VirtualFree(allocation, 0, MEM_RELEASE);
            pendingCommandWrite_ = nullptr;
            return false;
        }
        allocation->offset += transferred;
        allocation->overlappedPending = false;
        if (allocation->offset < allocation->size) {
            std::memset(&allocation->overlapped, 0, sizeof(allocation->overlapped));
            allocation->overlapped.hEvent = commandWriteEvent_;
        }
    }
    if (allocation->offset >= allocation->size) {
        if (allocation->inputBytes != 0 && allocation->id == 0) {
            acceptedInputBytes_ -= std::min(acceptedInputBytes_, allocation->inputBytes);
        }
        ::VirtualFree(allocation, 0, MEM_RELEASE);
        pendingCommandWrite_ = nullptr;
        return true;
    }
    DWORD written = 0;
    const BOOL result = ::WriteFile(commandPipe_, allocation->bytes + allocation->offset,
        static_cast<DWORD>(allocation->size - allocation->offset), &written,
        &allocation->overlapped);
    if (result) {
        allocation->offset += written;
        return allocation->offset >= allocation->size ? advanceCommandWrite() : true;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_IO_PENDING) {
        allocation->overlappedPending = true;
        return true;
    }
    const bool expectedCancel = stopRequested_.load() &&
        (error == ERROR_OPERATION_ABORTED || error == ERROR_BROKEN_PIPE);
    if (!expectedCancel) markTransportFailure(L"The broker command pipe write failed: " +
        win32Error(error));
    if (allocation->inputBytes != 0 && allocation->id == 0) {
        acceptedInputBytes_ -= std::min(acceptedInputBytes_, allocation->inputBytes);
    }
    ::VirtualFree(allocation, 0, MEM_RELEASE);
    pendingCommandWrite_ = nullptr;
    return false;
}

void TerminalSession::abandonCommandWrite()
{
    auto* allocation = pendingCommandWrite_;
    if (!allocation) return;
    if (!allocation->overlappedPending) {
        ::VirtualFree(allocation, 0, MEM_RELEASE);
        pendingCommandWrite_ = nullptr;
        return;
    }
    if (commandPipe_) ::CancelIoEx(commandPipe_, &allocation->overlapped);
    const HANDLE completionEvent = commandWriteEvent_ != nullptr
        ? commandWriteEvent_ : allocation->overlapped.hEvent;
    bool holdObservation = false;
#ifdef NPPTERMINAL_TESTS
    holdObservation = callbacks_.testHoldCommandCancellationObservation &&
        callbacks_.testHoldCommandCancellationObservation();
#endif
    if (!holdObservation && completionEvent &&
        ::WaitForSingleObject(completionEvent, 50) == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        if (commandPipe_) ::GetOverlappedResult(commandPipe_, &allocation->overlapped,
            &transferred, FALSE);
        // The completion event is signaled for success and failure alike.  The
        // event is the lifetime fence; a failed GetOverlappedResult still
        // means the kernel no longer references this OVERLAPPED allocation.
        allocation->offset += transferred;
        ::VirtualFree(allocation, 0, MEM_RELEASE);
        pendingCommandWrite_ = nullptr;
        return;
    }
    // The host is about to close the pipe and kill the job.  Keep this raw
    // allocation and its completion event alive in case the kernel completes
    // the canceled write after the handle is closed; neither has a C++ owner
    // or callback pointer.  Only one unresolved operation is allowed for the
    // lifetime of this facade, so a later start must wait for this event.
    if (orphanedCommandWrite_ != nullptr || orphanedCommandWriteEvent_ != nullptr) {
        markTransportFailure(L"A previous broker command write is still pending; "
            L"the terminal cannot start another session until its completion is observed.");
        return;
    }
    orphanedCommandWrite_ = allocation;
    orphanedCommandWriteEvent_ = completionEvent;
    commandWriteEvent_ = nullptr;
    markTransportFailure(L"The broker command write cancellation remains pending; "
        L"restart is blocked until the OS completion is observed.");
    pendingCommandWrite_ = nullptr;
}

void TerminalSession::reclaimOrphanedCommandWrite()
{
    if (!orphanedCommandWrite_) return;
#ifdef NPPTERMINAL_TESTS
    if (callbacks_.testHoldCommandCancellationObservation &&
        callbacks_.testHoldCommandCancellationObservation()) return;
#endif
    if (!orphanedCommandWriteEvent_ ||
        ::WaitForSingleObject(orphanedCommandWriteEvent_, 0) != WAIT_OBJECT_0) return;
    ::VirtualFree(orphanedCommandWrite_, 0, MEM_RELEASE);
    orphanedCommandWrite_ = nullptr;
    closeHandle(orphanedCommandWriteEvent_);
}

bool TerminalSession::start(const SessionStartOptions& options, std::wstring& error)
{
    error.clear();
    reclaimOrphanedCommandWrite();
    const SessionState currentState = state();
    if (currentState != SessionState::NoSession && currentState != SessionState::Exited &&
        currentState != SessionState::Error) {
        error = L"The previous terminal session is still stopping.";
        return false;
    }
    if (orphanedCommandWrite_ != nullptr || orphanedCommandWriteEvent_ != nullptr) {
        error = L"The previous broker command cancellation is still pending; retry after "
            L"the OS completion is observed.";
        stopRequested_.store(true);
        stopDeadlineActive_ = false;
        transportFailed_.store(true);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = SessionState::Error;
        }
        terminalStateSeen_ = true;
        pendingTerminalState_ = SessionState::Error;
        pendingTerminalMessage_ = error;
        terminalStateDelivered_ = true;
        stateNotifications_.push_back(StateNotification{generation(), SessionState::Error, error});
        return false;
    }
    if (hasProcess()) releaseResources(true);
    // Releasing an old session may discover a command write that Windows still
    // owns.  Do not start a replacement while that raw operation is unresolved
    // or the one-orphan lifetime cap would be bypassed.
    if (orphanedCommandWrite_ != nullptr || orphanedCommandWriteEvent_ != nullptr) {
        error = L"The previous broker command cancellation is still pending; retry after "
            L"the OS completion is observed.";
        stopRequested_.store(true);
        stopDeadlineActive_ = false;
        transportFailed_.store(true);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = SessionState::Error;
        }
        terminalStateSeen_ = true;
        pendingTerminalState_ = SessionState::Error;
        pendingTerminalMessage_ = error;
        terminalStateDelivered_ = true;
        stateNotifications_.push_back(StateNotification{generation(), SessionState::Error, error});
        return false;
    }

    std::uint64_t newGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        newGeneration = ++generation_;
        shellProcessId_ = 0;
        brokerProcessId_ = 0;
        state_ = SessionState::Starting;
    }
    stopRequested_.store(false);
    transportFailed_.store(false);
    commandQueue_.clear();
    stateNotifications_.clear();
    acceptedInputs_.clear();
    acceptedInputBytes_ = 0;
    outputBuffer_.clear();
    eventBuffer_.clear();
    if (pendingOutputFrame_) {
        delete pendingOutputFrame_;
        pendingOutputFrame_ = nullptr;
    }
    outputPipeClosed_ = false;
    eventPipeClosed_ = false;
    startCommandQueued_ = false;
    stopCommandQueued_ = false;
    terminalStateSeen_ = false;
    terminalStateDelivered_ = false;
    pendingTerminalMessage_.clear();
    stateNotifications_.push_back(StateNotification{newGeneration, SessionState::Starting,
        L"Starting cmd.exe"});

    if (options.expectedGeneration != 0 && options.expectedGeneration != newGeneration) {
        error = L"The terminal page generation is stale; retry the terminal.";
        dispatchState(newGeneration, SessionState::Error, error, 0);
        return false;
    }
    SessionStartOptions resolved = options;
    if (resolved.applicationName.empty() || resolved.commandLine.empty()) {
        std::wstring workingDirectory = resolved.workingDirectory;
        if (workingDirectory.empty()) {
            wchar_t currentDirectory[MAX_PATH * 4] = {};
            const DWORD length = ::GetCurrentDirectoryW(
                static_cast<DWORD>(std::size(currentDirectory)), currentDirectory);
            if (length == 0 || length >= std::size(currentDirectory)) {
                error = L"The terminal working directory is unavailable.";
                dispatchState(newGeneration, SessionState::Error, error, 0);
                return false;
            }
            workingDirectory.assign(currentDirectory, length);
        }
        SessionStartOptions defaultOptions;
        if (!makeCmdOptions(workingDirectory, resolved.columns, resolved.rows,
            defaultOptions, error)) {
            dispatchState(newGeneration, SessionState::Error, error, 0);
            return false;
        }
        defaultOptions.expectedGeneration = resolved.expectedGeneration;
        defaultOptions.gitBashPreserveDirectory = resolved.gitBashPreserveDirectory;
#ifdef NPPTERMINAL_TESTS
        defaultOptions.testStallOnStop = resolved.testStallOnStop;
        defaultOptions.testPauseCommandReader = resolved.testPauseCommandReader;
        defaultOptions.testBlockInputWrite = resolved.testBlockInputWrite;
        defaultOptions.testFailThreadStartIndex = resolved.testFailThreadStartIndex;
#endif
        resolved = std::move(defaultOptions);
    }
    if (!createBroker(resolved, newGeneration, error)) {
        dispatchState(newGeneration, SessionState::Error, error, 0);
        return false;
    }
    return true;
}

void TerminalSession::requestStop(StopReason /*reason*/)
{
    const SessionState currentState = state();
    if ((currentState == SessionState::NoSession || currentState == SessionState::Exited ||
        currentState == SessionState::Error) && !hasProcess()) return;
    if (!stopRequested_.exchange(true)) {
        stopDeadlineActive_ = true;
        stopDeadline_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kHardStopBudgetMs - kStopForceReserveMs);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = SessionState::Stopping;
        }
        stateNotifications_.push_back(StateNotification{generation(), SessionState::Stopping,
            L"Stopping terminal"});
    }
    // Accepted input remains charged until its broker acknowledgement.  It
    // cannot be delivered after Stop, so clear only commands that have not
    // entered the overlapped write and leave the accounting for release.
    for (auto it = commandQueue_.begin(); it != commandQueue_.end();) {
        if (it->inputBytes != 0) it = commandQueue_.erase(it);
        else ++it;
    }
    if (commandPipe_ && !stopCommandQueued_) {
        if (queueFrame(static_cast<std::uint16_t>(BrokerFrameType::Stop), generation(), 0, 0,
            {})) stopCommandQueued_ = true;
    }
}

bool TerminalSession::waitForStop(DWORD timeoutMs)
{
    const auto startTime = std::chrono::steady_clock::now();
    DWORD budget = timeoutMs;
    if (budget == INFINITE) budget = kHardStopBudgetMs;
    if (stopRequested_.load()) budget = std::min(budget, kHardStopBudgetMs);
    const auto deadline = startTime + std::chrono::milliseconds(budget);
    const auto forceAt = budget > kStopForceReserveMs
        ? deadline - std::chrono::milliseconds(kStopForceReserveMs)
        : deadline;
    for (;;) {
        poll();
        if (terminalStateDelivered_ && processSignaled()) {
            releaseResources(false);
            return true;
        }
        if (!hasProcess() && (state() == SessionState::NoSession ||
            state() == SessionState::Exited || state() == SessionState::Error)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= forceAt) break;
        ::Sleep(kPollSleepMs);
    }
    forceTerminateAndRelease();
    poll();
    return true;
}

void TerminalSession::stop(StopReason reason)
{
    requestStop(reason);
    waitForStop(kHardStopBudgetMs);
}

bool TerminalSession::resize(std::uint16_t columns, std::uint16_t rows)
{
    if (columns == 0 || rows == 0 || columns > 500 || rows > 200 ||
        (state() != SessionState::Running && state() != SessionState::Starting)) return false;
    std::vector<std::uint8_t> payload;
    if (!encodeBrokerResize(columns, rows, payload)) return false;
    // Keep at most one resize behind the currently writing command.  Window
    // resizes can arrive faster than the broker pipe drains and must not grow
    // the control queue without a bound.
    for (auto it = commandQueue_.begin(); it != commandQueue_.end();) {
        std::uint16_t type = 0;
        if (it->encoded.size() >= 8) std::memcpy(&type, it->encoded.data() + 6, sizeof(type));
        if (type == static_cast<std::uint16_t>(BrokerFrameType::Resize)) {
            it = commandQueue_.erase(it);
        } else {
            ++it;
        }
    }
    return queueFrame(static_cast<std::uint16_t>(BrokerFrameType::Resize), generation(), 0, 0,
        payload);
}

bool TerminalSession::write(const std::vector<std::uint8_t>& bytes)
{
    return write(generation(), 0, bytes);
}

bool TerminalSession::write(std::uint64_t frameGeneration, std::uint64_t id,
    const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty() || bytes.size() > kMaxInputBytes || state() != SessionState::Running ||
        frameGeneration != generation() || stopRequested_.load() ||
        acceptedInputBytes_ + bytes.size() > kMaxQueuedInputBytes) return false;
    std::uint64_t actualId = id;
    if (actualId == 0) {
        actualId = ++nextInputId_;
        if (actualId == 0) actualId = ++nextInputId_;
    }
    if (actualId != 0) {
        for (const AcceptedInput& accepted : acceptedInputs_) {
            if (accepted.generation == frameGeneration && accepted.id == actualId) return false;
        }
    }
    if (!queueFrame(static_cast<std::uint16_t>(BrokerFrameType::Input), frameGeneration, actualId, 0,
        bytes, bytes.size())) return false;
    acceptedInputBytes_ += bytes.size();
    acceptedInputs_.push_back(AcceptedInput{frameGeneration, actualId, bytes.size()});
    return true;
}

void TerminalSession::dispatchState(std::uint64_t frameGeneration, SessionState newState,
    const std::wstring& message, DWORD shellProcessId)
{
    if (frameGeneration != generation()) return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = newState;
        if (newState == SessionState::Running) shellProcessId_ = shellProcessId;
    }
    if (newState == SessionState::Exited || newState == SessionState::NoSession ||
        newState == SessionState::Error) {
        terminalStateSeen_ = true;
        pendingTerminalState_ = newState;
        pendingTerminalMessage_ = message;
        if (newState == SessionState::Error) {
            terminalStateDelivered_ = true;
            stateNotifications_.push_back(StateNotification{frameGeneration, newState, message});
        }
    } else {
        stateNotifications_.push_back(StateNotification{frameGeneration, newState, message});
    }
}

void TerminalSession::dispatchEvent(const BrokerFrame& frame)
{
    if (frame.generation != generation()) return;
    switch (frame.type) {
        case BrokerFrameType::State:
            if (frame.flags > static_cast<std::uint32_t>(SessionState::Error)) {
                markTransportFailure(L"The broker sent an invalid session state.");
                break;
            }
            if (!frame.payload.empty() && brokerWide(frame.payload).empty()) {
                markTransportFailure(L"The broker sent an invalid state message.");
                break;
            }
            dispatchState(frame.generation,
                static_cast<SessionState>(frame.flags), brokerWide(frame.payload),
                static_cast<DWORD>(frame.id));
            break;
        case BrokerFrameType::Error:
            if (!frame.payload.empty() && brokerWide(frame.payload).empty()) {
                markTransportFailure(L"The broker sent an invalid error message.");
                break;
            }
            dispatchState(frame.generation, SessionState::Error, brokerWide(frame.payload), 0);
            break;
        case BrokerFrameType::InputAck: {
            if (frame.flags != 0 || !frame.payload.empty() || frame.id == 0) {
                markTransportFailure(L"The broker sent a malformed input acknowledgement.");
                break;
            }
            auto it = std::find_if(acceptedInputs_.begin(), acceptedInputs_.end(),
                [&](const AcceptedInput& accepted) {
                    return accepted.generation == frame.generation && accepted.id == frame.id;
                });
            if (it == acceptedInputs_.end()) {
                markTransportFailure(L"The broker acknowledged an unknown input id.");
                break;
            }
            acceptedInputBytes_ -= std::min(acceptedInputBytes_, it->bytes);
            acceptedInputs_.erase(it);
            if (callbacks_.inputAck && frame.id != 0) {
                callbacks_.inputAck(frame.generation, frame.id);
            }
            break;
        }
#ifdef NPPTERMINAL_TESTS
        case BrokerFrameType::InputWriteStarted:
            if (frame.flags != 0 || !frame.payload.empty()) {
                markTransportFailure(L"The broker sent a malformed input-start event.");
                break;
            }
            if (callbacks_.inputWriteStarted) callbacks_.inputWriteStarted(frame.generation,
                frame.id);
            break;
        case BrokerFrameType::InputWriteFinished:
            if ((frame.flags & ~1u) != 0 || !frame.payload.empty()) {
                markTransportFailure(L"The broker sent a malformed input-finish event.");
                break;
            }
            if (callbacks_.inputWriteFinished) callbacks_.inputWriteFinished(frame.generation,
                frame.id, (frame.flags & 1u) != 0);
            break;
#endif
        case BrokerFrameType::Start:
        case BrokerFrameType::Input:
        case BrokerFrameType::Resize:
        case BrokerFrameType::Stop:
        case BrokerFrameType::TestStallOnStop:
        case BrokerFrameType::Output:
            markTransportFailure(L"The broker event channel contained a command frame.");
            break;
        default:
            break;
    }
}

void TerminalSession::drainEventPipe()
{
    if (eventPipeClosed_ || !eventPipe_) return;
    DWORD available = 0;
    if (!::PeekNamedPipe(eventPipe_, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD error = ::GetLastError();
        if (isPipeClosedError(error)) eventPipeClosed_ = true;
        else markTransportFailure(L"The broker event pipe failed: " + win32Error(error));
        return;
    }
    if (available != 0) {
        const DWORD amount = std::min<DWORD>(available, 32u * 1024u);
        std::vector<std::uint8_t> bytes(amount);
        DWORD read = 0;
        if (!::ReadFile(eventPipe_, bytes.data(), amount, &read, nullptr) || read == 0) {
            const DWORD error = ::GetLastError();
            if (isPipeClosedError(error)) eventPipeClosed_ = true;
            else markTransportFailure(L"The broker event read failed: " + win32Error(error));
            return;
        }
        eventBuffer_.insert(eventBuffer_.end(), bytes.begin(), bytes.begin() + read);
    }
    for (;;) {
        BrokerFrame frame;
        std::wstring error;
        if (!tryDecodeBrokerFrame(eventBuffer_, frame, error)) {
            if (!error.empty()) markTransportFailure(error);
            break;
        }
        dispatchEvent(frame);
    }
    if (eventPipeClosed_ && !eventBuffer_.empty()) {
        markTransportFailure(L"The broker event channel ended with a partial frame.");
    }
}

bool TerminalSession::consumeOutputFrame()
{
    if (!pendingOutputFrame_) return true;
    BrokerFrame* frame = pendingOutputFrame_;
    const bool consumed = !callbacks_.output || callbacks_.output(frame->generation, frame->id,
        frame->payload, stopRequested_);
    if (!consumed) return false;
    delete frame;
    pendingOutputFrame_ = nullptr;
    return true;
}

void TerminalSession::drainOutputPipe()
{
    if (pendingOutputFrame_ && !consumeOutputFrame()) return;
    if (outputPipeClosed_ || !outputPipe_) return;
    DWORD available = 0;
    if (!::PeekNamedPipe(outputPipe_, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD error = ::GetLastError();
        if (isPipeClosedError(error)) outputPipeClosed_ = true;
        else markTransportFailure(L"The broker output pipe failed: " + win32Error(error));
        return;
    }
    if (available != 0) {
        const DWORD amount = std::min<DWORD>(available, 32u * 1024u);
        std::vector<std::uint8_t> bytes(amount);
        DWORD read = 0;
        if (!::ReadFile(outputPipe_, bytes.data(), amount, &read, nullptr) || read == 0) {
            const DWORD error = ::GetLastError();
            if (isPipeClosedError(error)) outputPipeClosed_ = true;
            else markTransportFailure(L"The broker output read failed: " + win32Error(error));
            return;
        }
        outputBuffer_.insert(outputBuffer_.end(), bytes.begin(), bytes.begin() + read);
    }
    for (;;) {
        BrokerFrame frame;
        std::wstring error;
        if (!tryDecodeBrokerFrame(outputBuffer_, frame, error)) {
            if (!error.empty()) markTransportFailure(error);
            break;
        }
        if (frame.type != BrokerFrameType::Output) {
            markTransportFailure(L"The broker output channel contained a non-output frame.");
            return;
        }
        if (frame.generation != generation()) continue;
        pendingOutputFrame_ = new BrokerFrame(std::move(frame));
        if (!consumeOutputFrame()) return;
    }
    if (outputPipeClosed_ && !outputBuffer_.empty()) {
        markTransportFailure(L"The broker output channel ended with a partial frame.");
    }
}

void TerminalSession::finishTerminalStateIfReady()
{
    if (!terminalStateSeen_ || terminalStateDelivered_) return;
    if (!outputPipeClosed_ || !outputBuffer_.empty() || pendingOutputFrame_) return;
    terminalStateDelivered_ = true;
    stateNotifications_.push_back(StateNotification{generation(), pendingTerminalState_,
        pendingTerminalMessage_});
}

void TerminalSession::markTransportFailure(const std::wstring& message)
{
    if (transportFailed_.exchange(true)) return;
    stopRequested_.store(true);
    stopDeadlineActive_ = true;
    stopDeadline_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kHardStopBudgetMs - kStopForceReserveMs);
    const std::uint64_t currentGeneration = generation();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = SessionState::Error;
    }
    terminalStateSeen_ = true;
    pendingTerminalState_ = SessionState::Error;
    pendingTerminalMessage_ = message;
    terminalStateDelivered_ = true;
    stateNotifications_.push_back(StateNotification{currentGeneration, SessionState::Error,
        message});
    if (commandPipe_ && !stopCommandQueued_) {
        if (queueFrame(static_cast<std::uint16_t>(BrokerFrameType::Stop), currentGeneration, 0,
            0, {})) stopCommandQueued_ = true;
    }
}

void TerminalSession::poll()
{
    reclaimOrphanedCommandWrite();
    if (pendingCommandWrite_) advanceCommandWrite();
    if (!pendingCommandWrite_) beginCommandWrite();
    drainEventPipe();
    drainOutputPipe();
    finishTerminalStateIfReady();

    while (!stateNotifications_.empty()) {
        StateNotification notification = std::move(stateNotifications_.front());
        stateNotifications_.pop_front();
        if (callbacks_.state) callbacks_.state(notification.generation, notification.state,
            notification.message);
    }
    if (stopDeadlineActive_ && stopRequested_.load() && hasProcess() &&
        std::chrono::steady_clock::now() >= stopDeadline_) {
        forceTerminateAndRelease();
        return;
    }
    if (terminalStateDelivered_ && processSignaled()) {
        releaseResources(false);
    }
    if (processSignaled() && eventPipeClosed_ && eventBuffer_.empty() &&
        !terminalStateSeen_ && !transportFailed_.load()) {
        markTransportFailure(L"The NppTerminal broker exited unexpectedly.");
    }
}

void TerminalSession::forceTerminateAndRelease()
{
    if (pendingCommandWrite_) abandonCommandWrite();
    const bool cleanupFailure = orphanedCommandWrite_ != nullptr ||
        orphanedCommandWriteEvent_ != nullptr;
    releaseResources(true);
    if (cleanupFailure || transportFailed_.load()) {
        const std::wstring cleanupMessage =
            L"The broker command write cancellation remains pending; restart is blocked "
            L"until the OS completion is observed.";
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = SessionState::Error;
            shellProcessId_ = 0;
        }
        if (cleanupFailure && pendingTerminalMessage_ != cleanupMessage) {
            const std::uint64_t currentGeneration = generation();
            terminalStateSeen_ = true;
            terminalStateDelivered_ = true;
            pendingTerminalState_ = SessionState::Error;
            pendingTerminalMessage_ = cleanupMessage;
            stateNotifications_.push_back(StateNotification{currentGeneration,
                SessionState::Error, cleanupMessage});
        }
        stopDeadlineActive_ = false;
        return;
    }
    const std::uint64_t currentGeneration = generation();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = SessionState::NoSession;
        shellProcessId_ = 0;
    }
    terminalStateSeen_ = true;
    terminalStateDelivered_ = true;
    pendingTerminalState_ = SessionState::NoSession;
    pendingTerminalMessage_ = L"Terminal stopped";
    stateNotifications_.push_back(StateNotification{currentGeneration, SessionState::NoSession,
        pendingTerminalMessage_});
}

void TerminalSession::releaseResources(bool killJob)
{
    if (pendingCommandWrite_) abandonCommandWrite();
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE command = nullptr;
    HANDLE output = nullptr;
    HANDLE event = nullptr;
    HANDLE writeEvent = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        process = brokerProcess_;
        brokerProcess_ = nullptr;
        job = outerJob_;
        outerJob_ = nullptr;
        command = commandPipe_;
        commandPipe_ = nullptr;
        output = outputPipe_;
        outputPipe_ = nullptr;
        event = eventPipe_;
        eventPipe_ = nullptr;
        writeEvent = commandWriteEvent_;
        commandWriteEvent_ = nullptr;
    }
    if (killJob && job) {
        ::TerminateJobObject(job, ERROR_PROCESS_ABORTED);
        if (process) ::WaitForSingleObject(process, kStopForceReserveMs / 2);
    }
    closeHandle(command);
    closeHandle(output);
    closeHandle(event);
    if (writeEvent && writeEvent != orphanedCommandWriteEvent_) closeHandle(writeEvent);
    closeHandle(process);
    closeHandle(job);
    if (pendingOutputFrame_) {
        delete pendingOutputFrame_;
        pendingOutputFrame_ = nullptr;
    }
    commandQueue_.clear();
    acceptedInputs_.clear();
    acceptedInputBytes_ = 0;
    outputBuffer_.clear();
    eventBuffer_.clear();
    outputPipeClosed_ = true;
    eventPipeClosed_ = true;
    stopDeadlineActive_ = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        brokerProcessId_ = 0;
        shellProcessId_ = 0;
    }
}

bool TerminalSession::makeCmdOptions(const std::wstring& workingDirectory,
    std::uint16_t columns, std::uint16_t rows, SessionStartOptions& options,
    std::wstring& error)
{
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = L"Unable to resolve the Windows system directory.";
        return false;
    }
    options.applicationName = std::wstring(systemDirectory, length) + L"\\cmd.exe";
    options.commandLine = L"\"" + options.applicationName + L"\" /Q";
    options.workingDirectory = workingDirectory;
    options.columns = columns;
    options.rows = rows;
    return true;
}

} // namespace nppterminal
