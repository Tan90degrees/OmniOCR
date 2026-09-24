#include "omniocr/core.hpp"
#include "box_pool.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace omniocr {
std::vector<BatchResult> Pipeline::run_batch(const std::vector<BatchJob>& jobs,
                                             BatchOptions options) {
    if (jobs.empty()) return {};
    const auto& execution = config_.value("execution", Json::object());
    if (options.page_workers == 0)
        options.page_workers = execution.value("page_workers", execution.value("workers", 4));
    if (options.box_workers == 0)
        options.box_workers = execution.value("box_workers", 1);
    if (options.max_active_documents == 0)
        options.max_active_documents = execution.value("document_workers", 2);
    if (options.max_queued_pages == 0)
        options.max_queued_pages = execution.value("max_queued_pages", 2);
    if (options.page_workers < 1 || options.page_workers > 128 ||
        options.box_workers < 1 || options.box_workers > 128 ||
        options.max_active_documents < 1 || options.max_active_documents > 32 ||
        options.max_queued_pages < 1 || options.max_queued_pages > 256)
        throw std::runtime_error("invalid batch worker/queue limits");

    // Reject overlapping destinations before processing any documents.
    // Otherwise one job could delete/overwrite another job's assets and outputs.
    std::vector<fs::path> outputs;
    outputs.reserve(jobs.size());
    for (const auto& job : jobs) {
        if (job.input.empty() || job.output_dir.empty())
            throw std::runtime_error("batch input/output paths must not be empty");
        const auto path = fs::absolute(job.output_dir).lexically_normal();
        for (const auto& previous : outputs) {
            auto a = path.begin(), b = previous.begin();
            while (a != path.end() && b != previous.end() && *a == *b) { ++a; ++b; }
            if (a == path.end() || b == previous.end())
                throw std::runtime_error("batch output directories must not overlap");
        }
        outputs.push_back(path);
    }

    struct Pending {
        size_t job;
        int page;
        int priority;
        uint64_t sequence;
        Image image;
    };
    std::vector<BatchResult> results(jobs.size());
    std::vector<std::map<int, Page>> finished(jobs.size());
    std::vector<size_t> input_order(jobs.size());
    std::iota(input_order.begin(), input_order.end(), 0);
    std::stable_sort(input_order.begin(), input_order.end(),
        [&](size_t a, size_t b) { return jobs[a].priority > jobs[b].priority; });
    for (size_t i = 0; i < jobs.size(); ++i)
        results[i].document.source = fs::absolute(jobs[i].input).string();

    std::deque<Pending> ready;
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<size_t> next_document{0};
    uint64_t next_sequence = 0;
    const size_t reader_count = std::min(jobs.size(), size_t(options.max_active_documents));
    size_t readers_remaining = reader_count;
    bool abort = false;
    BoxTaskPool box_pool(options.box_workers > 1 ? options.box_workers : 0);

    auto fail = [&](size_t id, const std::string& reason) {
        std::lock_guard<std::mutex> guard(mutex);
        if (results[id].error.empty()) results[id].error = reason;
        ready.erase(std::remove_if(ready.begin(), ready.end(),
                    [&](const Pending& p) { return p.job == id; }), ready.end());
        finished[id].clear();
        changed.notify_all();
    };

    auto reader = [&] {
        while (true) {
            const size_t slot = next_document.fetch_add(1);
            if (slot >= jobs.size()) break;
            const size_t id = input_order[slot];
            try {
                if (fs::exists(outputs[id]) && !fs::is_empty(outputs[id]))
                    throw std::runtime_error("output directory must be empty");
                fs::create_directories(outputs[id]);
                read_document(jobs[id].input, config_.value("document", Json::object()),
                    [&](int number, const Image& image) {
                        std::unique_lock<std::mutex> lock(mutex);
                        changed.wait(lock, [&] {
                            return abort || !results[id].error.empty() ||
                                   ready.size() < size_t(options.max_queued_pages);
                        });
                        if (abort || !results[id].error.empty())
                            throw std::runtime_error("document page processing cancelled");
                        ready.push_back(Pending{id, number, jobs[id].priority, next_sequence++, image});
                        lock.unlock();
                        changed.notify_all();
                    });
            } catch (const std::exception& e) {
                fail(id, e.what());
            } catch (...) {
                fail(id, "unknown document reader error");
            }
        }
        {
            std::lock_guard<std::mutex> guard(mutex);
            --readers_remaining;
        }
        changed.notify_all();
    };

    auto page_worker = [&] {
        while (true) {
            Pending task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return abort || !ready.empty() || readers_remaining == 0; });
                if (abort || (ready.empty() && readers_remaining == 0)) return;
                // Strict priority of READY pages; FIFO for equal priority.
                // Running pages are never preempted, and page results are reassembled by number.
                const auto chosen = std::max_element(ready.begin(), ready.end(),
                    [](const Pending& a, const Pending& b) {
                        if (a.priority != b.priority) return a.priority < b.priority;
                        return a.sequence > b.sequence;
                    });
                task = std::move(*chosen);
                ready.erase(chosen);
            }
            changed.notify_all();  // free one bounded image slot for readers
            {
                std::lock_guard<std::mutex> guard(mutex);
                if (!results[task.job].error.empty()) continue;
            }
            try {
                Page page = options.box_workers > 1 ?
                    process_page(task.page, task.image, outputs[task.job], options.box_workers,
                        [&box_pool](std::function<void()> work) {
                            return box_pool.submit(std::move(work));
                        }) :
                    process_page(task.page, task.image, outputs[task.job], 1);
                std::lock_guard<std::mutex> guard(mutex);
                if (results[task.job].error.empty() &&
                    !finished[task.job].emplace(task.page, std::move(page)).second)
                    throw std::runtime_error("duplicate page number");
            } catch (const std::exception& e) {
                fail(task.job, e.what());
            } catch (...) {
                fail(task.job, "unknown page processing error");
            }
        }
    };

    std::vector<std::thread> threads;
    try {
        for (int i = 0; i < options.page_workers; ++i) threads.emplace_back(page_worker);
        for (size_t i = 0; i < reader_count; ++i) threads.emplace_back(reader);
    } catch (...) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            abort = true;
        }
        changed.notify_all();
        for (auto& thread : threads) thread.join();
        throw;
    }
    for (auto& thread : threads) thread.join();
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].error.empty()) continue;
        for (auto& [number, page] : finished[i])
            results[i].document.pages.push_back(std::move(page));
    }
    return results;
}
} // namespace omniocr
