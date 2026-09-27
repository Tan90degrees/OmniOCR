#pragma once
#include <atomic>
#include <memory>
#include <stdexcept>

namespace omniocr {
using CancellationToken = std::shared_ptr<std::atomic<bool>>;
class Cancelled final : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("job cancelled") {}
};
// Scoped context propagates a job token across synchronous model/document
// calls. Worker threads must establish their own scope explicitly.
inline thread_local const std::atomic<bool>* current_cancellation = nullptr;
class CancellationScope {
    const std::atomic<bool>* previous_;
public:
    explicit CancellationScope(const std::atomic<bool>* token)
        : previous_(current_cancellation) { current_cancellation = token; }
    ~CancellationScope() { current_cancellation = previous_; }
    CancellationScope(const CancellationScope&) = delete;
    CancellationScope& operator=(const CancellationScope&) = delete;
};
inline bool cancellation_requested() {
    return current_cancellation && current_cancellation->load(std::memory_order_relaxed);
}
inline void throw_if_cancelled() { if (cancellation_requested()) throw Cancelled(); }
} // namespace omniocr
