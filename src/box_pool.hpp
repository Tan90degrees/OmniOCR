#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace omniocr {
// A fixed set of BOX workers shared by all active pages. Page workers wait
// for their own futures; they never consume tasks from this pool.
class BoxTaskPool {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::packaged_task<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;

    void run() {
        while (true) {
            std::packaged_task<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
public:
    explicit BoxTaskPool(int count) {
        try {
            for (int i = 0; i < count; ++i)
                workers_.emplace_back([this] { run(); });
        } catch (...) {
            { std::lock_guard<std::mutex> guard(mutex_); stopping_ = true; }
            ready_.notify_all();
            for (auto& worker : workers_) worker.join();
            throw;
        }
    }
    BoxTaskPool(const BoxTaskPool&) = delete;
    BoxTaskPool& operator=(const BoxTaskPool&) = delete;
    ~BoxTaskPool() {
        { std::lock_guard<std::mutex> guard(mutex_); stopping_ = true; }
        ready_.notify_all();
        for (auto& worker : workers_) worker.join();
    }
    std::future<void> submit(std::function<void()> work) {
        std::packaged_task<void()> task(std::move(work));
        auto result = task.get_future();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopping_) throw std::runtime_error("BOX pool is stopping");
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return result;
    }
};
} // namespace omniocr
