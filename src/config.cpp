#include "omniocr/core.hpp"
#include "omniocr/plugins.hpp"
#include <fstream>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

namespace omniocr {
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error("config: " + message);
}
void positive(const Json& j, const char* key, int fallback, int max) {
    if (!j.contains(key)) {
        require(fallback > 0 && fallback <= max, std::string(key) + " out of range");
        return;
    }
    const auto& value = j.at(key);
    require(value.is_number_integer() || value.is_number_unsigned(),
            std::string(key) + " must be an integer");
    if (value.is_number_unsigned()) {
        require(value.get<uint64_t>() >= 1 && value.get<uint64_t>() <= uint64_t(max),
                std::string(key) + " out of range");
    } else {
        require(value.get<int64_t>() >= 1 && value.get<int64_t>() <= int64_t(max),
                std::string(key) + " out of range");
    }
}
}
void validate_config(const Json& c) {
    if (c.is_object() && c.contains("version") && c.at("version") == 2) {
        validate_config(normalize_config(c));
        return;
    }
    load_plugins(c);
    require(c.contains("version") && c.at("version").is_number_integer() &&
            c.at("version").get<int64_t>() == 1, "version must be 1");
    const auto& models = c.at("models");
    require(models.is_object() && !models.empty(), "models must be a nonempty object");
    for (const auto& [id, m] : models.items()) {
        require(!id.empty(), "empty model ID");
        positive(m, "instances", 1, 128);
        positive(m, "acquire_timeout_ms", 60000, 3600000);
        const auto backend = m.at("backend").get<std::string>();
        require(has_backend(backend), "unknown backend plugin " + backend);
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
    const auto provider = layout.at("provider").get<std::string>();
    const auto adapter = layout.value("adapter", provider);
    require(has_layout_adapter(adapter), "unknown layout adapter " + adapter);
    const auto coordinates = layout.value("coordinates", std::string("pixel"));
    require(coordinates == "pixel" || coordinates == "normalized" ||
            (adapter == "paddle.doclayout_v3.http" && coordinates == "model_input"), "invalid coordinates");
    positive(layout, "max_boxes", 2000, 100000);
    if (layout.contains("image_size")) {
        auto size = layout.at("image_size").get<std::vector<int>>();
        require(size.size() == 2 && size[0] > 0 && size[1] > 0 && size[0] <= 8192 && size[1] <= 8192, "invalid layout image_size");
        require(provider == "mineru" || (adapter == "paddle.doclayout_v3.http" && coordinates == "model_input"),
                "layout image_size requires MinerU or V3 model_input coordinates");
    }
    const auto& routes = c.at("routes");
    require(routes.is_object() && !routes.empty(), "routes must be a nonempty object");
    for (const auto& [type, route] : routes.items()) {
        if (route.contains("cropper")) {
            const auto cropper = route.at("cropper").get<std::string>();
            require(has_crop_adapter(cropper), "unknown cropper " + cropper);
            if (route.contains("crop_fallback"))
                require(cropper == "polygon_mask_crop" &&
                        route.at("crop_fallback") == "bbox_crop", "invalid crop_fallback");
            if (route.contains("mask_background")) {
                const auto& color = route.at("mask_background");
                require(color.is_number_integer() || color.is_number_unsigned(),
                        "mask_background must be an integer");
                if (color.is_number_unsigned())
                    require(color.get<uint64_t>() <= 255, "mask_background out of range");
                else require(color.get<int64_t>() >= 0 && color.get<int64_t>() <= 255,
                             "mask_background out of range");
            }
        }
        if (route.contains("adapter"))
            require(has_recognition_adapter(route.at("adapter").get<std::string>()),
                    "unknown recognition adapter");
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
    positive(doc, "max_csv_bytes", 16777216, 268435456);
    const auto delimiter = doc.value("csv_delimiter", std::string(","));
    require(delimiter == "," || delimiter == ";" || delimiter == "\t" || delimiter == "|",
            "csv_delimiter must be comma, semicolon, tab or pipe");
    for (const auto* key : {"soffice", "pdfinfo", "pdftoppm", "ebook_convert", "ofd_converter"}) {
        if (doc.contains(key)) {
            require(doc.at(key).is_string(), std::string(key) + " must be an executable path");
            const auto executable = doc.at(key).get<std::string>();
            require(!executable.empty() && executable.find('\0') == std::string::npos,
                    std::string(key) + " must be a nonempty executable path without NUL");
        }
    }
}
Json load_config(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config: " + path.string());
    Json c; in >> c;
    if (c.contains("plugins")) for (auto& plugin : c["plugins"]) {
        if (plugin.contains("library")) plugin["library"] =
            fs::absolute(path.parent_path() / plugin["library"].get<std::string>()).string();
    }
    c = normalize_config(c);
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
