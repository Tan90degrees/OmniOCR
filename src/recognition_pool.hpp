#pragma once
#include "omniocr/core.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace omniocr {
// Separate preparation from blocking model calls. The limits cover all
// admitted crops, including tasks running or waiting for a model lease.
class RecognitionTaskPool {
    struct Task {
        std::function<void()> work;
        std::function<void()> finished;
        size_t bytes;
        std::chrono::steady_clock::time_point queued_at;
    };
    std::mutex mutex_;
    std::condition_variable ready_, capacity_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    size_t max_requests_, max_bytes_, requests_ = 0, bytes_ = 0, running_ = 0;
    size_t peak_requests_ = 0, peak_bytes_ = 0, peak_queued_ = 0;
    uint64_t submitted_ = 0, completed_ = 0, failed_ = 0, cancelled_ = 0;
    uint64_t timeouts_ = 0, oversized_ = 0;
    double admission_wait_ms_ = 0, queue_wait_ms_ = 0;
    int timeout_ms_;
    bool stopping_ = false;
    void run() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
                ++running_;
                queue_wait_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - task.queued_at).count();
            }
            bool failed = false, cancelled = false;
            try { task.work(); }
            catch (const Cancelled&) { cancelled = true; }
            catch (...) { failed = true; }
            // Release captured images before returning their byte credits.
            task.work = {};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --running_; --requests_; bytes_ -= task.bytes;
                ++completed_;
                if (failed) ++failed_;
                if (cancelled) ++cancelled_;
            }
            capacity_.notify_all();
            task.finished();
        }
    }
public:
    explicit RecognitionTaskPool(const Json& settings)
        : max_requests_(settings.value("max_requests", size_t(256))),
          max_bytes_(settings.value("max_bytes", size_t(268435456))),
          timeout_ms_(settings.value("enqueue_timeout_ms", 600000)) {
        const int count = settings.value("workers", 128);
        try {
            for (int i = 0; i < count; ++i) workers_.emplace_back([this] { run(); });
        } catch (...) {
            { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
            ready_.notify_all();
            for (auto& worker : workers_) worker.join();
            throw;
        }
    }
    ~RecognitionTaskPool() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        ready_.notify_all(); capacity_.notify_all();
        for (auto& worker : workers_) worker.join();
    }
    void submit(size_t bytes, std::function<void()> work,
                const std::function<bool()>& stop, std::function<void()> finished) {
        const auto begin = std::chrono::steady_clock::now();
        const auto deadline = begin + std::chrono::milliseconds(timeout_ms_);
        std::unique_lock<std::mutex> lock(mutex_);
        if (bytes > max_bytes_) {
            ++oversized_;
            throw std::runtime_error("recognition crop exceeds async_recognition.max_bytes");
        }
        while (!stopping_ && !stop() && !cancellation_requested() &&
               (requests_ >= max_requests_ || bytes > max_bytes_ - bytes_)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ++timeouts_;
                throw std::runtime_error("async recognition queue admission timed out");
            }
            capacity_.wait_until(lock, std::min(deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
        }
        throw_if_cancelled();
        if (stop()) throw Cancelled();
        if (stopping_) throw std::runtime_error("recognition pool is stopping");
        tasks_.push_back({std::move(work), std::move(finished), bytes, std::chrono::steady_clock::now()});
        ++requests_; bytes_ += bytes; ++submitted_;
        peak_requests_ = std::max(peak_requests_, requests_);
        peak_bytes_ = std::max(peak_bytes_, bytes_);
        peak_queued_ = std::max(peak_queued_, tasks_.size());
        admission_wait_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        lock.unlock(); ready_.notify_one();
    }
    Json metrics() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {{"enabled", true}, {"workers", workers_.size()},
            {"max_requests", max_requests_}, {"max_bytes", max_bytes_},
            {"queued", tasks_.size()}, {"running", running_},
            {"admitted", requests_}, {"admitted_bytes", bytes_},
            {"peak_admitted", peak_requests_}, {"peak_admitted_bytes", peak_bytes_},
            {"peak_queued", peak_queued_}, {"submitted_total", submitted_},
            {"completed_total", completed_}, {"failed_total", failed_},
            {"cancelled_total", cancelled_}, {"admission_timeout_total", timeouts_},
            {"oversized_total", oversized_}, {"admission_wait_ms_total", admission_wait_ms_},
            {"queue_wait_ms_total", queue_wait_ms_}};
    }
};
} // namespace omniocr
