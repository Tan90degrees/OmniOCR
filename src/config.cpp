#include "omniocr/core.hpp"
#include <fstream>
#include <set>
#include <stdexcept>

namespace omniocr {
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error("config: " + message);
}
void positive(const Json& j, const char* key, int fallback, int max) {
    const int value = j.value(key, fallback);
    require(value > 0 && value <= max, std::string(key) + " out of range");
}
}
void validate_config(const Json& c) {
    require(c.value("version", 0) == 1, "version must be 1");
    const auto& models = c.at("models");
    require(models.is_object() && !models.empty(), "models must be a nonempty object");
    for (const auto& [id, m] : models.items()) {
        require(!id.empty(), "empty model ID");
        positive(m, "instances", 1, 128);
        positive(m, "acquire_timeout_ms", 60000, 3600000);
        const auto backend = m.at("backend").get<std::string>();
        require(std::set<std::string>{"vllm", "http_json", "onnx", "acl", "mock"}.count(backend), "unknown backend " + backend);
        if (backend == "vllm" || backend == "http_json") {
            const auto url = m.at("endpoint").get<std::string>();
            require(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0, "endpoint must be an HTTP(S) URL");
            positive(m, "timeout_seconds", 120, 3600);
            positive(m, "connect_timeout_seconds", 10, 3600);
            positive(m, "max_response_bytes", 16777216, 268435456);
            if (backend == "vllm") require(!m.at("model").get<std::string>().empty(), "missing served model name");
        }
        if (backend == "onnx" || backend == "acl") {
            require(!m.at("path").get<std::string>().empty(), "missing model path");
            auto decoder = m.at("decoder").at("type").get<std::string>();
            require(decoder == "ctc" || decoder == "paddle_layout", "unsupported tensor decoder");
            positive(m.at("preprocess"), "width", 0, 8192);
            positive(m.at("preprocess"), "height", 0, 8192);
            const auto& inputs = m.at("inputs");
            require(inputs.is_array() && !inputs.empty(), "inputs must be a nonempty array");
            std::set<std::string> names;
            for (const auto& input : inputs) {
                require(names.insert(input.at("name").get<std::string>()).second, "duplicate input name");
                require(std::set<std::string>{"image", "original_shape", "scale_factor", "constant"}.count(input.at("source").get<std::string>()), "unknown tensor input source");
            }
            if (backend == "acl") {
                const auto ids = m.value("device_ids", std::vector<int>{0});
                require(!ids.empty(), "device_ids cannot be empty");
                for (int device : ids) require(device >= 0, "negative device ID");
            }
        }
    }
    const auto& layout = c.at("layout");
    const auto threshold = layout.value("score_threshold", 0.0);
    require(threshold >= 0 && threshold <= 1, "layout score_threshold must be in [0,1]");
    require(models.contains(layout.at("model").get<std::string>()), "unknown layout model");
    require(std::set<std::string>{"paddle", "mineru", "normalized"}.count(layout.at("provider").get<std::string>()), "unknown layout provider");
    const auto coordinates = layout.value("coordinates", std::string("pixel"));
    require(coordinates == "pixel" || coordinates == "normalized", "invalid coordinates");
    positive(layout, "max_boxes", 2000, 100000);
    if (layout.contains("image_size")) {
        auto size = layout.at("image_size").get<std::vector<int>>();
        require(size.size() == 2 && size[0] > 0 && size[1] > 0 && size[0] <= 8192 && size[1] <= 8192, "invalid layout image_size");
        require(layout.at("provider") == "mineru", "layout image_size is only supported for MinerU normalized token coordinates");
    }
    const auto& routes = c.at("routes");
    require(routes.is_object() && !routes.empty(), "routes must be a nonempty object");
    for (const auto& [type, route] : routes.items()) {
        auto action = route.value("action", std::string("recognize"));
        require(action == "recognize" || action == "skip" || action == "image", "invalid route action");
        if (action == "recognize") {
            const bool single = route.contains("model"), multiple = route.contains("models");
            require(single != multiple, "recognition route must specify exactly one of model or models for " + type);
            if (single) {
                require(models.contains(route.at("model").get<std::string>()), "unknown route model for " + type);
            } else {
                const auto& choices = route.at("models");
                require(choices.is_array() && !choices.empty(), "models must be a nonempty array for " + type);
                std::set<std::string> seen;
                for (const auto& choice : choices) {
                    require(choice.is_string(), "route model IDs must be strings for " + type);
                    const auto id = choice.get<std::string>();
                    require(models.contains(id), "unknown route model " + id + " for " + type);
                    require(seen.insert(id).second, "duplicate route model " + id + " for " + type);
                }
            }
        }
    }
    const auto exec = c.value("execution", Json::object());
    positive(exec, "workers", 4, 128);
    require(exec.value("on_error", "fail") == "fail" || exec.value("on_error", "fail") == "record", "invalid on_error");
    const auto doc = c.value("document", Json::object());
    positive(doc, "dpi", 150, 1200);
    positive(doc, "max_pages", 1000, 100000);
    positive(doc, "timeout_seconds", 120, 3600);
    positive(doc, "max_pixels", 40000000, 200000000);
}
Json load_config(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config: " + path.string());
    Json c; in >> c;
    validate_config(c);
    // Model assets are resolved relative to the config, never the process cwd.
    for (auto& m : c["models"]) {
        if (m.contains("path")) m["path"] = fs::absolute(path.parent_path() / m["path"].get<std::string>()).string();
        if (m.contains("decoder") && m["decoder"].contains("dictionary"))
            m["decoder"]["dictionary"] = fs::absolute(path.parent_path() / m["decoder"]["dictionary"].get<std::string>()).string();
    }
    return c;
}
} // namespace omniocr
