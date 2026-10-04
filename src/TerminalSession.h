#pragma once

#include "BrokerProtocol.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nppterminal {

enum class SessionState : int {
    NoSession = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Exited = 4,
    Error = 5,
};

enum class StopReason {
    UserKill,
    Restart,
    PanelDestroy,
    HostShutdown,
};

struct SessionStartOptions {
    std::wstring applicationName;
    std::wstring commandLine;
    std::wstring workingDirectory;
    std::uint16_t columns = 80;
    std::uint16_t rows = 24;
    // If non-zero, the page generation that requested this start must match
    // the generation allocated by the session. This prevents a stale ready
    // event from starting a replacement process.
    std::uint64_t expectedGeneration = 0;
    // Set by shell discovery for Git Bash.  The broker applies the fixed
    // CHERE_INVOKING=1 override only to the child environment.
    bool gitBashPreserveDirectory = false;
#ifdef NPPTERMINAL_TESTS
    // Test-only helper fault injection.  The broker stops servicing its
    // command channel after receiving Stop so the DLL's forced-job path can
    // be measured without exposing a production command.
    bool testStallOnStop = false;
    // Test-only command reader pause used to prove cancellation of a full
    // overlapped host-to-broker write.
    bool testPauseCommandReader = false;
    // Test-only broker input transport replacement used to prove cancellation
    // of a synchronous ConPTY input write under a controlled blocked pipe.
    bool testBlockInputWrite = false;
    // Test-only helper fault injection.  The broker throws before creating
    // worker 1, 2, or 3 when this is set to that one-based index.
    int testFailThreadStartIndex = 0;
#endif
};

struct SessionCallbacks {
    std::function<bool(std::uint64_t, std::uint64_t, const std::vector<std::uint8_t>&,
        const std::atomic_bool&)> output;
    std::function<void(std::uint64_t, SessionState, const std::wstring&)> state;
    std::function<void(std::uint64_t, std::uint64_t)> inputAck;
#ifdef NPPTERMINAL_TESTS
    // Test-only observation points for proving synchronous input cancellation
    // reached the broker's WriteFile call. Production callbacks do not expose
    // these events.
    std::function<void(std::uint64_t, std::uint64_t)> inputWriteStarted;
    std::function<void(std::uint64_t, std::uint64_t, bool)> inputWriteFinished;
    // Test-only policy hook.  Returning true postpones host observation of a
    // canceled command write; CancelIoEx still runs and the raw allocation and
    // event remain owned until a later poll observes completion.  This proves
    // retention and reclamation policy without changing kernel I/O ownership.
    std::function<bool()> testHoldCommandCancellationObservation;
#endif
};

class TerminalSession final {
public:
    // Mutating methods and callbacks are caller-thread affine.  The host
    // invokes them from its UI/event thread; no method may be called
    // concurrently with poll or another mutating method.
    explicit TerminalSession(SessionCallbacks callbacks = {});
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    bool start(const SessionStartOptions& options, std::wstring& error);
    void requestStop(StopReason reason = StopReason::UserKill);
    bool waitForStop(DWORD timeoutMs = INFINITE);
    void stop(StopReason reason = StopReason::UserKill);
    bool resize(std::uint16_t columns, std::uint16_t rows);
    bool write(const std::vector<std::uint8_t>& bytes);
    bool write(std::uint64_t generation, std::uint64_t id,
        const std::vector<std::uint8_t>& bytes);

    // The facade has no worker thread.  The host calls poll from its UI/event
    // loop; all output, state, acknowledgement, and test callbacks happen in
    // that call.  Each invocation performs only bounded pipe work.
    void poll();
    bool hasPendingWork() const;
#ifdef NPPTERMINAL_TESTS
    bool hasPendingCommandWrite() const;
#endif

    SessionState state() const;
    std::uint64_t generation() const;
    std::uint64_t nextGeneration() const;
    bool hasProcess() const;
    DWORD processId() const;
    DWORD brokerProcessId() const;

    static bool makeCmdOptions(const std::wstring& workingDirectory,
        std::uint16_t columns, std::uint16_t rows, SessionStartOptions& options,
        std::wstring& error);

private:
    struct QueuedCommand {
        std::vector<std::uint8_t> encoded;
        std::size_t inputBytes = 0;
    };

    struct StateNotification {
        std::uint64_t generation = 0;
        SessionState state = SessionState::NoSession;
        std::wstring message;
    };

    struct BrokerWriteAllocation {
        OVERLAPPED overlapped{};
        std::size_t size = 0;
        std::size_t offset = 0;
        std::size_t inputBytes = 0;
        std::uint64_t generation = 0;
        std::uint64_t id = 0;
        std::uint16_t type = 0;
        bool overlappedPending = false;
        std::uint8_t bytes[36u + 128u * 1024u]{};
    };

    bool createBroker(const SessionStartOptions& options, std::uint64_t generation,
        std::wstring& error);
    bool createPipePair(bool parentWrites, HANDLE& parent, HANDLE& child,
        std::wstring& error, DWORD bufferBytes = 0);
    bool queueFrame(std::uint16_t type, std::uint64_t generation, std::uint64_t id,
        std::uint32_t flags, const std::vector<std::uint8_t>& payload,
        std::size_t inputBytes = 0);
    bool beginCommandWrite();
    bool advanceCommandWrite();
    void abandonCommandWrite();
    void reclaimOrphanedCommandWrite();
    void drainEventPipe();
    void drainOutputPipe();
    bool consumeOutputFrame();
    void dispatchEvent(const BrokerFrame& frame);
    void dispatchState(std::uint64_t generation, SessionState state,
        const std::wstring& message, DWORD shellProcessId);
    void finishTerminalStateIfReady();
    void markTransportFailure(const std::wstring& message);
    void forceTerminateAndRelease();
    void releaseResources(bool killJob);
    void closeHandle(HANDLE& handle);
    bool processSignaled() const;
    static std::wstring win32Error(DWORD code);
    static std::wstring helperPath();

    SessionCallbacks callbacks_;

    mutable std::mutex stateMutex_;
    SessionState state_ = SessionState::NoSession;
    std::uint64_t generation_ = 0;
    DWORD shellProcessId_ = 0;
    DWORD brokerProcessId_ = 0;

    mutable std::mutex resourceMutex_;
    HANDLE brokerProcess_ = nullptr;
    HANDLE outerJob_ = nullptr;
    HANDLE commandPipe_ = nullptr;
    HANDLE outputPipe_ = nullptr;
    HANDLE eventPipe_ = nullptr;
    HANDLE commandWriteEvent_ = nullptr;
    OVERLAPPED commandWriteOverlapped_{};
    BrokerWriteAllocation* pendingCommandWrite_ = nullptr;

    std::deque<QueuedCommand> commandQueue_;
    struct AcceptedInput {
        std::uint64_t generation = 0;
        std::uint64_t id = 0;
        std::size_t bytes = 0;
    };
    std::deque<StateNotification> stateNotifications_;
    std::deque<AcceptedInput> acceptedInputs_;
    std::size_t acceptedInputBytes_ = 0;
    std::vector<std::uint8_t> outputBuffer_;
    std::vector<std::uint8_t> eventBuffer_;
    BrokerFrame* pendingOutputFrame_ = nullptr;
    bool outputPipeClosed_ = false;
    bool eventPipeClosed_ = false;
    bool startCommandQueued_ = false;
    bool stopCommandQueued_ = false;
    bool terminalStateSeen_ = false;
    bool terminalStateDelivered_ = false;
    SessionState pendingTerminalState_ = SessionState::NoSession;
    std::wstring pendingTerminalMessage_;

    std::atomic_bool stopRequested_{false};
    std::atomic_bool transportFailed_{false};
    std::uint64_t nextInputId_ = 0;
    // These are deliberately raw.  If Windows refuses to report cancellation
    // before the pipe handle is released, the allocation and event must stay
    // alive until the kernel signals the overlapped operation.  At most one
    // such ownerless operation may exist for this facade lifetime.
    BrokerWriteAllocation* orphanedCommandWrite_ = nullptr;
    HANDLE orphanedCommandWriteEvent_ = nullptr;
    bool stopDeadlineActive_ = false;
    std::chrono::steady_clock::time_point stopDeadline_{};
};

} // namespace nppterminal
