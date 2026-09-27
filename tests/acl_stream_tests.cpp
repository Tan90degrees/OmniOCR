#include "omniocr/tensor.hpp"
#include <acl/acl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

struct FakeContext { int device; };
struct FakeStream {
    FakeContext* context;
    std::vector<std::function<void()>> tasks;
};
struct aclDataBuffer { void* ptr; size_t size; };
struct aclmdlDataset { std::vector<aclDataBuffer*> buffers; };
struct aclmdlDesc { uint32_t id = 0; };

namespace fake {
std::mutex mutex;
std::unordered_set<void*> pinned;
std::unordered_map<uint32_t, size_t> batch_sizes;
std::atomic<int> streams{0}, syncs{0}, async_copies{0}, sync_copies{0};
std::atomic<int> active{0}, peak{0}, fail_execute{0}, fail_sync{0};
std::atomic<uint32_t> next_model{1};
std::mutex overlap_mutex;
std::condition_variable overlap_ready;
int overlap_arrivals = 0;
bool await_overlap = false;
thread_local FakeContext* current = nullptr;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void execute(const aclmdlDataset* in, aclmdlDataset* out) {
    auto* input = static_cast<float*>(in->buffers[0]->ptr);
    auto* output = static_cast<float*>(out->buffers[0]->ptr);
    require(in->buffers[0]->size == out->buffers[0]->size, "model buffer size mismatch");
    for (size_t i = 0; i < in->buffers[0]->size / sizeof(float); ++i) output[i] = input[i] * 2;
}
} // namespace fake

aclError aclInit(const char*) { return ACL_SUCCESS; }
aclError aclFinalize() { return ACL_SUCCESS; }
aclError aclrtSetDevice(int) { return ACL_SUCCESS; }
aclError aclrtResetDevice(int) { return ACL_SUCCESS; }
aclError aclrtCreateContext(aclrtContext* p, int device) {
    *p = new FakeContext{device}; fake::current = *p; return ACL_SUCCESS;
}
aclError aclrtDestroyContext(aclrtContext p) { delete p; fake::current = nullptr; return ACL_SUCCESS; }
aclError aclrtSetCurrentContext(aclrtContext p) { fake::current = p; return ACL_SUCCESS; }
aclError aclrtGetRunMode(aclrtRunMode* p) { *p = ACL_HOST; return ACL_SUCCESS; }
aclError aclrtCreateStream(aclrtStream* p) {
    fake::require(fake::current, "stream created without context");
    *p = new FakeStream{fake::current, {}}; ++fake::streams; return ACL_SUCCESS;
}
aclError aclrtDestroyStream(aclrtStream p) {
    fake::require(p->tasks.empty(), "destroyed stream with pending work");
    delete p; --fake::streams; return ACL_SUCCESS;
}
aclError aclrtSynchronizeStream(aclrtStream p) {
    fake::require(fake::current == p->context, "synchronization used wrong context");
    ++fake::syncs;
    int now = ++fake::active, old = fake::peak.load();
    while (old < now && !fake::peak.compare_exchange_weak(old, now)) {}
    {
        std::unique_lock<std::mutex> lock(fake::overlap_mutex);
        if (fake::await_overlap) {
            ++fake::overlap_arrivals;
            fake::overlap_ready.notify_all();
            if (!fake::overlap_ready.wait_for(lock, std::chrono::seconds(3), [] {
                    return fake::overlap_arrivals == 2;
                })) { --fake::active; return 1; }
        }
    }
    if (fake::fail_sync.exchange(0)) { p->tasks.clear(); --fake::active; return 1; }
    for (auto& task : p->tasks) task();
    p->tasks.clear(); --fake::active;
    return ACL_SUCCESS;
}
aclError aclrtMalloc(void** p, size_t bytes, int) { *p = ::operator new(bytes); return ACL_SUCCESS; }
aclError aclrtFree(void* p) { ::operator delete(p); return ACL_SUCCESS; }
aclError aclrtMallocHost(void** p, size_t bytes) {
    *p = ::operator new(bytes);
    std::lock_guard<std::mutex> lock(fake::mutex);
    fake::pinned.insert(*p); return ACL_SUCCESS;
}
aclError aclrtFreeHost(void* p) {
    std::lock_guard<std::mutex> lock(fake::mutex);
    fake::require(fake::pinned.erase(p) == 1, "freed unknown pinned buffer");
    ::operator delete(p); return ACL_SUCCESS;
}
aclError aclrtMemcpy(void* dst, size_t capacity, const void* src, size_t bytes, aclrtMemcpyKind) {
    fake::require(bytes <= capacity, "sync copy overflow");
    ++fake::sync_copies; std::memcpy(dst, src, bytes); return ACL_SUCCESS;
}
aclError aclrtMemcpyAsync(void* dst, size_t capacity, const void* src, size_t bytes,
                          aclrtMemcpyKind kind, aclrtStream stream) {
    fake::require(fake::current == stream->context, "copy used wrong context");
    fake::require(bytes <= capacity, "async copy overflow");
    {
        std::lock_guard<std::mutex> lock(fake::mutex);
        fake::require(fake::pinned.count(kind == ACL_MEMCPY_HOST_TO_DEVICE ? const_cast<void*>(src) : dst),
                      "async copy used pageable host memory");
    }
    ++fake::async_copies;
    stream->tasks.emplace_back([=] { std::memcpy(dst, src, bytes); });
    return ACL_SUCCESS;
}
aclDataBuffer* aclCreateDataBuffer(void* p, size_t n) { return new aclDataBuffer{p, n}; }
aclError aclDestroyDataBuffer(aclDataBuffer* p) { delete p; return ACL_SUCCESS; }
aclmdlDataset* aclmdlCreateDataset() { return new aclmdlDataset; }
aclError aclmdlDestroyDataset(aclmdlDataset* p) { delete p; return ACL_SUCCESS; }
aclError aclmdlAddDatasetBuffer(aclmdlDataset* d, aclDataBuffer* b) { d->buffers.push_back(b); return ACL_SUCCESS; }
aclError aclmdlLoadFromFile(const char* path, uint32_t* id) {
    *id = fake::next_model++;
    fake::batch_sizes[*id] = std::string(path).find("b4") == std::string::npos ? 1 : 4;
    return ACL_SUCCESS;
}
aclError aclmdlUnload(uint32_t) { return ACL_SUCCESS; }
aclmdlDesc* aclmdlCreateDesc() { return new aclmdlDesc; }
aclError aclmdlDestroyDesc(aclmdlDesc* p) { delete p; return ACL_SUCCESS; }
aclError aclmdlGetDesc(aclmdlDesc* p, uint32_t id) { p->id = id; return ACL_SUCCESS; }
size_t aclmdlGetNumInputs(const aclmdlDesc*) { return 1; }
size_t aclmdlGetNumOutputs(const aclmdlDesc*) { return 1; }
int aclmdlGetInputDataType(const aclmdlDesc*, size_t) { return ACL_FLOAT; }
int aclmdlGetOutputDataType(const aclmdlDesc*, size_t) { return ACL_FLOAT; }
aclError aclmdlGetInputDims(const aclmdlDesc* desc, size_t, aclmdlIODims* d) {
    *d = {2, {int64_t(fake::batch_sizes.at(desc->id)), 2}}; return ACL_SUCCESS;
}
aclError aclmdlGetOutputDims(const aclmdlDesc* desc, size_t, aclmdlIODims* d) {
    *d = {2, {int64_t(fake::batch_sizes.at(desc->id)), 2}}; return ACL_SUCCESS;
}
size_t aclmdlGetInputSizeByIndex(const aclmdlDesc* desc, size_t) {
    return 2 * sizeof(float) * fake::batch_sizes.at(desc->id);
}
size_t aclmdlGetOutputSizeByIndex(const aclmdlDesc* desc, size_t) {
    return 2 * sizeof(float) * fake::batch_sizes.at(desc->id);
}
const char* aclmdlGetInputNameByIndex(const aclmdlDesc*, size_t) { return "input"; }
const char* aclmdlGetOutputNameByIndex(const aclmdlDesc*, size_t) { return "output"; }
aclError aclmdlExecute(uint32_t, const aclmdlDataset* in, aclmdlDataset* out) {
    fake::execute(in, out); return ACL_SUCCESS;
}
aclError aclmdlExecuteAsync(uint32_t, const aclmdlDataset* in, aclmdlDataset* out, aclrtStream stream) {
    fake::require(fake::current == stream->context, "execute used wrong context");
    if (fake::fail_execute.exchange(0)) return 1;
    stream->tasks.emplace_back([=] { fake::execute(in, out); });
    return ACL_SUCCESS;
}

int main() {
    try {
        using namespace omniocr;
        auto make_input = [](float x) { return std::vector<Tensor>{{"input", {1, 2}, {x, x + 1}}}; };
        auto verify = [&](TensorEngine& engine, float x) {
            auto output = engine.run(make_input(x));
            fake::require(output.size() == 1 && output[0].name == "output" &&
                          output[0].shape == std::vector<int64_t>({1, 2}) &&
                          output[0].data == std::vector<float>({2*x, 2*x+2}), "incorrect ACL output");
        };
        Json cfg = {{"path", "fake.om"}, {"device_ids", {0}}, {"acl_async_stream", true}};
        {
            auto a = make_acl_engine(cfg, 0);
            auto b = make_acl_engine(cfg, 1);
            fake::require(fake::streams == 2, "each instance needs its own stream");
            fake::await_overlap = true;
            auto first = std::async(std::launch::async, [&] { verify(*a, 3); });
            auto second = std::async(std::launch::async, [&] { verify(*b, 7); });
            first.get(); second.get();
            fake::await_overlap = false;
            fake::require(fake::peak == 2, "separate model streams did not overlap");
            verify(*a, 11);
            fake::require(fake::syncs == 3 && fake::async_copies == 6 && fake::sync_copies == 0,
                          "async path must enqueue H2D, execute and D2H with one sync per call");
            fake::fail_execute = 1;
            try { verify(*a, 1); throw std::runtime_error("expected enqueue failure"); }
            catch (const std::runtime_error& e) {
                fake::require(std::string(e.what()).find("aclmdlExecuteAsync") != std::string::npos,
                              "unexpected failure during enqueue");
            }
            verify(*a, 2); // Failed enqueue drains queued H2D before reuse.
            fake::fail_sync = 1;
            try { verify(*a, 1); throw std::runtime_error("expected sync failure"); }
            catch (const std::runtime_error& e) {
                fake::require(std::string(e.what()).find("aclrtSynchronizeStream") != std::string::npos,
                              "unexpected synchronization error");
            }
            try { verify(*a, 2); throw std::runtime_error("expected poisoned instance"); }
            catch (const std::runtime_error& e) {
                fake::require(std::string(e.what()).find("discard this model instance") != std::string::npos,
                              "failed stream was reused");
            }
            verify(*b, 5);
        }
        fake::require(fake::streams == 0 && fake::pinned.empty(), "ACL stream/pinned buffers leaked");
        cfg["path"] = "fake-b4.om";
        cfg["batch_size"] = 4;
        {
            auto batched = make_acl_engine(cfg, 0);
            fake::require(batched->fixed_batch_size() == 4, "static ACL batch dimension was lost");
            const auto result = batched->run({{"input", {4, 2}, {1, 2, 3, 4, 5, 6, 7, 8}}});
            fake::require(result.size() == 1 && result[0].shape == std::vector<int64_t>({4, 2}) &&
                          result[0].data == std::vector<float>({2, 4, 6, 8, 10, 12, 14, 16}),
                          "static B4 ACL result was not transferred correctly");
        }
        fake::require(fake::streams == 0 && fake::pinned.empty(), "batched ACL resources leaked");
        cfg["path"] = "fake.om";
        cfg.erase("batch_size");
        cfg["acl_async_stream"] = false;
        {
            auto legacy = make_acl_engine(cfg, 0);
            verify(*legacy, 9);
            fake::require(fake::streams == 0 && fake::sync_copies == 2,
                          "synchronous compatibility path not used");
        }
        cfg.erase("acl_async_stream");
        {
            auto default_engine = make_acl_engine(cfg, 0);
            fake::require(fake::streams == 1, "default ACL mode must create a stream");
            verify(*default_engine, 1);
            fake::require(fake::sync_copies == 2,
                          "default ACL mode must use asynchronous copies");
        }
        fake::require(fake::streams == 0 && fake::pinned.empty(), "default ACL stream leaked");
        std::cout << "PASS: ACL stream ordering, pinned buffers, parallel instances and cleanup\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
