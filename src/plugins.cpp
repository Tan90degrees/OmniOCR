#include "omniocr/plugins.hpp"
#include <map>
#include <mutex>
#include <stdexcept>

namespace omniocr {
std::vector<Box> parse_legacy_layout(const Json&, const Json&, int, int);
std::vector<Box> parse_doclayout_v3(const Json&, const Json&, int, int);
std::unique_ptr<Model> make_legacy_model(const Json&, size_t);
Image polygon_mask_crop(const Image&, const Box&, const Json&);
namespace {
struct Registry {
    std::mutex mutex;
    std::map<std::string, LayoutAdapter> layout;
    std::map<std::string, CropAdapter> crops;
    std::map<std::string, RecognitionAdapter> recognition;
    std::map<std::string, BackendFactory> backends;
    Registry() {
        auto legacy = [](const Json& response, const Json& settings, int w, int h) {
            return parse_legacy_layout(response, settings, w, h);
        };
        for (const auto& name : {"paddle", "normalized", "mineru"}) layout.emplace(name, legacy);
        layout.emplace("paddle.doclayout_v3.http", parse_doclayout_v3);
        // Distinct names make semantic crop selection explicit in route config.
        crops.emplace("bbox_crop", [](const Image& img, const Box& b, const Json&) {
            return img.crop(b.bbox);
        });
        crops.emplace("polygon_mask_crop", polygon_mask_crop);
        recognition.emplace("text", [](const Json& raw, const Json&) {
            return RecognitionResult{raw.at("text").get<std::string>(), ""};
        });
        recognition.emplace("table", [](const Json& raw, const Json&) {
            auto text = raw.at("text").get<std::string>();
            return RecognitionResult{table_to_html(text), text};
        });
        recognition.emplace("vlm.ovisocr2", recognition.at("text"));
        recognition.emplace("vlm.generic", recognition.at("text"));
        recognition.emplace("ctc", recognition.at("text"));
        recognition.emplace("table.otsl", recognition.at("table"));
        for (const auto& name : {"vllm", "http_json", "onnx", "acl", "mock"})
            backends.emplace(name, [](const Json& settings, size_t index) {
                return make_legacy_model(settings, index);
            });
    }
};
Registry& registry() { static Registry value; return value; }
template<class Container, class Callback> void put(Container& map, const std::string& name, Callback callback) {
    if (name.empty() || !callback || !map.emplace(name, std::move(callback)).second)
        throw std::runtime_error("duplicate or invalid plugin ID: " + name);
}
} // namespace
void register_layout_adapter(std::string id, LayoutAdapter fn) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); put(r.layout, id, std::move(fn));
}
void register_crop_adapter(std::string id, CropAdapter fn) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); put(r.crops, id, std::move(fn));
}
void register_recognition_adapter(std::string id, RecognitionAdapter fn) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); put(r.recognition, id, std::move(fn));
}
void register_backend(std::string id, BackendFactory fn) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); put(r.backends, id, std::move(fn));
}
bool has_layout_adapter(const std::string& id) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return r.layout.count(id);
}
bool has_crop_adapter(const std::string& id) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return r.crops.count(id);
}
bool has_recognition_adapter(const std::string& id) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return r.recognition.count(id);
}
bool has_backend(const std::string& id) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return r.backends.count(id);
}
std::vector<Box> parse_layout(const Json& response, const Json& settings, int w, int h) {
    const auto name = settings.value("adapter", settings.at("provider").get<std::string>());
    LayoutAdapter fn;
    { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.layout.find(name);
      if (it == r.layout.end()) throw std::runtime_error("unknown layout adapter: " + name);
      fn = it->second;
    }
    return fn(response, settings, w, h);
}
Image crop_region(const Image& image, const Box& box, const Json& route) {
    auto name = route.value("cropper", std::string("bbox_crop"));
    CropAdapter fn;
    { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.crops.find(name);
      if (it == r.crops.end()) throw std::runtime_error("unknown cropper: " + name);
      fn = it->second;
    }
    return fn(image, box, route);
}
RecognitionResult decode_recognition(const Json& raw, const Json& route, const std::string& box_type) {
    auto name = route.value("adapter", box_type=="table" ? std::string("table") : std::string("text"));
    RecognitionAdapter fn;
    { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.recognition.find(name);
      if (it == r.recognition.end()) throw std::runtime_error("unknown recognition adapter: " + name);
      fn = it->second;
    }
    return fn(raw, route);
}
std::unique_ptr<Model> create_backend_model(const Json& config, size_t index) {
    const auto name = config.at("backend").get<std::string>();
    BackendFactory fn;
    { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.backends.find(name);
      if (it == r.backends.end()) throw std::runtime_error("unknown backend plugin: " + name);
      fn = it->second;
    }
    return fn(config, index);
}
std::unique_ptr<Model> make_model(const Json& config, size_t index) {
    return create_backend_model(config, index);
}
} // namespace omniocr
