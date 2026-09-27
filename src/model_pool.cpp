#include "omniocr/core.hpp"
#include "backends/http_client.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace omniocr {
std::vector<Json> Model::infer_batch(const std::vector<BatchInput>& inputs) {
    if (inputs.size() != 1 || !inputs[0].image)
        throw std::runtime_error("model does not support native batch inference");
    return {infer(*inputs[0].image, inputs[0].prompt)};
}
struct ModelRegistry::Impl {
    struct Pool {
        struct Request {
            Model::BatchInput input;
            std::chrono::steady_clock::time_point queued_at;
            std::promise<Json> answer;
            bool started = false; // protected by mutex
        };
        std::vector<std::unique_ptr<Model>> models;
        struct Slot { int batch_size, max_wait_ms; };
        std::vector<Slot> slots;
        std::deque<size_t> available;
        std::deque<std::shared_ptr<Request>> queue;
        struct ScalarWaiter { uint64_t ticket; size_t tokens; int bypasses = 0; };
        std::deque<ScalarWaiter> scalar_queue;
        uint64_t next_ticket = 0;
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable ready;
        int timeout_ms;
        int max_pending = 256, max_concurrent = 1;
        bool batching = false;
        bool stopping = false;
        // All feedback and leases are protected by mutex. max_concurrent is a
        // physical handle ceiling; target is the currently admitted limit.
        bool adaptive = false;
        int minimum = 1, target = 1, window_ms = 1000, min_samples = 8, cooldown_ms = 2000;
        int latency_target_ms = 0, pixels_per_token = 784, expected_output_tokens = 512;
        size_t token_budget = 32768, inflight_tokens = 0, inflight = 0;
        double token_scale = 1.0, previous_throughput = 0, previous_latency_ms = 0;
        double window_throughput = 0, window_latency_ms = 0;
        uint64_t completed_total = 0, failed_total = 0, overload_total = 0;
        uint64_t pressure_total = 0, acquisition_timeout_total = 0;
        double queue_wait_ms_total = 0;
        uint64_t window_completed = 0, window_failed = 0, window_pressure = 0;
        double window_latency_sum = 0;
        bool window_overload = false;
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point backoff_until{};
        size_t estimate_tokens(const Image& image, const std::string& prompt) const {
            const uint64_t pixels = uint64_t(std::max(1, image.width)) * uint64_t(std::max(1, image.height));
            const uint64_t raw = 64 + (prompt.size() + 3) / 4 + uint64_t(expected_output_tokens) +
                                 (pixels + uint64_t(pixels_per_token) - 1) / uint64_t(pixels_per_token);
            return size_t(std::max(1.0, std::ceil(double(raw) * token_scale)));
        }
        bool fits(size_t tokens) const {
            return inflight < size_t(target) &&
                   (!inflight || (tokens <= token_budget && inflight_tokens <= token_budget - tokens));
        }
        uint64_t next_admissible() const {
            if (scalar_queue.empty() || available.empty()) return std::numeric_limits<uint64_t>::max();
            if (!adaptive) return scalar_queue.front().ticket;
            if (fits(scalar_queue.front().tokens)) return scalar_queue.front().ticket;
            // Avoid head-of-line blocking on an oversized BOX. The head may
            // be bypassed at most four times before the pool drains for it.
            if (scalar_queue.front().bypasses >= 4) return std::numeric_limits<uint64_t>::max();
            for (size_t i = 1; i < scalar_queue.size(); ++i)
                if (fits(scalar_queue[i].tokens)) return scalar_queue[i].ticket;
            return std::numeric_limits<uint64_t>::max();
        }
        void complete(bool failed, bool overloaded, double latency_ms, size_t estimate, size_t actual) {
            std::lock_guard<std::mutex> guard(mutex);
            --inflight;
            inflight_tokens -= estimate;
            ++completed_total;
            ++window_completed;
            window_latency_sum += latency_ms;
            // Requests already queued before this window still represent
            // sustained demand; arrival-only pressure would stop ramping.
            if (!scalar_queue.empty()) { ++window_pressure; ++pressure_total; }
            if (failed) { ++failed_total; ++window_failed; }
            if (actual && !failed) {
                const double ratio = std::clamp(double(actual) / double(estimate) * token_scale, 0.5, 4.0);
                token_scale = 0.8 * token_scale + 0.2 * ratio;
            }
            if (overloaded) {
                ++overload_total;
                window_overload = true;
                target = std::max(minimum, target / 2);
                backoff_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(cooldown_ms);
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double, std::milli>(now - window_start).count();
            if (elapsed >= window_ms && window_completed >= uint64_t(min_samples)) {
                window_throughput = 1000.0 * double(window_completed - window_failed) / elapsed;
                window_latency_ms = window_latency_sum / double(window_completed);
                if (!window_overload && window_pressure >= uint64_t(min_samples)) {
                    if (window_failed * 5 >= window_completed ||
                        (latency_target_ms && window_latency_ms > latency_target_ms) ||
                        (previous_throughput > 0 && window_throughput < previous_throughput * 0.9 &&
                         window_latency_ms > previous_latency_ms * 1.2))
                        target = std::max(minimum, target - 1);
                    else if (now >= backoff_until && window_throughput >= previous_throughput * 0.9) {
                        // Probe quickly while throughput improves; near a
                        // plateau, use single-slot probes to limit overshoot.
                        const bool ramp = previous_throughput == 0 ||
                                          window_throughput > previous_throughput * 1.2;
                        target = std::min(max_concurrent, ramp ? target * 2 : target + 1);
                    }
                }
                previous_throughput = window_throughput;
                previous_latency_ms = window_latency_ms;
                window_start = now;
                window_completed = window_failed = window_pressure = 0;
                window_latency_sum = 0;
                window_overload = false;
            }
            ready.notify_all();
        }
        Json metrics() {
            std::lock_guard<std::mutex> guard(mutex);
            return {{"strategy", adaptive ? "adaptive" : "fixed"},
                    {"concurrency_limit", adaptive ? target : max_concurrent},
                    {"max_concurrency", max_concurrent}, {"inflight", inflight},
                    {"inflight_estimated_tokens", inflight_tokens},
                    {"token_budget", adaptive ? token_budget : 0},
                    {"token_estimate_scale", token_scale},
                    {"queued", batching ? queue.size() : scalar_queue.size()},
                    {"window_throughput_per_second", window_throughput},
                    {"window_mean_latency_ms", window_latency_ms},
                    {"completed_total", completed_total}, {"failed_total", failed_total},
                    {"overload_total", overload_total}, {"pressure_total", pressure_total},
                    {"acquisition_timeout_total", acquisition_timeout_total},
                    {"cooldown_remaining_ms", adaptive ? std::max<int64_t>(0,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             backoff_until - std::chrono::steady_clock::now()).count()) : 0},
                    {"queue_wait_ms_total", queue_wait_ms_total}};
        }
        ~Pool() {
            { std::lock_guard<std::mutex> guard(mutex); stopping = true; }
            ready.notify_all();
            for (auto& worker : workers) worker.join();
        }
        void batch_worker(size_t index) {
            const auto profile = slots[index];
            while (true) {
                std::vector<std::shared_ptr<Request>> batch;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty()) return;
                    while (!stopping && !queue.empty() &&
                           queue.size() < size_t(profile.batch_size) && profile.max_wait_ms > 0) {
                        const auto deadline = queue.front()->queued_at + std::chrono::milliseconds(profile.max_wait_ms);
                        if (ready.wait_until(lock, deadline, [&] {
                            return stopping || queue.empty() || queue.size() >= size_t(profile.batch_size);
                        })) continue;
                        break;
                    }
                    if (queue.empty()) continue;
                    for (size_t i = 0, count = std::min(queue.size(), size_t(profile.batch_size)); i < count; ++i) {
                        auto request = std::move(queue.front()); queue.pop_front();
                        request->started = true;
                        batch.push_back(request);
                    }
                }
                ready.notify_all();
                try {
                    std::vector<Model::BatchInput> inputs;
                    inputs.reserve(batch.size());
                    for (const auto& request : batch) inputs.push_back(request->input);
                    auto responses = profile.batch_size == 1
                        ? std::vector<Json>{models[index]->infer(*inputs[0].image, inputs[0].prompt)}
                        : models[index]->infer_batch(inputs);
                    if (responses.size() != batch.size())
                        throw std::runtime_error("batch model returned an incorrect number of results");
                    for (size_t i = 0; i < batch.size(); ++i)
                        batch[i]->answer.set_value(std::move(responses[i]));
                } catch (...) {
                    const auto error = std::current_exception();
                    for (const auto& request : batch) request->answer.set_exception(error);
                }
            }
        }
        Json infer(const Image& image, const std::string& prompt) {
            throw_if_cancelled();
            if (batching) {
                auto request = std::make_shared<Request>();
                request->input = {&image, prompt};
                request->queued_at = std::chrono::steady_clock::now();
                auto result = request->answer.get_future();
                std::unique_lock<std::mutex> lock(mutex);
                if (queue.size() >= size_t(max_pending))
                    throw std::runtime_error("model batch queue is full");
                queue.push_back(request);
                ready.notify_all();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
                while (!request->started && !cancellation_requested() &&
                       std::chrono::steady_clock::now() < deadline)
                    ready.wait_until(lock, std::min(deadline,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
                if (!request->started) {
                    auto it = std::find(queue.begin(), queue.end(), request);
                    if (it != queue.end()) queue.erase(it);
                    lock.unlock(); ready.notify_all();
                    throw_if_cancelled();
                    throw std::runtime_error("model batch queue acquisition timed out");
                }
                lock.unlock();
                auto response = result.get(); // Worker still owns the input image until this returns.
                throw_if_cancelled();
                return response;
            }
            std::unique_lock<std::mutex> lock(mutex);
            if (scalar_queue.size() >= size_t(max_pending))
                throw std::runtime_error("model instance queue is full");
            if (adaptive && !inflight && scalar_queue.empty()) {
                // Exclude the idle gap between offline jobs from the next
                // throughput window.
                window_start = std::chrono::steady_clock::now();
                window_completed = window_failed = window_pressure = 0;
                window_latency_sum = 0;
                window_overload = false;
                previous_throughput = previous_latency_ms = 0;
            }
            const auto ticket = next_ticket++;
            const size_t tokens = adaptive ? estimate_tokens(image, prompt) : 0;
            const auto queued_at = std::chrono::steady_clock::now();
            scalar_queue.push_back({ticket, tokens});
            if (adaptive && (inflight >= size_t(target) ||
                (inflight && tokens > token_budget - std::min(token_budget, inflight_tokens)))) {
                ++window_pressure;
                ++pressure_total;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (next_admissible() != ticket && !cancellation_requested() &&
                   std::chrono::steady_clock::now() < deadline)
                ready.wait_until(lock, std::min(deadline,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
            if (next_admissible() != ticket || cancellation_requested()) {
                if (adaptive && !cancellation_requested()) ++acquisition_timeout_total;
                scalar_queue.erase(std::find_if(scalar_queue.begin(), scalar_queue.end(),
                    [&](const ScalarWaiter& waiter) { return waiter.ticket == ticket; }));
                lock.unlock(); ready.notify_all();
                throw_if_cancelled();
                throw std::runtime_error("model instance acquisition timed out");
            }
            auto selected = std::find_if(scalar_queue.begin(), scalar_queue.end(),
                [&](const ScalarWaiter& waiter) { return waiter.ticket == ticket; });
            if (selected != scalar_queue.begin()) ++scalar_queue.front().bypasses;
            scalar_queue.erase(selected);
            if (adaptive)
                queue_wait_ms_total += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - queued_at).count();
            size_t index = available.front(); available.pop_front();
            ++inflight;
            if (adaptive) inflight_tokens += tokens;
            lock.unlock();
            ready.notify_all();
            // Returning a lease is exception safe. No instance is used concurrently.
            struct Lease {
                Pool& pool; size_t index;
                ~Lease() {
                    { std::lock_guard<std::mutex> guard(pool.mutex);
                      pool.available.push_back(index);
                      if (!pool.adaptive) --pool.inflight; }
                    pool.ready.notify_all();
                }
            } lease{*this, index};
            if (!adaptive) {
                throw_if_cancelled();
                auto response = models[index]->infer(image, prompt);
                throw_if_cancelled();
                return response;
            }
            const auto start = std::chrono::steady_clock::now();
            bool failed = false, overloaded = false;
            struct Feedback {
                Pool& pool; size_t tokens, index;
                bool& failed; bool& overloaded;
                std::chrono::steady_clock::time_point start;
                ~Feedback() {
                    const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                    pool.complete(failed, overloaded, ms, tokens,
                                  failed ? 0 : pool.models[index]->last_usage_tokens());
                }
            } feedback{*this, tokens, index, failed, overloaded, start};
            try {
                throw_if_cancelled();
                auto response = models[index]->infer(image, prompt);
                throw_if_cancelled();
                return response;
            }
            catch (const HttpStatusError& e) {
                failed = true;
                overloaded = e.status == 429 || e.status == 503;
                throw;
            } catch (...) { failed = true; throw; }
        }
        void start() {
            if (!batching) return;
            for (int i = 0; i < max_concurrent; ++i)
                workers.emplace_back([this, i] { batch_worker(i); });
        }
    };
    std::map<std::string, std::unique_ptr<Pool>> pools;
};
ModelRegistry::ModelRegistry(const Json& models, ModelFactory factory) : impl_(std::make_unique<Impl>()) {
    for (const auto& [id, config] : models.items()) {
        auto pool = std::make_unique<Impl::Pool>();
        pool->timeout_ms = config.value("acquire_timeout_ms", 60000);
        const int default_batch = config.value("batch_size", 1);
        const int default_wait = config.value("max_batch_wait_ms", 5);
        pool->max_pending = config.value("max_pending_requests", 256);
        const int count = config.value("instances", 1);
        if (count <= 0 || count > 128) throw std::runtime_error("invalid instances for " + id);
        pool->max_concurrent = config.value("max_concurrent_requests", count);
        if (pool->max_concurrent < 1 || pool->max_concurrent > 128)
            throw std::runtime_error("invalid max_concurrent_requests for " + id);
        if (config.contains("adaptive_concurrency")) {
            const auto& adaptive = config.at("adaptive_concurrency");
            if (!adaptive.is_object()) throw std::runtime_error("invalid adaptive_concurrency for " + id);
            pool->adaptive = adaptive.value("enabled", false);
            if (pool->adaptive) {
                if (config.value("backend", std::string{}) != "vllm")
                    throw std::runtime_error("adaptive_concurrency requires vllm backend: " + id);
                pool->minimum = adaptive.value("min_concurrency", 1);
                pool->target = adaptive.value("initial_concurrency", std::min(4, pool->max_concurrent));
                pool->window_ms = adaptive.value("window_ms", 1000);
                pool->min_samples = adaptive.value("min_samples", 8);
                pool->cooldown_ms = adaptive.value("cooldown_ms", 2000);
                pool->latency_target_ms = adaptive.value("latency_target_ms", 0);
                pool->pixels_per_token = adaptive.value("image_pixels_per_token", 784);
                pool->expected_output_tokens = adaptive.value("expected_output_tokens", 512);
                pool->token_budget = adaptive.value("token_budget", 32768);
                if (pool->minimum < 1 || pool->target < pool->minimum || pool->target > pool->max_concurrent ||
                    pool->window_ms < 100 || pool->min_samples < 1 || pool->pixels_per_token < 1 ||
                    pool->expected_output_tokens < 1 || pool->token_budget < 1 || pool->latency_target_ms < 0 ||
                    pool->cooldown_ms < 0)
                    throw std::runtime_error("invalid adaptive_concurrency settings for " + id);
            }
        }
        if (default_batch < 1 || default_batch > 128 || default_wait < 0 ||
            (default_batch > 1 && default_wait >= pool->timeout_ms))
            throw std::runtime_error("invalid batch settings for " + id);
        const auto overrides = config.value("instance_overrides", Json::array());
        if (!overrides.is_array() || overrides.size() > size_t(pool->max_concurrent))
            throw std::runtime_error("invalid instance_overrides for " + id);
        for (int i = 0; i < pool->max_concurrent; ++i) {
            const auto& setting = size_t(i) < overrides.size() ? overrides[size_t(i)] : Json::object();
            if (!setting.is_object()) throw std::runtime_error("invalid instance override for " + id);
            const int batch = setting.value("batch_size", default_batch);
            const int wait = setting.value("max_batch_wait_ms", default_wait);
            if (batch < 1 || batch > 128 || wait < 0 || wait > 1000 ||
                (batch > 1 && wait >= pool->timeout_ms) || pool->max_pending < batch)
                throw std::runtime_error("invalid instance batch settings for " + id);
            pool->slots.push_back({batch, wait});
            pool->batching |= batch > 1;
        }
        // Each active slot owns an independent backend handle. HTTP slots are
        // lightweight connection clients; local backends require separate
        // model/engine instances because their handles are not reentrant.
        for (int i = 0; i < std::max(count, pool->max_concurrent); ++i) {
            Json instance_config = config;
            if (i < pool->max_concurrent) instance_config["batch_size"] = pool->slots[size_t(i)].batch_size;
            auto model = factory(instance_config, size_t(i));
            if (!model) throw std::runtime_error("model factory returned null for " + id);
            if (i < pool->max_concurrent && pool->slots[size_t(i)].batch_size > 1 && !model->supports_batch())
                throw std::runtime_error("model does not support native batching: " + id);
            pool->models.push_back(std::move(model));
            if (i < pool->max_concurrent) pool->available.push_back(size_t(i));
        }
        if (pool->adaptive && pool->batching)
            throw std::runtime_error("adaptive_concurrency does not support native batching: " + id);
        pool->start();
        impl_->pools.emplace(id, std::move(pool));
    }
}
ModelRegistry::~ModelRegistry() = default;
Json ModelRegistry::infer(const std::string& id, const Image& image, const std::string& prompt) {
    auto it = impl_->pools.find(id);
    if (it == impl_->pools.end()) throw std::runtime_error("unknown model: " + id);
    return it->second->infer(image, prompt);
}
Json ModelRegistry::scheduler_metrics() {
    Json result = Json::object();
    for (const auto& [id, pool] : impl_->pools) result[id] = pool->metrics();
    return result;
}
} // namespace omniocr
