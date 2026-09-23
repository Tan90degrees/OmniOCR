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
        std::deque<size_t> available;
        std::deque<std::shared_ptr<Request>> queue;
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable ready;
        int timeout_ms;
        int batch_size = 1, max_wait_ms = 5, max_pending = 256;
        bool stopping = false;
        ~Pool() {
            { std::lock_guard<std::mutex> guard(mutex); stopping = true; }
            ready.notify_all();
            for (auto& worker : workers) worker.join();
        }
        void batch_worker(size_t index) {
            while (true) {
                std::vector<std::shared_ptr<Request>> batch;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty()) return;
                    while (!stopping && !queue.empty() &&
                           queue.size() < size_t(batch_size) && max_wait_ms > 0) {
                        const auto deadline = queue.front()->queued_at + std::chrono::milliseconds(max_wait_ms);
                        if (ready.wait_until(lock, deadline, [&] {
                            return stopping || queue.empty() || queue.size() >= size_t(batch_size);
                        })) continue;
                        break;
                    }
                    if (queue.empty()) continue;
                    for (size_t i = 0, count = std::min(queue.size(), size_t(batch_size)); i < count; ++i) {
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
                    auto responses = models[index]->infer_batch(inputs);
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
            if (batch_size > 1) {
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
            if (!ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !available.empty(); }))
                throw std::runtime_error("model instance acquisition timed out");
            size_t index = available.front(); available.pop_front();
            lock.unlock();
            // Returning a lease is exception safe. No instance is used concurrently.
            struct Lease {
                Pool& pool; size_t index;
                ~Lease() {
                    { std::lock_guard<std::mutex> guard(pool.mutex); pool.available.push_back(index); }
                    pool.ready.notify_one();
                }
            } lease{*this, index};
            return models[index]->infer(image, prompt);
        }
        void start() {
            if (batch_size <= 1) return;
            for (size_t i = 0; i < models.size(); ++i)
                workers.emplace_back([this, i] { batch_worker(i); });
        }
    };
    std::map<std::string, std::unique_ptr<Pool>> pools;
};
ModelRegistry::ModelRegistry(const Json& models, ModelFactory factory) : impl_(std::make_unique<Impl>()) {
    for (const auto& [id, config] : models.items()) {
        auto pool = std::make_unique<Impl::Pool>();
        pool->timeout_ms = config.value("acquire_timeout_ms", 60000);
        pool->batch_size = config.value("batch_size", 1);
        pool->max_wait_ms = config.value("max_batch_wait_ms", 5);
        pool->max_pending = config.value("max_pending_requests", 256);
        const int count = config.value("instances", 1);
        if (count <= 0 || count > 128) throw std::runtime_error("invalid instances for " + id);
        if (pool->batch_size < 1 || pool->batch_size > 128 || pool->max_wait_ms < 0 ||
            (pool->batch_size > 1 && pool->max_wait_ms >= pool->timeout_ms) ||
            pool->max_pending < pool->batch_size)
            throw std::runtime_error("invalid batch settings for " + id);
        for (int i = 0; i < count; ++i) {
            auto model = factory(config, size_t(i));
            if (!model) throw std::runtime_error("model factory returned null for " + id);
            if (pool->batch_size > 1 && !model->supports_batch())
                throw std::runtime_error("model does not support native batching: " + id);
            pool->models.push_back(std::move(model));
            pool->available.push_back(size_t(i));
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
