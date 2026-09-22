#include "omniocr/core.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

namespace omniocr {
struct ModelRegistry::Impl {
    struct Pool {
        std::vector<std::unique_ptr<Model>> models;
        std::deque<size_t> available;
        std::mutex mutex;
        std::condition_variable ready;
        int timeout_ms;
        Json infer(const Image& image, const std::string& prompt) {
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
    };
    std::map<std::string, std::unique_ptr<Pool>> pools;
};
ModelRegistry::ModelRegistry(const Json& models, ModelFactory factory) : impl_(std::make_unique<Impl>()) {
    for (const auto& [id, config] : models.items()) {
        auto pool = std::make_unique<Impl::Pool>();
        pool->timeout_ms = config.value("acquire_timeout_ms", 60000);
        const int count = config.value("instances", 1);
        if (count <= 0 || count > 128) throw std::runtime_error("invalid instances for " + id);
        for (int i = 0; i < count; ++i) {
            auto model = factory(config, size_t(i));
            if (!model) throw std::runtime_error("model factory returned null for " + id);
            pool->models.push_back(std::move(model));
            pool->available.push_back(size_t(i));
        }
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
