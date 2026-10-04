#include "BrokerSession.h"

#include "Protocol.h"
#include "TerminalSession.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <utility>

namespace nppterminal {

namespace {

constexpr std::size_t kMaxQueuedInputBytes = kMaxInputBytes;
constexpr DWORD kWorkerCancelWaitMs = 2000;
constexpr DWORD kProcessStopWaitMs = 2000;

#pragma pack(push, 1)
struct WireHeader {
    std::uint32_t magic = kBrokerProtocolMagic;
    std::uint16_t version = kBrokerProtocolVersion;
    std::uint16_t type = 0;
    std::uint32_t payloadBytes = 0;
    std::uint64_t generation = 0;
    std::uint64_t id = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == kBrokerFrameHeaderBytes,
    "broker frame header must remain fixed width");

void closeHandle(HANDLE& handle)
{
    if (handle != nullptr) {
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

bool sameEnvironmentName(const std::wstring& entry, const wchar_t* name)
{
    // Entries beginning with '=' are the hidden per-drive current-directory
    // variables maintained by Windows.  Their first '=' is part of the
    // entry, so they are never mistaken for a regular variable name.
    if (entry.empty() || entry.front() == L'=') return false;
    const std::size_t separator = entry.find(L'=');
    if (separator == std::wstring::npos) return false;
    const std::wstring key = entry.substr(0, separator);
    return ::CompareStringOrdinal(key.data(), static_cast<int>(key.size()),
        name, -1, TRUE) == CSTR_EQUAL;
}

bool buildGitBashEnvironment(std::vector<wchar_t>& block, std::wstring& error)
{
    block.clear();
    LPWCH environment = ::GetEnvironmentStringsW();
    if (!environment) {
        error = L"GetEnvironmentStringsW failed.";
        return false;
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* entry = environment; *entry != L'\0';) {
        const std::wstring value(entry);
        if (!sameEnvironmentName(value, L"CHERE_INVOKING")) entries.push_back(value);
        entry += value.size() + 1;
    }
    ::FreeEnvironmentStringsW(environment);
    entries.emplace_back(L"CHERE_INVOKING=1");
    std::sort(entries.begin(), entries.end(), [](const std::wstring& left,
        const std::wstring& right) {
        const int result = ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE);
        return result == CSTR_LESS_THAN;
    });
    std::size_t characters = 1;
    for (const std::wstring& entry : entries) characters += entry.size() + 1;
    block.reserve(characters);
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return true;
}

} // namespace

BrokerSession::BrokerSession(HANDLE commandRead, HANDLE outputWrite, HANDLE eventWrite)
    : commandRead_(commandRead)
    , outputWrite_(outputWrite)
    , eventWrite_(eventWrite)
{
}

BrokerSession::~BrokerSession()
{
    shutdownWorkersBounded();
    closeResources();
    closeHandle(commandRead_);
    closeHandle(outputWrite_);
    closeHandle(eventWrite_);
}

std::wstring BrokerSession::win32Error(DWORD code)
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

bool BrokerSession::readExact(void* bytes, DWORD count)
{
    auto* destination = static_cast<std::uint8_t*>(bytes);
    DWORD total = 0;
    while (total < count) {
        DWORD received = 0;
        if (!::ReadFile(commandRead_, destination + total, count - total, &received, nullptr) ||
            received == 0) {
            return false;
        }
        total += received;
    }
    return true;
}

bool BrokerSession::readFrame(BrokerFrame& frame)
{
    WireHeader header{};
    if (!readExact(&header, sizeof(header))) return false;
    if (header.magic != kBrokerProtocolMagic || header.version != kBrokerProtocolVersion ||
        header.reserved != 0 || header.payloadBytes > kBrokerMaxPayloadBytes) {
        return false;
    }
    std::vector<std::uint8_t> encoded(sizeof(header) + header.payloadBytes);
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (header.payloadBytes != 0 &&
        !readExact(encoded.data() + sizeof(header), header.payloadBytes)) {
        return false;
    }
    std::wstring error;
    return tryDecodeBrokerFrame(encoded, frame, error) && encoded.empty();
}

bool BrokerSession::writeExact(HANDLE pipe, const std::uint8_t* bytes, DWORD count)
{
    DWORD total = 0;
    while (total < count) {
        DWORD written = 0;
        if (!::WriteFile(pipe, bytes + total, count - total, &written, nullptr) ||
            written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

bool BrokerSession::writeFrame(HANDLE pipe, std::mutex* writeMutex, const BrokerFrame& frame)
{
    std::vector<std::uint8_t> encoded;
    if (!encodeBrokerFrame(frame.type, frame.generation, frame.id, frame.flags,
        frame.payload, encoded)) return false;
    std::unique_lock<std::mutex> lock;
    if (writeMutex != nullptr) lock = std::unique_lock<std::mutex>(*writeMutex);
    return writeExact(pipe, encoded.data(), static_cast<DWORD>(encoded.size()));
}

bool BrokerSession::loadConPtyFunctions(std::wstring& error)
{
    HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    createPseudoConsole_ = reinterpret_cast<CreatePseudoConsoleFn>(
        ::GetProcAddress(kernel, "CreatePseudoConsole"));
    resizePseudoConsole_ = reinterpret_cast<ResizePseudoConsoleFn>(
        ::GetProcAddress(kernel, "ResizePseudoConsole"));
    closePseudoConsole_ = reinterpret_cast<ClosePseudoConsoleFn>(
        ::GetProcAddress(kernel, "ClosePseudoConsole"));
    if (!createPseudoConsole_ || !resizePseudoConsole_ || !closePseudoConsole_) {
        error = L"Windows ConPTY is unavailable on this host.";
        return false;
    }
    return true;
}

bool BrokerSession::createProcess(const BrokerStartData& options, std::wstring& error)
{
    if (options.applicationName.empty() || options.commandLine.empty()) {
        error = L"The shell executable or command line is empty.";
        return false;
    }
    if (options.workingDirectory.empty() ||
        ::GetFileAttributesW(options.workingDirectory.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"The terminal working directory is unavailable: " + options.workingDirectory;
        return false;
    }

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE inputPtyRead = nullptr;
    HANDLE inputWrite = nullptr;
    HANDLE outputRead = nullptr;
    HANDLE outputPtyWrite = nullptr;
    if (!::CreatePipe(&inputPtyRead, &inputWrite, &securityAttributes, 0) ||
        !::CreatePipe(&outputRead, &outputPtyWrite, &securityAttributes, 0)) {
        error = L"Unable to create ConPTY pipes: " + win32Error(0);
        closeHandle(inputPtyRead);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(outputPtyWrite);
        return false;
    }
    ::SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);

    HPCON pseudoConsole = nullptr;
    const COORD size{static_cast<SHORT>(options.columns), static_cast<SHORT>(options.rows)};
    const HRESULT result = createPseudoConsole_(size, inputPtyRead, outputPtyWrite, 0,
        &pseudoConsole);
    if (FAILED(result)) {
        error = L"CreatePseudoConsole failed: " + win32Error(static_cast<DWORD>(result));
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(inputPtyRead);
        closeHandle(outputPtyWrite);
        return false;
    }

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        error = L"CreateJobObject failed: " + win32Error(0);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(inputPtyRead);
        closeHandle(outputPtyWrite);
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        error = L"SetInformationJobObject failed: " + win32Error(0);
        closeHandle(job);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(inputPtyRead);
        closeHandle(outputPtyWrite);
        return false;
    }

    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    if (attributeSize == 0) {
        error = L"Unable to allocate the ConPTY process attribute list.";
        closeHandle(job);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(inputPtyRead);
        closeHandle(outputPtyWrite);
        return false;
    }
    std::vector<std::uint8_t> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            pseudoConsole, sizeof(pseudoConsole), nullptr, nullptr)) {
        error = L"Unable to configure the ConPTY process attribute list: " + win32Error(0);
        ::DeleteProcThreadAttributeList(attributes);
        closeHandle(job);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(inputPtyRead);
        closeHandle(outputPtyWrite);
        return false;
    }

    std::vector<wchar_t> commandLine(options.commandLine.begin(), options.commandLine.end());
    commandLine.push_back(L'\0');
    std::vector<wchar_t> childEnvironment;
    LPVOID environmentBlock = nullptr;
    if (options.gitBashPreserveDirectory) {
        if (!buildGitBashEnvironment(childEnvironment, error)) {
            ::DeleteProcThreadAttributeList(attributes);
            closeHandle(job);
            closePseudoConsole_(pseudoConsole);
            closeHandle(inputWrite);
            closeHandle(outputRead);
            closeHandle(inputPtyRead);
            closeHandle(outputPtyWrite);
            return false;
        }
        environmentBlock = childEnvironment.data();
    }
    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = nullptr;
    startupInfo.StartupInfo.hStdOutput = nullptr;
    startupInfo.StartupInfo.hStdError = nullptr;
    startupInfo.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    // The dedicated ConPTY and kill-on-close job already isolate this session.
    // CREATE_NEW_PROCESS_GROUP would disable Ctrl+C in the shell and children.
    const DWORD creationFlags = EXTENDED_STARTUPINFO_PRESENT |
        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    const BOOL created = ::CreateProcessW(options.applicationName.c_str(), commandLine.data(),
        nullptr, nullptr, FALSE, creationFlags, environmentBlock, options.workingDirectory.c_str(),
        &startupInfo.StartupInfo, &processInfo);
    closeHandle(inputPtyRead);
    closeHandle(outputPtyWrite);
    ::DeleteProcThreadAttributeList(attributes);
    if (!created) {
        error = L"CreateProcessW failed: " + win32Error(0);
        closeHandle(job);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        return false;
    }
    if (!::AssignProcessToJobObject(job, processInfo.hProcess)) {
        error = L"AssignProcessToJobObject failed; the shell was not started in an uncontained process: " +
            win32Error(0);
        ::TerminateProcess(processInfo.hProcess, ERROR_ACCESS_DENIED);
        ::WaitForSingleObject(processInfo.hProcess, 2000);
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(job);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        return false;
    }
#ifdef NPPTERMINAL_TESTS
    HANDLE blockedInputRead = nullptr;
    HANDLE blockedInputWrite = nullptr;
    HANDLE originalInputWrite = nullptr;
    if (options.testBlockInputWrite) {
        // Keep the read endpoint open in the broker without ever reading it.
        // This is a test-only transport replacement: the real ConPTY and
        // shell remain alive, while the input worker exercises cancellation
        // of an actual synchronous WriteFile blocked by a small pipe buffer.
        if (!::CreatePipe(&blockedInputRead, &blockedInputWrite,
            &securityAttributes, 4096) ||
            !::SetHandleInformation(blockedInputRead,
                HANDLE_FLAG_INHERIT, 0) ||
            !::SetHandleInformation(blockedInputWrite, HANDLE_FLAG_INHERIT, 0)) {
            error = L"Unable to create the blocked input test pipe: " + win32Error(0);
            closeHandle(blockedInputRead);
            closeHandle(blockedInputWrite);
            ::TerminateProcess(processInfo.hProcess, ERROR_INVALID_HANDLE);
            ::WaitForSingleObject(processInfo.hProcess, 2000);
            closeHandle(processInfo.hThread);
            closeHandle(processInfo.hProcess);
            closeHandle(job);
            closePseudoConsole_(pseudoConsole);
            closeHandle(inputWrite);
            closeHandle(outputRead);
            return false;
        }
    }
#endif
#ifdef NPPTERMINAL_TESTS
    if (options.testBlockInputWrite) {
        originalInputWrite = inputWrite;
        inputWrite = blockedInputWrite;
        blockedInputWrite = nullptr;
    }
#endif
    if (::ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        error = L"ResumeThread failed: " + win32Error(0);
#ifdef NPPTERMINAL_TESTS
        closeHandle(blockedInputRead);
        closeHandle(blockedInputWrite);
        closeHandle(originalInputWrite);
#endif
        ::TerminateJobObject(job, ERROR_PROCESS_ABORTED);
        ::WaitForSingleObject(processInfo.hProcess, 2000);
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(job);
        closePseudoConsole_(pseudoConsole);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        return false;
    }
    closeHandle(processInfo.hThread);
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        process_ = processInfo.hProcess;
        processId_ = processInfo.dwProcessId;
        job_ = job;
        inputWrite_ = inputWrite;
        outputRead_ = outputRead;
        pseudoConsole_ = pseudoConsole;
#ifdef NPPTERMINAL_TESTS
        testOriginalInputWrite_ = originalInputWrite;
        testBlockedInputRead_ = blockedInputRead;
#endif
    }
    return true;
}

void BrokerSession::emitState(SessionState state, const std::wstring& message)
{
    BrokerFrame frame;
    frame.type = BrokerFrameType::State;
    frame.generation = generation_.load();
    frame.id = processId_;
    frame.flags = static_cast<std::uint32_t>(state);
    frame.payload = brokerUtf8(message);
    writeFrame(eventWrite_, &eventWriteMutex_, frame);
}

void BrokerSession::emitInputAck(std::uint64_t generation, std::uint64_t id)
{
    BrokerFrame frame;
    frame.type = BrokerFrameType::InputAck;
    frame.generation = generation;
    frame.id = id;
    writeFrame(eventWrite_, &eventWriteMutex_, frame);
}

void BrokerSession::emitInputWriteStarted(std::uint64_t generation, std::uint64_t id)
{
#ifdef NPPTERMINAL_TESTS
    BrokerFrame frame;
    frame.type = BrokerFrameType::InputWriteStarted;
    frame.generation = generation;
    frame.id = id;
    writeFrame(eventWrite_, &eventWriteMutex_, frame);
#else
    (void)generation;
    (void)id;
#endif
}

void BrokerSession::emitInputWriteFinished(std::uint64_t generation, std::uint64_t id,
    bool success)
{
#ifdef NPPTERMINAL_TESTS
    BrokerFrame frame;
    frame.type = BrokerFrameType::InputWriteFinished;
    frame.generation = generation;
    frame.id = id;
    frame.flags = success ? 1u : 0u;
    writeFrame(eventWrite_, &eventWriteMutex_, frame);
#else
    (void)generation;
    (void)id;
    (void)success;
#endif
}

void BrokerSession::signalStop()
{
    stopRequested_.store(true);
    HANDLE stop = nullptr;
    HANDLE wake = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        stop = stopEvent_;
        wake = inputWakeEvent_;
    }
    if (stop) ::SetEvent(stop);
    if (wake) ::SetEvent(wake);
    inputCondition_.notify_all();
}

void BrokerSession::terminateJob()
{
    std::lock_guard<std::mutex> lock(resourceMutex_);
    if (job_) {
        ::TerminateJobObject(job_, ERROR_PROCESS_ABORTED);
    } else if (process_) {
        ::TerminateProcess(process_, ERROR_PROCESS_ABORTED);
    }
}

void BrokerSession::cancelInputIo()
{
    if (inputThread_.joinable()) {
        ::CancelSynchronousIo(static_cast<HANDLE>(inputThread_.native_handle()));
    }
}

void BrokerSession::requestTransportClose()
{
    HANDLE inputWrite = nullptr;
#ifdef NPPTERMINAL_TESTS
    HANDLE originalInputWrite = nullptr;
    HANDLE blockedInputRead = nullptr;
#endif
    HPCON pseudoConsole = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        inputWrite = inputWrite_;
        inputWrite_ = nullptr;
#ifdef NPPTERMINAL_TESTS
        originalInputWrite = testOriginalInputWrite_;
        testOriginalInputWrite_ = nullptr;
        blockedInputRead = testBlockedInputRead_;
        testBlockedInputRead_ = nullptr;
#endif
        pseudoConsole = pseudoConsole_;
        pseudoConsole_ = nullptr;
    }
    closeHandle(inputWrite);
#ifdef NPPTERMINAL_TESTS
    closeHandle(originalInputWrite);
    closeHandle(blockedInputRead);
#endif
    if (pseudoConsole && closePseudoConsole_) closePseudoConsole_(pseudoConsole);
}

void BrokerSession::waitWorker(HANDLE doneEvent, std::thread& worker)
{
    if (!worker.joinable()) return;
    if (!doneEvent) {
        ::CancelSynchronousIo(static_cast<HANDLE>(worker.native_handle()));
        ::TerminateProcess(::GetCurrentProcess(), ERROR_INVALID_HANDLE);
        return;
    }
    if (::WaitForSingleObject(doneEvent, kWorkerCancelWaitMs) == WAIT_TIMEOUT) {
        ::CancelSynchronousIo(static_cast<HANDLE>(worker.native_handle()));
        if (::WaitForSingleObject(doneEvent, 500) == WAIT_TIMEOUT) {
            // The helper is deliberately disposable.  A stuck native syscall
            // must never keep the host-side facade or this process alive.
            ::TerminateProcess(::GetCurrentProcess(), ERROR_TIMEOUT);
            return;
        }
    }
    worker.join();
}

void BrokerSession::shutdownWorkersBounded()
{
    signalStop();
    terminateJob();

    // The input worker may be in a synchronous ConPTY write.  Cancel it and
    // wait before closing the transport it owns.
    cancelInputIo();
    waitWorker(inputDoneEvent_, inputThread_);

    // Closing the input side and the pseudoconsole is the EOF fence for the
    // output reader.  It must happen only after the input worker has left.
    requestTransportClose();
    waitWorker(outputDoneEvent_, outputThread_);

    // A synchronous command read has no event-driven cancellation.  Cancel
    // the owning thread while its handle is still valid, then join through the
    // bounded helper.  If the OS still refuses to return, the disposable
    // helper terminates itself instead of unwinding with a joinable thread.
    if (commandThread_.joinable()) {
        ::CancelSynchronousIo(static_cast<HANDLE>(commandThread_.native_handle()));
        waitWorker(commandDoneEvent_, commandThread_);
    }
}

void BrokerSession::closeResources()
{
    HANDLE outputRead = nullptr;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE processThread = nullptr;
    HANDLE stop = nullptr;
    HANDLE wake = nullptr;
    HANDLE outputDone = nullptr;
    HANDLE inputDone = nullptr;
    HANDLE commandDone = nullptr;
    HANDLE parentDisconnected = nullptr;
    HANDLE resize = nullptr;
#ifdef NPPTERMINAL_TESTS
    HANDLE originalInputWrite = nullptr;
    HANDLE blockedInputRead = nullptr;
#endif
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        outputRead = outputRead_;
        outputRead_ = nullptr;
        process = process_;
        process_ = nullptr;
        processId_ = 0;
        job = job_;
        job_ = nullptr;
        processThread = processThread_;
        processThread_ = nullptr;
        stop = stopEvent_;
        stopEvent_ = nullptr;
        wake = inputWakeEvent_;
        inputWakeEvent_ = nullptr;
        outputDone = outputDoneEvent_;
        outputDoneEvent_ = nullptr;
        inputDone = inputDoneEvent_;
        inputDoneEvent_ = nullptr;
        commandDone = commandDoneEvent_;
        commandDoneEvent_ = nullptr;
        parentDisconnected = parentDisconnectedEvent_;
        parentDisconnectedEvent_ = nullptr;
        resize = resizeEvent_;
        resizeEvent_ = nullptr;
#ifdef NPPTERMINAL_TESTS
        originalInputWrite = testOriginalInputWrite_;
        testOriginalInputWrite_ = nullptr;
        blockedInputRead = testBlockedInputRead_;
        testBlockedInputRead_ = nullptr;
#endif
    }
    closeHandle(outputRead);
    closeHandle(processThread);
    closeHandle(process);
    closeHandle(job);
    closeHandle(stop);
    closeHandle(wake);
    closeHandle(outputDone);
    closeHandle(inputDone);
    closeHandle(commandDone);
    closeHandle(parentDisconnected);
    closeHandle(resize);
#ifdef NPPTERMINAL_TESTS
    closeHandle(originalInputWrite);
    closeHandle(blockedInputRead);
#endif
}

void BrokerSession::commandLoop()
{
#ifdef NPPTERMINAL_TESTS
    if (testPauseCommandReader_) {
        // Deliberately leave the inherited command endpoint unread.  The
        // host-side command write must remain cancelable when its bounded
        // pipe buffer is full; the outer job is the only exit for this test.
        for (;;) ::Sleep(1000);
    }
#endif
    for (;;) {
        BrokerFrame frame;
        if (!readFrame(frame)) {
            commandDone_.store(true);
            HANDLE disconnected = nullptr;
            {
                std::lock_guard<std::mutex> lock(resourceMutex_);
                disconnected = parentDisconnectedEvent_;
                if (commandDoneEvent_) ::SetEvent(commandDoneEvent_);
            }
            if (disconnected) ::SetEvent(disconnected);
            signalStop();
            return;
        }
        const std::uint64_t currentGeneration = generation_.load();
        if (frame.generation != 0 && frame.generation != currentGeneration) continue;
        switch (frame.type) {
            case BrokerFrameType::Input: {
                if (frame.payload.empty() || frame.payload.size() > kMaxInputBytes) {
                    BrokerFrame error;
                    error.type = BrokerFrameType::Error;
                    error.generation = currentGeneration;
                    error.payload = brokerUtf8(L"The broker received an invalid input frame.");
                    writeFrame(eventWrite_, &eventWriteMutex_, error);
                    signalStop();
                    return;
                }
                std::lock_guard<std::mutex> lock(inputMutex_);
                if (stopRequested_.load() || inputBytes_ + frame.payload.size() >
                    kMaxQueuedInputBytes) {
                    BrokerFrame error;
                    error.type = BrokerFrameType::Error;
                    error.generation = currentGeneration;
                    error.payload = brokerUtf8(L"The broker input queue exceeded its bound.");
                    writeFrame(eventWrite_, &eventWriteMutex_, error);
                    signalStop();
                    return;
                }
                inputQueue_.push_back(InputItem{currentGeneration, frame.id,
                    std::move(frame.payload)});
                inputBytes_ += inputQueue_.back().data.size();
                HANDLE wake = nullptr;
                {
                    std::lock_guard<std::mutex> resourceLock(resourceMutex_);
                    wake = inputWakeEvent_;
                }
                if (wake) ::SetEvent(wake);
                inputCondition_.notify_one();
                break;
            }
            case BrokerFrameType::Resize: {
                std::uint16_t columns = 0;
                std::uint16_t rows = 0;
                if (!decodeBrokerResize(frame.payload, columns, rows)) break;
                {
                    std::lock_guard<std::mutex> lock(resizeMutex_);
                    pendingResizeColumns_ = columns;
                    pendingResizeRows_ = rows;
                }
                HANDLE resize = nullptr;
                {
                    std::lock_guard<std::mutex> lock(resourceMutex_);
                    resize = resizeEvent_;
                }
                if (resize) ::SetEvent(resize);
                break;
            }
            case BrokerFrameType::Stop:
#ifdef NPPTERMINAL_TESTS
                if (testStallOnStop_) {
                    stallOnStop_.store(true);
                    for (;;) ::Sleep(1000);
                }
#endif
                signalStop();
                commandDone_.store(true);
                {
                    std::lock_guard<std::mutex> lock(resourceMutex_);
                    if (commandDoneEvent_) ::SetEvent(commandDoneEvent_);
                }
                return;
            case BrokerFrameType::TestStallOnStop:
#ifdef NPPTERMINAL_TESTS
                testStallOnStop_ = true;
#endif
                break;
            default:
                break;
        }
    }
}

void BrokerSession::inputLoop()
{
    for (;;) {
        HANDLE events[2] = {};
        {
            std::lock_guard<std::mutex> lock(resourceMutex_);
            events[0] = stopEvent_;
            events[1] = inputWakeEvent_;
        }
        if (!events[0] || !events[1]) break;
        const DWORD wait = ::WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;

        for (;;) {
            InputItem item;
            {
                std::lock_guard<std::mutex> lock(inputMutex_);
                if (inputQueue_.empty()) {
                    std::lock_guard<std::mutex> resourceLock(resourceMutex_);
                    if (inputWakeEvent_) ::ResetEvent(inputWakeEvent_);
                    break;
                }
                item = std::move(inputQueue_.front());
                inputQueue_.pop_front();
            }
            HANDLE input = nullptr;
            {
                std::lock_guard<std::mutex> lock(resourceMutex_);
                input = inputWrite_;
            }
            if (!input) {
                std::lock_guard<std::mutex> lock(inputMutex_);
                inputBytes_ -= std::min(inputBytes_, item.data.size());
                break;
            }
            emitInputWriteStarted(item.generation, item.id);
            DWORD written = 0;
            const BOOL success = ::WriteFile(input, item.data.data(),
                static_cast<DWORD>(item.data.size()), &written, nullptr);
            const bool complete = success != FALSE && written == item.data.size();
            {
                std::lock_guard<std::mutex> lock(inputMutex_);
                // Keep the active synchronous write charged until it leaves
                // WriteFile.  This makes the command reader's 64 KiB bound
                // cover both queued and in-flight input.
                inputBytes_ -= std::min(inputBytes_, item.data.size());
            }
            emitInputWriteFinished(item.generation, item.id, complete);
            if (!complete) {
                if (!stopRequested_.load()) signalStop();
                break;
            }
            if (!stopRequested_.load() && item.id != 0) {
                emitInputAck(item.generation, item.id);
            }
        }
    }
    inputDone_.store(true);
    HANDLE done = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        done = inputDoneEvent_;
    }
    if (done) ::SetEvent(done);
}

void BrokerSession::outputLoop()
{
    std::vector<std::uint8_t> buffer(kMaxOutputChunkBytes);
    HANDLE output = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        output = outputRead_;
    }
    // Keep reading after a stop request.  The controller closes the
    // pseudoconsole only after the input worker has left; that close is what
    // supplies EOF and lets already buffered final shell bytes reach the
    // host in order.
    while (output) {
        DWORD bytesRead = 0;
        if (!::ReadFile(output, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead,
            nullptr) || bytesRead == 0) break;
        BrokerFrame frame;
        frame.type = BrokerFrameType::Output;
        frame.generation = generation_.load();
        frame.id = outputId_.fetch_add(1) + 1;
        frame.payload.assign(buffer.begin(), buffer.begin() + bytesRead);
        if (!writeFrame(outputWrite_, nullptr, frame)) {
            signalStop();
            break;
        }
    }
    outputDone_.store(true);
    HANDLE done = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        done = outputDoneEvent_;
    }
    if (done) ::SetEvent(done);
}

bool BrokerSession::applyResize(std::uint16_t columns, std::uint16_t rows)
{
    HPCON pseudoConsole = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        pseudoConsole = pseudoConsole_;
        resize = resizePseudoConsole_;
    }
    if (!pseudoConsole || !resize) return false;
    return SUCCEEDED(resize(pseudoConsole,
        COORD{static_cast<SHORT>(columns), static_cast<SHORT>(rows)}));
}

void BrokerSession::controlLoop()
{
    HANDLE process = nullptr;
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        process = process_;
    }
    bool naturalExit = false;
    while (process && !stopRequested_.load()) {
        HANDLE events[4] = {};
        {
            std::lock_guard<std::mutex> lock(resourceMutex_);
            events[0] = process_;
            events[1] = stopEvent_;
            events[2] = resizeEvent_;
            events[3] = parentDisconnectedEvent_;
        }
        if (!events[0]) break;
        const DWORD wait = ::WaitForMultipleObjects(4, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            naturalExit = true;
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_OBJECT_0 + 3) break;
        if (wait == WAIT_OBJECT_0 + 2) {
            std::uint16_t columns = 0;
            std::uint16_t rows = 0;
            {
                std::lock_guard<std::mutex> lock(resizeMutex_);
                columns = pendingResizeColumns_;
                rows = pendingResizeRows_;
                pendingResizeColumns_ = 0;
                pendingResizeRows_ = 0;
                std::lock_guard<std::mutex> resourceLock(resourceMutex_);
                if (resizeEvent_) ::ResetEvent(resizeEvent_);
            }
            if (columns != 0 && rows != 0) applyResize(columns, rows);
        }
    }

    naturalExit_.store(naturalExit && !stopRequested_.load());
    shutdownWorkersBounded();

    // The command reader is now joined while its handle is still valid.
    // Closing a synchronous pipe handle from another thread is not a reliable
    // cancellation primitive on every supported Windows build.
    HANDLE command = commandRead_;
    commandRead_ = nullptr;
    closeHandle(command);

    const SessionState finalState = naturalExit_.load() ? SessionState::Exited :
        SessionState::NoSession;
    emitState(finalState, naturalExit_.load()
        ? L"The shell exited; press Restart to start a new session."
        : L"Terminal stopped");
}

int BrokerSession::run()
{
    BrokerFrame startFrame;
    if (!readFrame(startFrame) || startFrame.type != BrokerFrameType::Start ||
        startFrame.generation == 0) {
        return 2;
    }
    generation_.store(startFrame.generation);
    BrokerStartData options;
    std::wstring error;
    if (!decodeBrokerStart(startFrame.payload, options, error) ||
        !loadConPtyFunctions(error)) {
        BrokerFrame frame;
        frame.type = BrokerFrameType::State;
        frame.generation = generation_.load();
        frame.flags = static_cast<std::uint32_t>(SessionState::Error);
        frame.payload = brokerUtf8(error.empty() ? L"Unable to start the terminal." : error);
        writeFrame(eventWrite_, &eventWriteMutex_, frame);
        closeResources();
        return 1;
    }
#ifdef NPPTERMINAL_TESTS
    testStallOnStop_ = options.testStallOnStop;
    testPauseCommandReader_ = options.testPauseCommandReader;
#endif
    {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        inputWakeEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        outputDoneEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        inputDoneEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        commandDoneEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        parentDisconnectedEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        resizeEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    HANDLE events[] = {stopEvent_, inputWakeEvent_, outputDoneEvent_, inputDoneEvent_,
        commandDoneEvent_, parentDisconnectedEvent_, resizeEvent_};
    for (HANDLE event : events) {
        if (!event) {
            error = L"Unable to create broker synchronization events: " + win32Error(0);
            closeResources();
            return 1;
        }
    }
    if (!createProcess(options, error)) {
        BrokerFrame frame;
        frame.type = BrokerFrameType::State;
        frame.generation = generation_.load();
        frame.flags = static_cast<std::uint32_t>(SessionState::Error);
        frame.payload = brokerUtf8(error);
        writeFrame(eventWrite_, &eventWriteMutex_, frame);
        closeResources();
        return 1;
    }

    try {
#ifdef NPPTERMINAL_TESTS
        if (options.testFailThreadStartIndex == 1) throw 1;
#endif
        commandThread_ = std::thread(&BrokerSession::commandLoop, this);
#ifdef NPPTERMINAL_TESTS
        if (options.testFailThreadStartIndex == 2) throw 1;
#endif
        inputThread_ = std::thread(&BrokerSession::inputLoop, this);
#ifdef NPPTERMINAL_TESTS
        if (options.testFailThreadStartIndex == 3) throw 1;
#endif
        outputThread_ = std::thread(&BrokerSession::outputLoop, this);
    } catch (...) {
        BrokerFrame frame;
        frame.type = BrokerFrameType::State;
        frame.generation = generation_.load();
        frame.flags = static_cast<std::uint32_t>(SessionState::Error);
        frame.payload = brokerUtf8(L"Unable to create broker worker threads.");
        writeFrame(eventWrite_, &eventWriteMutex_, frame);
        shutdownWorkersBounded();
        closeResources();
        return 1;
    }
    emitState(SessionState::Running, L"Terminal is running");
    controlLoop();
    return 0;
}

} // namespace nppterminal
