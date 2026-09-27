#include "omniocr/core.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
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
        std::deque<uint64_t> scalar_queue;
        uint64_t next_ticket = 0;
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable ready;
        int timeout_ms;
        int max_pending = 256, max_concurrent = 1;
        bool batching = false;
        bool stopping = false;
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
                if (!ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return request->started; })) {
                    auto it = std::find(queue.begin(), queue.end(), request);
                    if (it != queue.end()) queue.erase(it);
                    lock.unlock(); ready.notify_all();
                    throw std::runtime_error("model batch queue acquisition timed out");
                }
                lock.unlock();
                return result.get();
            }
            std::unique_lock<std::mutex> lock(mutex);
            if (scalar_queue.size() >= size_t(max_pending))
                throw std::runtime_error("model instance queue is full");
            const auto ticket = next_ticket++;
            scalar_queue.push_back(ticket);
            if (!ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                return scalar_queue.front() == ticket && !available.empty();
            })) {
                scalar_queue.erase(std::find(scalar_queue.begin(), scalar_queue.end(), ticket));
                lock.unlock(); ready.notify_all();
                throw std::runtime_error("model instance acquisition timed out");
            }
            scalar_queue.pop_front();
            size_t index = available.front(); available.pop_front();
            lock.unlock();
            ready.notify_all();
            // Returning a lease is exception safe. No instance is used concurrently.
            struct Lease {
                Pool& pool; size_t index;
                ~Lease() {
                    { std::lock_guard<std::mutex> guard(pool.mutex); pool.available.push_back(index); }
                    pool.ready.notify_all();
                }
            } lease{*this, index};
            return models[index]->infer(image, prompt);
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
} // namespace omniocr
