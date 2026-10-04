#include "Bridge.h"

namespace nppterminal {

void BoundedOutputBridge::attachDispatcher(HWND window, UINT message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = window;
    dispatchMessage_ = message;
}

void BoundedOutputBridge::beginGeneration(std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_ = generation;
        cancelled_ = false;
        queue_.clear();
        inFlight_.clear();
        queuedBytes_ = 0;
        inFlightBytes_ = 0;
        dispatchPosted_.store(false);
    }
}

bool BoundedOutputBridge::tryPush(OutputChunk chunk)
{
    if (chunk.data.empty() || chunk.data.size() > kQueueLimitBytes) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_ || chunk.generation != generation_ ||
            queuedBytes_ + chunk.data.size() > kQueueLimitBytes) {
            return false;
        }
        queuedBytes_ += chunk.data.size();
        queue_.push_back(std::move(chunk));
    }
    notifyDispatcher();
    return true;
}

std::optional<OutputChunk> BoundedOutputBridge::takeForSend()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queueHeadFitsLocked()) return std::nullopt;

    OutputChunk chunk = std::move(queue_.front());
    queue_.pop_front();
    queuedBytes_ -= chunk.data.size();
    inFlightBytes_ += chunk.data.size();
    inFlight_.insert_or_assign(chunk.id, chunk.data.size());
    return chunk;
}

void BoundedOutputBridge::returnUnsent(OutputChunk chunk)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = inFlight_.find(chunk.id);
        if (it != inFlight_.end()) {
            inFlightBytes_ -= it->second;
            inFlight_.erase(it);
        }
        if (chunk.generation == generation_ && !cancelled_ &&
            queuedBytes_ + chunk.data.size() <= kQueueLimitBytes) {
            queuedBytes_ += chunk.data.size();
            queue_.push_front(std::move(chunk));
        }
    }
    notifyDispatcher();
}

void BoundedOutputBridge::acknowledge(std::uint64_t generation, std::uint64_t id)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return;
        const auto it = inFlight_.find(id);
        if (it == inFlight_.end()) return;
        inFlightBytes_ -= it->second;
        inFlight_.erase(it);
    }
    notifyDispatcher();
}

bool BoundedOutputBridge::queueHeadFitsLocked() const
{
    return !queue_.empty() && inFlightBytes_ < kInFlightLimitBytes &&
        queue_.front().data.size() <= kInFlightLimitBytes - inFlightBytes_;
}

void BoundedOutputBridge::onDispatchHandled()
{
    HWND dispatcher = nullptr;
    UINT message = 0;
    bool repost = false;
    {
        // Clear the posted bit and inspect the queue under the same mutex
        // producers use. This closes the handoff race where a producer pushes
        // between the UI pump's last pop and clearing dispatchPosted_.
        std::lock_guard<std::mutex> lock(mutex_);
        dispatchPosted_.store(false);
        if (!cancelled_ && queueHeadFitsLocked()) {
            dispatchPosted_.store(true);
            dispatcher = dispatcher_;
            message = dispatchMessage_;
            repost = true;
        }
    }
    if (repost && (!dispatcher || !message || !::PostMessageW(dispatcher, message, 0, 0))) {
        dispatchPosted_.store(false);
    }
}

void BoundedOutputBridge::cancel()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        queue_.clear();
        inFlight_.clear();
        queuedBytes_ = 0;
        inFlightBytes_ = 0;
    }
}

void BoundedOutputBridge::clear()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        inFlight_.clear();
        queuedBytes_ = 0;
        inFlightBytes_ = 0;
    }
}

std::size_t BoundedOutputBridge::queuedBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queuedBytes_;
}

std::size_t BoundedOutputBridge::inFlightBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightBytes_;
}

std::uint64_t BoundedOutputBridge::generation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

void BoundedOutputBridge::notifyDispatcher()
{
    HWND dispatcher = nullptr;
    UINT message = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dispatchPosted_.load() || cancelled_ || !queueHeadFitsLocked()) return;
        dispatchPosted_.store(true);
        dispatcher = dispatcher_;
        message = dispatchMessage_;
    }
    if (!dispatcher || !message || !::PostMessageW(dispatcher, message, 0, 0)) {
        dispatchPosted_.store(false);
    }
}

} // namespace nppterminal
