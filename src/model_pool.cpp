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
        int input_min_width = 1, input_min_height = 1;
        int input_max_width = 100000, input_max_height = 100000;
        int input_factor = 1;
        uint64_t input_min_pixels = 0, input_max_pixels = std::numeric_limits<uint64_t>::max();
        bool input_resize_enabled = false;
        bool batching = false;
        bool stopping = false;
        // All feedback and leases are protected by mutex. max_concurrent is a
        // physical request ceiling; the adaptive window controls token budget.
        bool adaptive = false;
        int minimum = 1, target = 1, window_ms = 5000, min_samples = 8, cooldown_ms = 3000;
        int pixels_per_token = 784, expected_output_tokens = 512;
        size_t token_budget = 32768, min_token_budget = 1, current_token_budget = 1;
        size_t inflight_tokens = 0, inflight = 0, comparison_budget = 0;
        double token_scale = 1.0, comparison_work_rate = 0;
        double window_throughput = 0, window_latency_ms = 0, window_work_rate = 0, throughput_gain = 0;
        double slow_start_gain = 0.10, probe_gain = 0.03, probe_step = 0.10, backoff_ratio = 0.85;
        enum class Phase { SlowStart, Probe, Hold } phase = Phase::SlowStart;
        bool evaluating_probe = false;
        uint64_t control_epoch = 0, throughput_backoff_total = 0;
        size_t stale_inflight = 0;
        uint64_t completed_total = 0, failed_total = 0, overload_total = 0;
        uint64_t completed_normalized_work_total = 0;
        uint64_t pressure_total = 0, acquisition_timeout_total = 0;
        double queue_wait_ms_total = 0;
        uint64_t window_completed = 0, window_failed = 0, window_pressure = 0;
        uint64_t window_budget_pressure = 0, budget_pressure_total = 0;
        double window_latency_sum = 0, window_work = 0;
        bool window_overload = false;
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point backoff_until{};
        std::array<int, 2> input_size(const Image& image) const {
            if (image.width<=0 || image.height<=0) throw std::runtime_error("invalid model input dimensions");
            if (!input_resize_enabled) return {image.width,image.height};
            const int f=input_factor;
            const int lo_w=std::max(f,((input_min_width+f-1)/f)*f);
            const int lo_h=std::max(f,((input_min_height+f-1)/f)*f);
            const int hi_w=(input_max_width/f)*f;
            const int hi_h=(input_max_height/f)*f;
            if (lo_w>hi_w || lo_h>hi_h) throw std::runtime_error("infeasible input_resize dimensions");
            double source_w=image.width,source_h=image.height;
            // Like smart_resize, expand a sub-factor short side first. This
            // preserves the intended aspect ratio for very thin BOX crops.
            if (source_h<f) {source_w=std::round(source_w*f/source_h);source_h=f;}
            if (source_w<f) {source_h=std::round(source_h*f/source_w);source_w=f;}
            const double pixels=source_w*source_h;
            const double min_scale=std::max({double(lo_w)/source_w,double(lo_h)/source_h,
                std::sqrt(double(input_min_pixels)/pixels)});
            const double max_scale=std::min({double(hi_w)/source_w,double(hi_h)/source_h,
                std::sqrt(double(input_max_pixels)/pixels)});
            const double scale=std::clamp(1.0,std::min(min_scale,max_scale),std::max(min_scale,max_scale));
            const auto quantize=[&](double dimension,int low,int high) {
                const double units=dimension/f;
                const double rounded=scale<1.0 ? std::floor(units+1e-10) :
                                     scale>1.0 ? std::ceil(units-1e-10) : std::round(units);
                return std::clamp(int(std::clamp(rounded,0.0,100000.0))*f,low,high);
            };
            const int candidate_w=quantize(source_w*scale,lo_w,hi_w);
            const int candidate_h=quantize(source_h*scale,lo_h,hi_h);
            const auto fits=[&](int w,int h) {
                const uint64_t area=uint64_t(w)*h;
                return w>=lo_w && w<=hi_w && h>=lo_h && h<=hi_h &&
                    area>=input_min_pixels && area<=input_max_pixels;
            };
            if (fits(candidate_w,candidate_h)) return {candidate_w,candidate_h};

            // Rounding each axis independently can cross a pixel bound. Search
            // the feasible grid only in that case and retain the closest aspect
            // ratio and scale; this path is uncommon for normal BOX sizes.
            double best=std::numeric_limits<double>::infinity();
            std::array<int,2> result{};
            const double wanted_area=std::clamp(double(image.width)*image.height,
                double(input_min_pixels),double(input_max_pixels));
            for (int w=lo_w;w<=hi_w;w+=f) {
                const int h_low=std::max(lo_h,int(((input_min_pixels+w-1)/w+f-1)/f)*f);
                const int h_high=int(std::min(uint64_t(hi_h),(input_max_pixels/w/f)*uint64_t(f)));
                if (h_low>h_high) continue;
                const double ideal=double(w)*image.height/image.width;
                const int middle=std::clamp(int(std::round(ideal/f))*f,h_low,h_high);
                for (const int h : {h_low,middle,h_high}) {
                    const double aspect=std::abs(std::log((double(w)/h)/(double(image.width)/image.height)));
                    const double area=std::abs(std::log((double(w)*h)/wanted_area));
                    const double score=aspect+area;
                    if (score<best) {best=score;result={w,h};}
                }
            }
            if (!std::isfinite(best)) throw std::runtime_error("no image size satisfies input_resize bounds");
            return result;
        }
        size_t estimate_tokens(const Image& image, const std::string& prompt) const {
            const uint64_t pixels = uint64_t(std::max(1, image.width)) * uint64_t(std::max(1, image.height));
            const uint64_t raw = 64 + (prompt.size() + 3) / 4 + uint64_t(expected_output_tokens) +
                                 (pixels + uint64_t(pixels_per_token) - 1) / uint64_t(pixels_per_token);
            return size_t(std::max(1.0, std::ceil(double(raw) * token_scale)));
        }
        size_t normalized_work(const Image& image, const std::string& prompt) const {
            const uint64_t pixels = uint64_t(std::max(1, image.width)) * uint64_t(std::max(1, image.height));
            return size_t(64 + (prompt.size() + 3) / 4 + uint64_t(expected_output_tokens) +
                (pixels + uint64_t(pixels_per_token) - 1) / uint64_t(pixels_per_token));
        }
        bool fits(size_t tokens) const {
            return inflight < size_t(target) &&
                   (!inflight || (tokens <= current_token_budget &&
                                   inflight_tokens <= current_token_budget - tokens));
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
        void set_budget(size_t next) {
            current_token_budget = std::clamp(next, min_token_budget, token_budget);
            ++control_epoch;
            stale_inflight = inflight;
            window_start = std::chrono::steady_clock::now();
            window_completed = window_failed = window_pressure = window_budget_pressure = 0;
            window_work = window_latency_sum = 0;
        }
        size_t increased_budget(double factor) const {
            return std::min(token_budget, std::max(current_token_budget + 1,
                size_t(std::ceil(double(current_token_budget) * factor))));
        }
        void complete(bool failed, bool overloaded, double latency_ms, size_t estimate,
                      size_t work, size_t actual, size_t output_tokens, uint64_t epoch) {
            std::lock_guard<std::mutex> guard(mutex);
            if (!failed && output_tokens)
                work = work - size_t(expected_output_tokens) + output_tokens;
            const bool budget_blocked = !scalar_queue.empty() && inflight &&
                (inflight_tokens > current_token_budget ||
                 scalar_queue.front().tokens > current_token_budget - inflight_tokens);
            --inflight;
            inflight_tokens -= estimate;
            ++completed_total;
            if (!scalar_queue.empty()) ++pressure_total;
            if (failed) ++failed_total;
            else completed_normalized_work_total += work;
            if (epoch != control_epoch && stale_inflight) {
                --stale_inflight;
                if (!stale_inflight) {
                    window_start = std::chrono::steady_clock::now();
                    window_completed = window_failed = window_pressure = window_budget_pressure = 0;
                    window_work = window_latency_sum = 0;
                }
            }
            if (actual && !failed) {
                const double ratio = std::clamp(double(actual) / double(estimate) * token_scale, 0.5, 4.0);
                token_scale = 0.8 * token_scale + 0.2 * ratio;
            }
            if (overloaded) {
                ++overload_total;
                window_overload = true;
                const auto smaller = std::max(min_token_budget,
                    size_t(double(current_token_budget) * backoff_ratio));
                if (smaller < current_token_budget) set_budget(smaller);
                phase = Phase::Hold;
                evaluating_probe = false;
                backoff_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(cooldown_ms);
            }
            // In-flight requests from an earlier budget do not enter the new
            // budget's throughput sample; they still free their lease above.
            if (epoch != control_epoch || stale_inflight) { ready.notify_all(); return; }
            ++window_completed;
            window_latency_sum += latency_ms;
            if (failed) ++window_failed;
            else window_work += double(work);
            if (!scalar_queue.empty()) ++window_pressure;
            if (budget_blocked) { ++window_budget_pressure; ++budget_pressure_total; }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double, std::milli>(now - window_start).count();
            if (elapsed >= window_ms && window_completed >= uint64_t(min_samples)) {
                window_throughput = 1000.0 * double(window_completed - window_failed) / elapsed;
                window_latency_ms = window_latency_sum / double(window_completed);
                window_work_rate = 1000.0 * window_work / elapsed;
                const bool demand = window_pressure >= uint64_t(min_samples) &&
                                    window_budget_pressure >= uint64_t(min_samples);
                size_t next = current_token_budget;
                if (!window_overload && window_failed * 5 >= window_completed) {
                    next = std::max(min_token_budget,
                        size_t(double(current_token_budget) * backoff_ratio));
                    phase = Phase::Hold;
                    evaluating_probe = false;
                    backoff_until = now + std::chrono::milliseconds(cooldown_ms);
                } else if (!window_overload) {
                    if (evaluating_probe && comparison_work_rate > 0) {
                        throughput_gain = window_work_rate / comparison_work_rate - 1.0;
                        if (current_token_budget < comparison_budget) {
                            // At the hard ceiling, periodically test whether
                            // less in-flight work completes just as quickly.
                            if (throughput_gain < -probe_gain) {
                                next = comparison_budget;
                                phase = Phase::Hold;
                            } else if (throughput_gain > probe_gain && demand)
                                next = std::max(min_token_budget,
                                    size_t(double(current_token_budget) * (1.0 - probe_step)));
                            else phase = Phase::Hold;
                            if (next >= comparison_budget || next == current_token_budget)
                                backoff_until = now + std::chrono::milliseconds(cooldown_ms);
                        } else if (phase == Phase::SlowStart && throughput_gain >= slow_start_gain && demand)
                            next = increased_budget(2.0);
                        else {
                            if (phase == Phase::SlowStart) phase = Phase::Probe;
                            if (throughput_gain >= probe_gain && demand)
                                next = increased_budget(1.0 + probe_step);
                            else if (throughput_gain < -probe_gain) {
                                next = std::max(min_token_budget,
                                    size_t(double(current_token_budget) * backoff_ratio));
                                ++throughput_backoff_total;
                                phase = Phase::Hold;
                                backoff_until = now + std::chrono::milliseconds(cooldown_ms);
                            } else {
                                // A flat probe bought no useful throughput.
                                next = std::max(min_token_budget, comparison_budget);
                                phase = Phase::Hold;
                                backoff_until = now + std::chrono::milliseconds(cooldown_ms);
                            }
                        }
                    } else if (demand && now >= backoff_until && current_token_budget < token_budget) {
                        next = increased_budget(phase == Phase::SlowStart ? 2.0 : 1.0 + probe_step);
                        phase = phase == Phase::SlowStart ? Phase::SlowStart : Phase::Probe;
                    } else if (window_pressure >= uint64_t(min_samples) && now >= backoff_until &&
                               current_token_budget == token_budget && comparison_work_rate > 0) {
                        next = std::max(min_token_budget,
                            size_t(double(current_token_budget) * (1.0 - probe_step)));
                        phase = Phase::Probe;
                    }
                }
                const bool changed = next != current_token_budget;
                if (changed) {
                    comparison_work_rate = window_work_rate;
                    comparison_budget = current_token_budget;
                    set_budget(next);
                } else {
                    evaluating_probe = false;
                    // No changed budget: learn the workload's current rate,
                    // without backing off merely because BOX mix became slow.
                    comparison_work_rate = window_work_rate;
                    comparison_budget = current_token_budget;
                    window_start = now;
                    window_completed = window_failed = window_pressure = window_budget_pressure = 0;
                    window_work = window_latency_sum = 0;
                }
                if (changed) evaluating_probe = phase != Phase::Hold;
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
                    {"current_token_budget", adaptive ? current_token_budget : 0},
                    {"stale_inflight", adaptive ? stale_inflight : 0},
                    {"min_token_budget", adaptive ? min_token_budget : 0},
                    {"control_phase", phase == Phase::SlowStart ? "slow_start" :
                                      phase == Phase::Probe ? "probe" : "hold"},
                    {"token_estimate_scale", token_scale},
                    {"queued", batching ? queue.size() : scalar_queue.size()},
                    {"window_throughput_per_second", window_throughput},
                    {"window_mean_latency_ms", window_latency_ms},
                    {"window_normalized_work_per_second", window_work_rate},
                    {"throughput_gain", throughput_gain},
                    {"throughput_backoff_total", throughput_backoff_total},
                    {"budget_pressure_total", budget_pressure_total},
                    {"completed_total", completed_total}, {"failed_total", failed_total},
                    {"completed_normalized_work_total", completed_normalized_work_total},
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
            std::optional<Image> resized;
            const Image* input=&image;
            const auto size=input_size(image);
            if (size[0]!=image.width || size[1]!=image.height) {
                resized.emplace(image.resize(size[0],size[1]));
                input=&*resized;
            }
            if (batching) {
                auto request = std::make_shared<Request>();
                request->input = {input, prompt};
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
                window_completed = window_failed = window_pressure = window_budget_pressure = 0;
                window_latency_sum = 0;
                window_overload = false;
                comparison_work_rate = 0;
                evaluating_probe = false;
                phase = Phase::SlowStart;
            }
            const auto ticket = next_ticket++;
            const size_t tokens = adaptive ? estimate_tokens(*input, prompt) : 0;
            const size_t work = adaptive ? normalized_work(*input, prompt) : 0;
            const auto queued_at = std::chrono::steady_clock::now();
            scalar_queue.push_back({ticket, tokens});
            const bool budget_blocked = adaptive && inflight &&
                (inflight_tokens > current_token_budget || tokens > current_token_budget - inflight_tokens);
            if (budget_blocked) { ++window_budget_pressure; ++budget_pressure_total; }
            if (adaptive && (inflight >= size_t(target) ||
                budget_blocked)) {
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
            const auto epoch = control_epoch;
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
                auto response = models[index]->infer(*input, prompt);
                throw_if_cancelled();
                return response;
            }
            const auto start = std::chrono::steady_clock::now();
            bool failed = false, overloaded = false;
            struct Feedback {
                Pool& pool; size_t tokens, work, index;
                uint64_t epoch;
                bool& failed; bool& overloaded;
                std::chrono::steady_clock::time_point start;
                ~Feedback() {
                    const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                    pool.complete(failed, overloaded, ms, tokens, work,
                                  failed ? 0 : pool.models[index]->last_usage_tokens(),
                                  failed ? 0 : pool.models[index]->last_completion_tokens(), epoch);
                }
            } feedback{*this, tokens, work, index, epoch, failed, overloaded, start};
            try {
                throw_if_cancelled();
                auto response = models[index]->infer(*input, prompt);
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
        if (config.contains("input_resize")) {
            pool->input_resize_enabled = true;
            const auto& resize=config.at("input_resize");
            if (!resize.is_object() || resize.empty())
                throw std::runtime_error("input_resize must be a nonempty object: " + id);
            for (const auto& [key,value]:resize.items()) {
                if (key!="max_width" && key!="max_height" && key!="max_pixels" &&
                    key!="min_width" && key!="min_height" && key!="min_pixels" && key!="factor")
                    throw std::runtime_error("unknown input_resize setting: " + key);
                const uint64_t limit=key=="max_pixels" || key=="min_pixels" ? 200000000 :
                                     key=="factor" ? 4096 : 100000;
                if (!(value.is_number_integer() || value.is_number_unsigned()) ||
                    (value.is_number_unsigned() ?
                        value.get<uint64_t>()<1 || value.get<uint64_t>()>limit :
                        value.get<int64_t>()<1 || uint64_t(value.get<int64_t>())>limit))
                    throw std::runtime_error("invalid input_resize." + key + ": " + id);
            }
            pool->input_max_width=resize.value("max_width",100000);
            pool->input_max_height=resize.value("max_height",100000);
            pool->input_max_pixels=resize.value("max_pixels",std::numeric_limits<uint64_t>::max());
            pool->input_min_width=resize.value("min_width",1);
            pool->input_min_height=resize.value("min_height",1);
            pool->input_min_pixels=resize.value("min_pixels",uint64_t(0));
            pool->input_factor=resize.value("factor",1);
            if (pool->input_min_width>pool->input_max_width ||
                pool->input_min_height>pool->input_max_height ||
                pool->input_min_pixels>pool->input_max_pixels)
                throw std::runtime_error("conflicting input_resize bounds: " + id);
        }
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
                if (pool->minimum < 1 || pool->target < pool->minimum || pool->target > pool->max_concurrent)
                    throw std::runtime_error("invalid adaptive_concurrency settings for " + id);
                pool->window_ms = adaptive.value("window_ms", 5000);
                pool->min_samples = adaptive.value("min_samples", 8);
                pool->cooldown_ms = adaptive.value("cooldown_ms", 3000);
                pool->pixels_per_token = adaptive.value("image_pixels_per_token", 784);
                pool->expected_output_tokens = adaptive.value("expected_output_tokens", 512);
                pool->token_budget = adaptive.value("token_budget", 32768);
                pool->min_token_budget = adaptive.value("min_token_budget",
                    std::max<size_t>(1, pool->token_budget * size_t(pool->minimum) / size_t(pool->max_concurrent)));
                pool->current_token_budget = adaptive.value("initial_token_budget",
                    std::max(pool->min_token_budget,
                        pool->token_budget * size_t(pool->target) / size_t(pool->max_concurrent)));
                pool->slow_start_gain = adaptive.value("slow_start_gain", 0.10);
                pool->probe_gain = adaptive.value("probe_gain", 0.03);
                pool->probe_step = adaptive.value("probe_step", 0.10);
                pool->backoff_ratio = adaptive.value("backoff_ratio", 0.85);
                pool->target = pool->max_concurrent;
                if (pool->minimum < 1 || pool->target < pool->minimum || pool->target > pool->max_concurrent ||
                    pool->window_ms < 100 || pool->min_samples < 1 || pool->pixels_per_token < 1 ||
                    pool->expected_output_tokens < 1 || pool->token_budget < 1 ||
                    pool->min_token_budget < 1 || pool->min_token_budget > pool->current_token_budget ||
                    pool->current_token_budget > pool->token_budget || pool->cooldown_ms < 0 ||
                    !std::isfinite(pool->slow_start_gain) || !std::isfinite(pool->probe_gain) ||
                    !std::isfinite(pool->probe_step) || !std::isfinite(pool->backoff_ratio) ||
                    pool->slow_start_gain <= 0 || pool->probe_gain <= 0 ||
                    pool->probe_step <= 0 || pool->backoff_ratio <= 0 || pool->backoff_ratio >= 1)
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
std::array<int, 2> ModelRegistry::input_size(const std::string& id, const Image& image) const {
    auto it=impl_->pools.find(id);
    if (it==impl_->pools.end()) throw std::runtime_error("unknown model: " + id);
    return it->second->input_size(image);
}
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
