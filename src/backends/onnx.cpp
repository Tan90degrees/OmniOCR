#include "omniocr/tensor.hpp"
#include <stdexcept>
#ifdef OMNIOCR_WITH_ONNX
#include <onnxruntime_cxx_api.h>
#include <map>

namespace omniocr {
namespace {
class OnnxEngine final : public TensorEngine {
    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "omniocr"};
    Ort::SessionOptions options_;
    std::unique_ptr<Ort::Session> session_;
public:
    explicit OnnxEngine(const Json& c) {
        int threads = c.value("intra_op_threads", 1);
        if (threads < 1) throw std::runtime_error("invalid ONNX intra_op_threads");
        options_.SetIntraOpNumThreads(threads);
        options_.SetInterOpNumThreads(1);
        session_ = std::make_unique<Ort::Session>(env_, c.at("path").get<std::string>().c_str(), options_);
    }
    std::vector<Tensor> run(const std::vector<Tensor>& inputs) override {
        if (session_->GetInputCount() != inputs.size()) throw std::runtime_error("ONNX input count mismatch");
        Ort::AllocatorWithDefaultOptions allocator;
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<const char*> names;
        std::vector<Ort::Value> values;
        for (size_t i = 0; i < session_->GetInputCount(); ++i) {
            auto name = session_->GetInputNameAllocated(i, allocator);
            const Tensor* tensor = nullptr;
            for (const auto& input : inputs) if (input.name == name.get()) tensor = &input;
            if (!tensor) throw std::runtime_error("ONNX input name not configured: " + std::string(name.get()));
            auto type_info = session_->GetInputTypeInfo(i);
            auto info = type_info.GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) throw std::runtime_error("initial ONNX codecs require float32 inputs");
            auto expected = info.GetShape();
            if (expected.size() != tensor->shape.size()) throw std::runtime_error("ONNX input rank mismatch");
            for (size_t j = 0; j < expected.size(); ++j)
                if (expected[j] >= 0 && expected[j] != tensor->shape[j]) throw std::runtime_error("ONNX input shape mismatch");
            names.push_back(tensor->name.c_str());
            values.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(tensor->data.data()),
                tensor->data.size(), tensor->shape.data(), tensor->shape.size()));
        }
        std::vector<std::string> output_names;
        std::vector<const char*> output_ptrs;
        for (size_t i = 0; i < session_->GetOutputCount(); ++i)
            output_names.emplace_back(session_->GetOutputNameAllocated(i, allocator).get());
        for (const auto& name : output_names) output_ptrs.push_back(name.c_str());
        auto results = session_->Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(), output_ptrs.data(), output_ptrs.size());
        std::vector<Tensor> out;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto info = results[i].GetTensorTypeAndShapeInfo();
            // Non-float auxiliary outputs (e.g. bbox_num) retain their slot, but cannot be decoded.
            Tensor t; t.name = output_names[i]; t.shape = info.GetShape();
            if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                const float* ptr = results[i].GetTensorData<float>();
                t.data.assign(ptr, ptr + info.GetElementCount());
            }
            out.push_back(std::move(t));
        }
        return out;
    }
};
}
std::unique_ptr<TensorEngine> make_onnx_engine(const Json& c) { return std::make_unique<OnnxEngine>(c); }
}
#else
namespace omniocr {
std::unique_ptr<TensorEngine> make_onnx_engine(const Json&) {
    throw std::runtime_error("ONNX backend disabled; build with -DOMNIOCR_WITH_ONNX=ON");
}
}
#endif
