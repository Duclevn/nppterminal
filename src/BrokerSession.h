#pragma once

#include "BrokerProtocol.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nppterminal {

enum class SessionState : int;

// This class is compiled into NppTerminalBroker.exe (and its test build),
// never into the plugin DLL.  All ConPTY handles and native worker threads
// therefore die with the helper process if the host disappears.
class BrokerSession final {
public:
    BrokerSession(HANDLE commandRead, HANDLE outputWrite, HANDLE eventWrite);
    ~BrokerSession();

    BrokerSession(const BrokerSession&) = delete;
    BrokerSession& operator=(const BrokerSession&) = delete;

    int run();

private:
    struct InputItem {
        std::uint64_t generation = 0;
        std::uint64_t id = 0;
        std::vector<std::uint8_t> data;
    };

    using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
    using ClosePseudoConsoleFn = VOID(WINAPI*)(HPCON);

    bool readFrame(BrokerFrame& frame);
    bool readExact(void* bytes, DWORD count);
    bool writeFrame(HANDLE pipe, std::mutex* writeMutex, const BrokerFrame& frame);
    bool writeExact(HANDLE pipe, const std::uint8_t* bytes, DWORD count);

    bool loadConPtyFunctions(std::wstring& error);
    bool createProcess(const BrokerStartData& options, std::wstring& error);
    void commandLoop();
    void inputLoop();
    void outputLoop();
    void controlLoop();

    bool applyResize(std::uint16_t columns, std::uint16_t rows);
    void emitState(SessionState state, const std::wstring& message);
    void emitInputAck(std::uint64_t generation, std::uint64_t id);
    void emitInputWriteStarted(std::uint64_t generation, std::uint64_t id);
    void emitInputWriteFinished(std::uint64_t generation, std::uint64_t id, bool success);
    void terminateJob();
    void requestTransportClose();
    void cancelInputIo();
    void signalStop();
    void shutdownWorkersBounded();
    void waitWorker(HANDLE doneEvent, std::thread& worker);
    void closeResources();
    static std::wstring win32Error(DWORD code);

    HANDLE commandRead_ = nullptr;
    HANDLE outputWrite_ = nullptr;
    HANDLE eventWrite_ = nullptr;
    std::mutex eventWriteMutex_;

    mutable std::mutex resourceMutex_;
    HANDLE process_ = nullptr;
    HANDLE processThread_ = nullptr;
    DWORD processId_ = 0;
    HANDLE job_ = nullptr;
    HANDLE inputWrite_ = nullptr;
    HANDLE outputRead_ = nullptr;
    HPCON pseudoConsole_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE inputWakeEvent_ = nullptr;
    HANDLE outputDoneEvent_ = nullptr;
    HANDLE inputDoneEvent_ = nullptr;
    HANDLE commandDoneEvent_ = nullptr;
    HANDLE parentDisconnectedEvent_ = nullptr;
    HANDLE resizeEvent_ = nullptr;

    CreatePseudoConsoleFn createPseudoConsole_ = nullptr;
    ResizePseudoConsoleFn resizePseudoConsole_ = nullptr;
    ClosePseudoConsoleFn closePseudoConsole_ = nullptr;

    std::thread commandThread_;
    std::thread inputThread_;
    std::thread outputThread_;

    mutable std::mutex inputMutex_;
    std::condition_variable inputCondition_;
    std::deque<InputItem> inputQueue_;
    std::size_t inputBytes_ = 0;

    mutable std::mutex resizeMutex_;
    std::uint16_t pendingResizeColumns_ = 0;
    std::uint16_t pendingResizeRows_ = 0;

    std::atomic_bool stopRequested_{false};
    std::atomic_bool naturalExit_{false};
    std::atomic_bool outputDone_{false};
    std::atomic_bool inputDone_{false};
    std::atomic_bool commandDone_{false};
#ifdef NPPTERMINAL_TESTS
    std::atomic_bool stallOnStop_{false};
#endif
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> outputId_{0};
#ifdef NPPTERMINAL_TESTS
    bool testStallOnStop_ = false;
    bool testPauseCommandReader_ = false;
    // The blocked-input fixture keeps the real ConPTY write endpoint alive
    // until teardown so the pseudo console does not observe input EOF during
    // shell startup. The worker writes to the separate controlled pipe.
    HANDLE testOriginalInputWrite_ = nullptr;
    HANDLE testBlockedInputRead_ = nullptr;
#endif
};

} // namespace nppterminal
