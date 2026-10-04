#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nppterminal {

struct OutputChunk {
    std::uint64_t generation = 0;
    std::uint64_t id = 0;
    std::vector<std::uint8_t> data;
};

class BoundedOutputBridge final {
public:
    static constexpr std::size_t kQueueLimitBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t kInFlightLimitBytes = 256u * 1024u;

    BoundedOutputBridge() = default;
    BoundedOutputBridge(const BoundedOutputBridge&) = delete;
    BoundedOutputBridge& operator=(const BoundedOutputBridge&) = delete;

    void attachDispatcher(HWND window, UINT message);
    void beginGeneration(std::uint64_t generation);
    // Enqueue from the UI-owned session pump without waiting for renderer
    // acknowledgements. A false result means that this chunk must remain at
    // the session boundary for a later poll; it is not a session cancellation.
    bool tryPush(OutputChunk chunk);
    std::optional<OutputChunk> takeForSend();
    void returnUnsent(OutputChunk chunk);
    void acknowledge(std::uint64_t generation, std::uint64_t id);
    void onDispatchHandled();
    void cancel();
    void clear();
    std::size_t queuedBytes() const;
    std::size_t inFlightBytes() const;
    std::uint64_t generation() const;

private:
    bool queueHeadFitsLocked() const;
    void notifyDispatcher();

    mutable std::mutex mutex_;
    std::deque<OutputChunk> queue_;
    std::unordered_map<std::uint64_t, std::size_t> inFlight_;
    std::size_t queuedBytes_ = 0;
    std::size_t inFlightBytes_ = 0;
    std::uint64_t generation_ = 0;
    HWND dispatcher_ = nullptr;
    UINT dispatchMessage_ = 0;
    bool cancelled_ = false;
    std::atomic_bool dispatchPosted_{false};
};

} // namespace nppterminal
