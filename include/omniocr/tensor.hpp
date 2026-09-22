#pragma once
#include "omniocr/core.hpp"

namespace omniocr {
struct Tensor {
    std::string name;
    std::vector<int64_t> shape;
    // Initial codecs accept float32 tensors; other dtypes fail explicitly.
    std::vector<float> data;
};
class TensorEngine {
public:
    virtual ~TensorEngine() = default;
    virtual std::vector<Tensor> run(const std::vector<Tensor>&) = 0;
};
std::unique_ptr<TensorEngine> make_onnx_engine(const Json&);
std::unique_ptr<TensorEngine> make_acl_engine(const Json&, size_t instance);
std::unique_ptr<Model> make_local_model(const Json&, std::unique_ptr<TensorEngine>);
std::vector<Tensor> preprocess(const Image&, const Json&);
Json decode_tensors(const std::vector<Tensor>&, const Image&, const Json&);
} // namespace omniocr
