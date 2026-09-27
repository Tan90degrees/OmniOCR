#include "omniocr/tensor.hpp"
#include <stdexcept>
#ifdef OMNIOCR_WITH_ACL
#include <acl/acl.h>
#include <map>
#include <mutex>
#include <set>
#include <cstring>

namespace omniocr {
namespace {
void check(aclError rc, const char* operation) {
    if (rc != ACL_SUCCESS) throw std::runtime_error(std::string(operation) + " failed, ACL code=" + std::to_string(rc));
}
struct Runtime {
    std::mutex mutex;
    std::set<int> devices;
    Runtime() { check(aclInit(nullptr), "aclInit"); }
    void select(int device) {
        std::lock_guard<std::mutex> lock(mutex);
        check(aclrtSetDevice(device), "aclrtSetDevice"); devices.insert(device);
    }
    ~Runtime() { for (int device : devices) aclrtResetDevice(device); aclFinalize(); }
};
Runtime& runtime() { static Runtime instance; return instance; }
struct DeviceBuffer {
    void* ptr = nullptr;
    aclDataBuffer* buffer = nullptr;
    size_t bytes = 0;
    explicit DeviceBuffer(size_t n) : bytes(n) {
        if (!n) throw std::runtime_error("zero-size ACL buffer");
        check(aclrtMalloc(&ptr, n, ACL_MEM_MALLOC_NORMAL_ONLY), "aclrtMalloc");
        buffer = aclCreateDataBuffer(ptr, n);
        if (!buffer) { aclrtFree(ptr); ptr = nullptr; throw std::runtime_error("aclCreateDataBuffer failed"); }
    }
    ~DeviceBuffer() { if (buffer) aclDestroyDataBuffer(buffer); if (ptr) aclrtFree(ptr); }
};
struct Dataset {
    aclmdlDataset* data = aclmdlCreateDataset();
    std::vector<std::unique_ptr<DeviceBuffer>> buffers;
    Dataset() { if (!data) throw std::runtime_error("aclmdlCreateDataset failed"); }
    void append(size_t bytes) {
        auto buffer = std::make_unique<DeviceBuffer>(bytes);
        check(aclmdlAddDatasetBuffer(data, buffer->buffer), "aclmdlAddDatasetBuffer");
        buffers.push_back(std::move(buffer));
    }
    ~Dataset() { aclmdlDestroyDataset(data); }
};
struct HostBuffer {
    void* ptr = nullptr;
    explicit HostBuffer(size_t bytes) {
        if (!bytes) throw std::runtime_error("zero-size ACL host buffer");
        check(aclrtMallocHost(&ptr, bytes), "aclrtMallocHost");
    }
    ~HostBuffer() { if (ptr) aclrtFreeHost(ptr); }
};
class AclEngine final : public TensorEngine {
    aclrtContext context_ = nullptr;
    uint32_t model_id_ = 0;
    bool loaded_ = false;
    aclmdlDesc* desc_ = nullptr;
    std::unique_ptr<Dataset> inputs_, outputs_;
    aclrtStream stream_ = nullptr;
    std::vector<std::unique_ptr<HostBuffer>> host_inputs_, host_outputs_;
    struct InputInfo { std::string name; std::vector<int64_t> shape; };
    std::vector<InputInfo> input_info_;
    size_t fixed_batch_ = 0;
    bool async_stream_ = true, stream_failed_ = false;
    void clear() noexcept {
        if (context_) aclrtSetCurrentContext(context_);
        // Do not release pinned/device buffers while tasks might still use them.
        if (stream_) { aclrtSynchronizeStream(stream_); aclrtDestroyStream(stream_); stream_ = nullptr; }
        host_outputs_.clear(); host_inputs_.clear();
        outputs_.reset(); inputs_.reset();
        if (desc_) { aclmdlDestroyDesc(desc_); desc_ = nullptr; }
        if (loaded_) { aclmdlUnload(model_id_); loaded_ = false; }
        if (context_) { aclrtDestroyContext(context_); context_ = nullptr; }
    }
public:
    AclEngine(const Json& c, size_t index) {
        async_stream_ = c.value("acl_async_stream", true);
        const bool batching = c.value("batch_size", 1) > 1;
        const auto devices = c.value("device_ids", std::vector<int>{0});
        if (devices.empty()) throw std::runtime_error("empty ACL device_ids");
        const int device = devices[index % devices.size()];
        try {
            runtime().select(device);
            check(aclrtCreateContext(&context_, device), "aclrtCreateContext");
            if (async_stream_) check(aclrtCreateStream(&stream_), "aclrtCreateStream");
            aclrtRunMode mode;
            check(aclrtGetRunMode(&mode), "aclrtGetRunMode");
            if (mode != ACL_HOST) throw std::runtime_error("initial ACL backend requires host mode");
            check(aclmdlLoadFromFile(c.at("path").get<std::string>().c_str(), &model_id_), "aclmdlLoadFromFile");
            loaded_ = true;
            desc_ = aclmdlCreateDesc();
            if (!desc_) throw std::runtime_error("aclmdlCreateDesc failed");
            check(aclmdlGetDesc(desc_, model_id_), "aclmdlGetDesc");
            inputs_ = std::make_unique<Dataset>(); outputs_ = std::make_unique<Dataset>();
            for (size_t i = 0; i < aclmdlGetNumInputs(desc_); ++i) {
                if (aclmdlGetInputDataType(desc_, i) != ACL_FLOAT) throw std::runtime_error("initial ACL codecs require float32 inputs");
                aclmdlIODims dims; check(aclmdlGetInputDims(desc_, i, &dims), "aclmdlGetInputDims");
                if (!dims.dimCount || dims.dims[0] <= 0) {
                    if (batching) throw std::runtime_error("ACL batching requires a static leading batch dimension");
                } else {
                    if (batching && fixed_batch_ && fixed_batch_ != size_t(dims.dims[0]))
                        throw std::runtime_error("ACL input batch dimensions differ");
                    fixed_batch_ = size_t(dims.dims[0]);
                }
                for (size_t j = 0; j < dims.dimCount; ++j)
                    if (dims.dims[j] <= 0) throw std::runtime_error("dynamic OM inputs need a model-specific adapter");
                const auto* name = aclmdlGetInputNameByIndex(desc_, i);
                if (!name) throw std::runtime_error("ACL input has no name");
                InputInfo info; info.name = name;
                for (size_t j = 0; j < dims.dimCount; ++j) info.shape.push_back(dims.dims[j]);
                input_info_.push_back(std::move(info));
                const auto bytes = aclmdlGetInputSizeByIndex(desc_, i);
                inputs_->append(bytes);
                if (async_stream_) host_inputs_.push_back(std::make_unique<HostBuffer>(bytes));
            }
            for (size_t i = 0; i < aclmdlGetNumOutputs(desc_); ++i) {
                const auto bytes = aclmdlGetOutputSizeByIndex(desc_, i);
                outputs_->append(bytes);
                if (async_stream_ && aclmdlGetOutputDataType(desc_, i) == ACL_FLOAT)
                    host_outputs_.push_back(std::make_unique<HostBuffer>(bytes));
                else host_outputs_.push_back(nullptr);
            }
        } catch (...) { clear(); throw; }
    }
    ~AclEngine() override { clear(); }
    size_t fixed_batch_size() const override { return fixed_batch_; }
    std::vector<Tensor> run(const std::vector<Tensor>& tensors) override {
        // Leases can move between caller threads; explicitly restore the instance context.
        check(aclrtSetCurrentContext(context_), "aclrtSetCurrentContext");
        if (stream_failed_) throw std::runtime_error("ACL stream failed; discard this model instance");
        if (tensors.size() != inputs_->buffers.size()) throw std::runtime_error("ACL input count mismatch");
        // Check every input before enqueueing any work on this instance.
        std::vector<const Tensor*> ordered;
        ordered.reserve(input_info_.size());
        for (size_t i = 0; i < tensors.size(); ++i) {
            const Tensor* tensor = nullptr;
            for (const auto& t : tensors) if (t.name == input_info_[i].name) tensor = &t;
            if (!tensor) throw std::runtime_error("ACL input name not configured: " + input_info_[i].name);
            if (input_info_[i].shape != tensor->shape) throw std::runtime_error("ACL input shape mismatch");
            const auto& buffer = inputs_->buffers[i];
            if (buffer->bytes != tensor->data.size() * sizeof(float)) throw std::runtime_error("ACL input byte size mismatch");
            ordered.push_back(tensor);
        }
        if (async_stream_) {
            // One stream per independently leased model ID, with a single
            // synchronization after H2D, execute and D2H have been queued.
            bool submitted = false;
            try {
                for (size_t i = 0; i < ordered.size(); ++i) {
                    const auto bytes = inputs_->buffers[i]->bytes;
                    std::memcpy(host_inputs_[i]->ptr, ordered[i]->data.data(), bytes);
                    submitted = true;
                    check(aclrtMemcpyAsync(inputs_->buffers[i]->ptr, bytes, host_inputs_[i]->ptr,
                                           bytes, ACL_MEMCPY_HOST_TO_DEVICE, stream_), "aclrtMemcpyAsync H2D");
                }
                check(aclmdlExecuteAsync(model_id_, inputs_->data, outputs_->data, stream_), "aclmdlExecuteAsync");
                for (size_t i = 0; i < outputs_->buffers.size(); ++i)
                    if (host_outputs_[i]) {
                        const auto bytes = outputs_->buffers[i]->bytes;
                        check(aclrtMemcpyAsync(host_outputs_[i]->ptr, bytes, outputs_->buffers[i]->ptr,
                                               bytes, ACL_MEMCPY_DEVICE_TO_HOST, stream_), "aclrtMemcpyAsync D2H");
                    }
                const auto rc = aclrtSynchronizeStream(stream_);
                submitted = false;
                if (rc != ACL_SUCCESS) { stream_failed_ = true; check(rc, "aclrtSynchronizeStream"); }
            } catch (...) {
                // A failed enqueue may leave earlier tasks in flight. Keep
                // their buffers alive until the stream has drained.
                if (submitted && aclrtSynchronizeStream(stream_) != ACL_SUCCESS) stream_failed_ = true;
                throw;
            }
        } else {
            for (size_t i = 0; i < ordered.size(); ++i) {
                const auto bytes = inputs_->buffers[i]->bytes;
                check(aclrtMemcpy(inputs_->buffers[i]->ptr, bytes, ordered[i]->data.data(),
                                  bytes, ACL_MEMCPY_HOST_TO_DEVICE), "aclrtMemcpy H2D");
            }
            check(aclmdlExecute(model_id_, inputs_->data, outputs_->data), "aclmdlExecute");
        }
        std::vector<Tensor> result;
        for (size_t i = 0; i < outputs_->buffers.size(); ++i) {
            Tensor t;
            const auto* name = aclmdlGetOutputNameByIndex(desc_, i); t.name = name ? name : "";
            aclmdlIODims dims; check(aclmdlGetOutputDims(desc_, i, &dims), "aclmdlGetOutputDims");
            size_t count = 1;
            for (size_t j = 0; j < dims.dimCount; ++j) {
                if (dims.dims[j] <= 0 || uint64_t(dims.dims[j]) > outputs_->buffers[i]->bytes / sizeof(float) / count)
                    throw std::runtime_error("unsupported ACL output shape");
                t.shape.push_back(dims.dims[j]); count *= size_t(dims.dims[j]);
            }
            if (aclmdlGetOutputDataType(desc_, i) == ACL_FLOAT) {
                t.data.resize(count);
                if (async_stream_) std::memcpy(t.data.data(), host_outputs_[i]->ptr, count * sizeof(float));
                else check(aclrtMemcpy(t.data.data(), count * sizeof(float), outputs_->buffers[i]->ptr,
                                      count * sizeof(float), ACL_MEMCPY_DEVICE_TO_HOST), "aclrtMemcpy D2H");
            }
            result.push_back(std::move(t));
        }
        return result;
    }
};
}
std::unique_ptr<TensorEngine> make_acl_engine(const Json& c, size_t i) { return std::make_unique<AclEngine>(c, i); }
}
#else
namespace omniocr {
std::unique_ptr<TensorEngine> make_acl_engine(const Json&, size_t) {
    throw std::runtime_error("ACL backend disabled; build with -DOMNIOCR_WITH_ACL=ON");
}
}
#endif
