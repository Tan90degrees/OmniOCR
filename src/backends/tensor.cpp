#include "omniocr/tensor.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace omniocr {
namespace {
size_t elements(const std::vector<int64_t>& shape) {
    size_t count = 1;
    for (auto d : shape) {
        if (d < 0 || (count && uint64_t(d) > std::numeric_limits<size_t>::max() / count))
            throw std::runtime_error("invalid tensor shape");
        count *= size_t(d);
    }
    return count;
}
class LocalModel final : public Model {
    Json config_;
    std::unique_ptr<TensorEngine> engine_;
public:
    LocalModel(Json config, std::unique_ptr<TensorEngine> engine) : config_(std::move(config)), engine_(std::move(engine)) {
        const auto configured = size_t(config_.value("batch_size", 1));
        if (configured > 1 && engine_->fixed_batch_size() && engine_->fixed_batch_size() != configured)
            throw std::runtime_error("tensor model batch_size does not match its static input batch dimension");
        auto& decoder = config_["decoder"];
        if (decoder.value("type", "") == "ctc" && decoder.contains("dictionary")) {
            std::ifstream in(decoder.at("dictionary").get<std::string>());
            if (!in) throw std::runtime_error("cannot open CTC dictionary");
            std::vector<std::string> vocab;
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                vocab.push_back(line);
            }
            // Dictionary must include the blank entry at blank_index.
            decoder["vocabulary"] = vocab;
        }
    }
    Json infer(const Image& image, const std::string&) override {
        return decode_tensors(engine_->run(preprocess(image, config_)), image, config_);
    }
    bool supports_batch() const override { return true; }
    std::vector<Json> infer_batch(const std::vector<BatchInput>& inputs) override {
        if (inputs.empty() || inputs.size() > size_t(config_.value("batch_size", 1)))
            throw std::runtime_error("invalid tensor batch size");
        const size_t physical = engine_->fixed_batch_size() ? engine_->fixed_batch_size() : inputs.size();
        if (physical < inputs.size()) throw std::runtime_error("tensor model batch capacity exceeded");
        std::vector<std::vector<Tensor>> prepared;
        prepared.reserve(inputs.size());
        for (const auto& item : inputs) {
            if (!item.image) throw std::runtime_error("null batch image");
            prepared.push_back(preprocess(*item.image, config_));
        }
        // Static OM/ONNX models execute at their exported batch size. Reuse
        // the final prepared input when filling the aggregate tensor, without
        // allocating another full preprocessing buffer for each padding item.
        // Keep the first prepared sample available when it is also the padding
        // source; moving it would leave a one-item tail with no input tensors.
        auto combined = physical > 1 ? prepared[0] : std::move(prepared[0]);
        for (auto& t : combined) {
            if (t.shape.empty() || t.shape.front() != 1)
                throw std::runtime_error("tensor batch input must have leading dimension 1");
            t.data.reserve(t.data.size() * physical);
            t.shape.front() = int64_t(physical);
        }
        for (size_t sample = 1; sample < physical; ++sample)
            for (size_t i = 0; i < combined.size(); ++i) {
                const auto& part = prepared[std::min(sample, prepared.size() - 1)][i];
                if (part.name != combined[i].name || part.shape.size() != combined[i].shape.size() ||
                    part.shape.empty() || part.shape.front() != 1 ||
                    !std::equal(part.shape.begin() + 1, part.shape.end(), combined[i].shape.begin() + 1))
                    throw std::runtime_error("tensor inputs cannot be grouped into a batch");
                combined[i].data.insert(combined[i].data.end(), part.data.begin(), part.data.end());
            }
        const auto outputs = engine_->run(combined);
        const auto& decoder = config_.at("decoder");
        const size_t output_index = decoder.value("output_index", size_t(0));
        if (output_index >= outputs.size()) throw std::runtime_error("decoder output index out of range");
        const auto& output = outputs[output_index];
        const auto type = decoder.at("type").get<std::string>();
        if (output.shape.size() != 3 || output.shape[0] != int64_t(physical) ||
            (type == "paddle_layout" && output.shape[2] != 6) ||
            (type != "paddle_layout" && type != "ctc") ||
            output.data.size() % physical)
            throw std::runtime_error("batched tensor output must be [B,N,6] for layout or [B,T,C] for CTC");
        const size_t stride = output.data.size() / physical;
        std::vector<Json> results;
        results.reserve(inputs.size());
        for (size_t sample = 0; sample < inputs.size(); ++sample) {
            std::vector<Tensor> one(outputs.size());
            auto& t = one[output_index];
            t.name = output.name;
            t.shape = type == "paddle_layout" ?
                std::vector<int64_t>{output.shape[1], output.shape[2]} :
                std::vector<int64_t>{1, output.shape[1], output.shape[2]};
            t.data.assign(output.data.begin() + sample * stride,
                          output.data.begin() + (sample + 1) * stride);
            results.push_back(decode_tensors(one, *inputs[sample].image, config_));
        }
        return results;
    }
};
}
std::unique_ptr<Model> make_local_model(const Json& c, std::unique_ptr<TensorEngine> e) {
    return std::make_unique<LocalModel>(c, std::move(e));
}
std::vector<Tensor> preprocess(const Image& image, const Json& config) {
    const auto& p = config.at("preprocess");
    const int width = p.at("width"), height = p.at("height");
    const auto mean = p.value("mean", std::array<float, 3>{0, 0, 0});
    const auto stddev = p.value("std", std::array<float, 3>{1, 1, 1});
    const float scale = p.value("scale", 1.0f / 255);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(mean[i]) || !std::isfinite(stddev[i]) || stddev[i] <= 0 || !std::isfinite(scale))
            throw std::runtime_error("invalid image normalization");
    auto resized = image.resize(width, height);
    const auto color = p.value("color", std::string("rgb"));
    if (color != "rgb" && color != "bgr") throw std::runtime_error("color must be rgb or bgr");
    std::vector<Tensor> tensors;
    for (const auto& spec : config.at("inputs")) {
        Tensor t; t.name = spec.at("name").get<std::string>();
        const auto source = spec.at("source").get<std::string>();
        if (source == "image") {
            t.shape = {1, 3, height, width}; t.data.resize(size_t(3) * height * width);
            for (int c = 0; c < 3; ++c) for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                t.data[(size_t(c) * height + y) * width + x] =
                    (resized.rgb[(size_t(y) * width + x) * 3 + (color == "bgr" ? 2 - c : c)] * scale - mean[c]) / stddev[c];
        } else if (source == "original_shape") {
            t.shape = {1, 2}; t.data = {float(image.height), float(image.width)};
        } else if (source == "scale_factor") {
            t.shape = {1, 2}; t.data = {float(height) / image.height, float(width) / image.width};
        } else if (source == "constant") {
            t.shape = spec.at("shape").get<std::vector<int64_t>>();
            t.data = spec.at("data").get<std::vector<float>>();
        } else throw std::runtime_error("unsupported tensor input source");
        if (elements(t.shape) != t.data.size()) throw std::runtime_error("input shape/data mismatch");
        tensors.push_back(std::move(t));
    }
    return tensors;
}
Json decode_tensors(const std::vector<Tensor>& outputs, const Image& image, const Json& config) {
    const auto& d = config.at("decoder");
    const size_t index = d.value("output_index", size_t(0));
    if (index >= outputs.size()) throw std::runtime_error("decoder output index out of range");
    const auto& t = outputs[index];
    if (elements(t.shape) != t.data.size()) throw std::runtime_error("output shape/data mismatch");
    for (float f : t.data) if (!std::isfinite(f)) throw std::runtime_error("non-finite output tensor");
    if (d.at("type") == "paddle_layout") {
        if (t.shape.size() != 2 || t.shape.back() != 6) throw std::runtime_error("paddle_layout expects [N,6]: class,score,x1,y1,x2,y2");
        const auto labels = d.at("labels").get<std::vector<std::string>>();
        const auto space = d.value("coordinates", std::string("pixel"));
        if (space != "pixel" && space != "input") throw std::runtime_error("unsupported paddle coordinate space");
        Json boxes = Json::array();
        for (size_t i = 0; i < t.data.size(); i += 6) {
            if (t.data[i + 1] < d.value("score_threshold", .5f)) continue;
            const float cls = t.data[i];
            if (cls < 0 || cls >= labels.size() || std::floor(cls) != cls) throw std::runtime_error("unknown layout class ID");
            std::array<double, 4> bbox{t.data[i+2], t.data[i+3], t.data[i+4], t.data[i+5]};
            if (space == "input") for (int k = 0; k < 4; ++k)
                bbox[k] *= k % 2 ? double(image.height) / config.at("preprocess").at("height").get<int>()
                                : double(image.width) / config.at("preprocess").at("width").get<int>();
            boxes.push_back({{"label", labels[size_t(cls)]}, {"score", t.data[i+1]}, {"coordinate", bbox}});
        }
        return {{"boxes", boxes}};
    }
    if (d.at("type") == "ctc") {
        if (t.shape.size() != 3 || t.shape[0] != 1) throw std::runtime_error("CTC expects [1,T,C]");
        const auto vocab = d.at("vocabulary").get<std::vector<std::string>>();
        const size_t classes = size_t(t.shape[2]);
        if (vocab.size() != classes) throw std::runtime_error("CTC vocabulary size does not match output classes");
        const int blank = d.value("blank_index", 0);
        if (blank < 0 || size_t(blank) >= classes) throw std::runtime_error("invalid CTC blank_index");
        int previous = -1; std::string text;
        for (size_t i = 0; i < t.data.size(); i += classes) {
            auto start = t.data.begin() + i;
            int token = int(std::max_element(start, start + classes) - start);
            if (token != blank && token != previous) text += vocab[size_t(token)];
            previous = token;
        }
        return {{"text", text}};
    }
    throw std::runtime_error("unsupported tensor decoder");
}
} // namespace omniocr
