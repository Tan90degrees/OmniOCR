#include "omniocr/core.hpp"
#include "omniocr/plugins.hpp"
#include <algorithm>
#include <fstream>
#include <cstdint>
#include <cmath>
#include <limits>
#include <map>
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
void bounded(const Json& j, const char* key, uint64_t low, uint64_t high) {
    if (!j.contains(key)) return;
    const auto& value = j.at(key);
    require(value.is_number_integer() || value.is_number_unsigned(),
            std::string(key) + " must be an integer");
    if (value.is_number_unsigned())
        require(value.get<uint64_t>() >= low && value.get<uint64_t>() <= high,
                std::string(key) + " out of range");
    else require(value.get<int64_t>() >= 0 && uint64_t(value.get<int64_t>()) >= low &&
                 uint64_t(value.get<int64_t>()) <= high, std::string(key) + " out of range");
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
        positive(m, "max_concurrent_requests", m.value("instances", 1), 128);
        positive(m, "acquire_timeout_ms", 60000, 3600000);
        positive(m, "batch_size", 1, 128);
        positive(m, "max_pending_requests", 256, 100000);
        const auto batch_size = m.value("batch_size", 1);
        int wait = 5;
        if (m.contains("max_batch_wait_ms")) {
            const auto& value = m.at("max_batch_wait_ms");
            require(value.is_number_integer() || value.is_number_unsigned(),
                    "max_batch_wait_ms must be an integer");
            if (value.is_number_unsigned())
                require(value.get<uint64_t>() <= 1000, "max_batch_wait_ms out of range");
            else require(value.get<int64_t>() >= 0 && value.get<int64_t>() <= 1000,
                         "max_batch_wait_ms out of range");
            wait = value.get<int>();
        }
        const auto overrides = m.value("instance_overrides", Json::array());
        require(overrides.is_array() && overrides.size() <= size_t(m.value("max_concurrent_requests", m.value("instances", 1))),
                "instance_overrides must be an array no longer than max_concurrent_requests");
        bool needs_batch = batch_size > 1;
        int largest_batch = batch_size;
        for (const auto& slot : overrides) {
            require(slot.is_object(), "instance override must be an object");
            for (const auto& [key, value] : slot.items())
                require(key == "batch_size" || key == "max_batch_wait_ms", "unknown instance override: " + key);
            positive(slot, "batch_size", batch_size, 128);
            const auto size = slot.value("batch_size", batch_size);
            const auto window = slot.value("max_batch_wait_ms", wait);
            require(!slot.contains("max_batch_wait_ms") ||
                    (slot.at("max_batch_wait_ms").is_number_integer() || slot.at("max_batch_wait_ms").is_number_unsigned()),
                    "instance max_batch_wait_ms must be an integer");
            require(window >= 0 && window <= 1000, "instance max_batch_wait_ms out of range");
            if (size > 1) require(window < m.value("acquire_timeout_ms", 60000),
                                  "instance max_batch_wait_ms must be less than acquire_timeout_ms");
            needs_batch |= size > 1;
            largest_batch = std::max(largest_batch, size);
        }
        require(m.value("max_pending_requests", 256) >= largest_batch,
                "max_pending_requests must be at least the largest instance batch_size");
        if (batch_size > 1) require(wait < m.value("acquire_timeout_ms", 60000),
                                    "max_batch_wait_ms must be less than acquire_timeout_ms");
        const auto backend = m.at("backend").get<std::string>();
        require(has_backend(backend), "unknown backend plugin " + backend);
        if (m.contains("input_resize")) {
            const auto& resize=m.at("input_resize");
            require(resize.is_object() && !resize.empty(),
                    "input_resize must be a nonempty object: " + id);
            for (const auto& [key,value]:resize.items()) {
                require(key=="max_width" || key=="max_height" || key=="max_pixels" ||
                        key=="min_width" || key=="min_height" || key=="min_pixels" || key=="factor",
                        "unknown input_resize setting: " + key);
                bounded(resize,key.c_str(),1,key=="max_pixels" || key=="min_pixels" ? 200000000 :
                    key=="factor" ? 4096 : 100000);
            }
            const int factor=resize.value("factor",1);
            const int min_w=resize.value("min_width",1),max_w=resize.value("max_width",100000);
            const int min_h=resize.value("min_height",1),max_h=resize.value("max_height",100000);
            const uint64_t min_p=resize.value("min_pixels",uint64_t(0));
            const uint64_t max_p=resize.value("max_pixels",std::numeric_limits<uint64_t>::max());
            require(min_w<=max_w && min_h<=max_h && min_p<=max_p,
                    "conflicting input_resize bounds: " + id);
            const uint64_t lo_w=uint64_t((min_w+factor-1)/factor)*factor;
            const uint64_t lo_h=uint64_t((min_h+factor-1)/factor)*factor;
            const uint64_t hi_w=uint64_t(max_w/factor)*factor;
            const uint64_t hi_h=uint64_t(max_h/factor)*factor;
            require(lo_w<=hi_w && lo_h<=hi_h && lo_w*lo_h<=max_p && hi_w*hi_h>=min_p,
                    "infeasible input_resize bounds: " + id);
        }
        if (m.contains("adaptive_concurrency")) {
            const auto& adaptive = m.at("adaptive_concurrency");
            require(adaptive.is_object(), "adaptive_concurrency must be an object");
            const std::set<std::string> fields = {"enabled", "min_concurrency", "initial_concurrency",
                "window_ms", "min_samples", "cooldown_ms", "latency_target_ms",
                "latency_guard_ratio", "latency_guard_windows", "token_budget",
                "min_token_budget", "initial_token_budget", "slow_start_gain",
                "probe_gain", "probe_step", "backoff_ratio",
                "image_pixels_per_token", "expected_output_tokens"};
            for (const auto& [key, value] : adaptive.items())
                require(fields.count(key) != 0, "unknown adaptive_concurrency setting: " + key);
            if (adaptive.contains("enabled"))
                require(adaptive.at("enabled").is_boolean(), "adaptive_concurrency.enabled must be boolean");
            bounded(adaptive, "min_concurrency", 1, 128);
            bounded(adaptive, "initial_concurrency", 1, 128);
            bounded(adaptive, "window_ms", 100, 60000);
            bounded(adaptive, "min_samples", 1, 10000);
            bounded(adaptive, "cooldown_ms", 0, 60000);
            bounded(adaptive, "latency_target_ms", 0, 3600000);
            bounded(adaptive, "latency_guard_windows", 1, 100);
            if (adaptive.contains("latency_guard_ratio")) {
                const auto& ratio = adaptive.at("latency_guard_ratio");
                require(ratio.is_number() && std::isfinite(ratio.get<double>()) &&
                        (ratio.get<double>() == 0 || (ratio.get<double>() >= 1.1 && ratio.get<double>() <= 100)),
                        "latency_guard_ratio must be 0 or between 1.1 and 100");
            }
            bounded(adaptive, "token_budget", 1, 1000000000);
            bounded(adaptive, "min_token_budget", 1, 1000000000);
            bounded(adaptive, "initial_token_budget", 1, 1000000000);
            for (const char* key : {"slow_start_gain", "probe_gain", "probe_step", "backoff_ratio"}) {
                if (!adaptive.contains(key)) continue;
                const auto& value = adaptive.at(key);
                require(value.is_number() && std::isfinite(value.get<double>()) &&
                        value.get<double>() > 0 && value.get<double>() < 1,
                        std::string(key) + " must be between 0 and 1");
            }
            bounded(adaptive, "image_pixels_per_token", 1, 1000000);
            bounded(adaptive, "expected_output_tokens", 1, 1000000);
            if (adaptive.value("enabled", false)) {
                require(backend == "vllm", "adaptive_concurrency requires vllm backend");
                require(!needs_batch && overrides.empty(),
                        "adaptive_concurrency requires batch_size 1 without instance_overrides");
                const int ceiling = m.value("max_concurrent_requests", m.value("instances", 1));
                const int minimum = adaptive.value("min_concurrency", 1);
                const int initial = adaptive.value("initial_concurrency", std::min(4, ceiling));
                require(minimum <= initial && initial <= ceiling,
                        "adaptive_concurrency requires min <= initial <= max_concurrent_requests");
                const int budget = adaptive.value("token_budget", 32768);
                const int floor = adaptive.value("min_token_budget", int(std::max<int64_t>(1,
                    int64_t(budget) * minimum / ceiling)));
                const int starting = adaptive.value("initial_token_budget", int(std::max<int64_t>(floor,
                    int64_t(budget) * initial / ceiling)));
                require(floor <= starting && starting <= budget,
                        "adaptive_concurrency requires min_token_budget <= initial_token_budget <= token_budget");
            }
        }
        if (backend == "vllm" || backend == "http_json") {
            const auto url = m.at("endpoint").get<std::string>();
            require(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0, "endpoint must be an HTTP(S) URL");
            positive(m, "timeout_seconds", 120, 3600);
            positive(m, "connect_timeout_seconds", 10, 3600);
            positive(m, "max_response_bytes", 16777216, 268435456);
            if (backend == "vllm") require(!m.at("model").get<std::string>().empty(), "missing served model name");
            if (backend == "vllm") require(!needs_batch,
                "vLLM chat API has no native batch request; configure batching in vLLM serving instead");
            if (backend == "http_json" && needs_batch) {
                const auto endpoint = m.at("batch_endpoint").get<std::string>();
                require(endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0,
                        "batch_endpoint must be an HTTP(S) URL");
            }
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
                if (m.contains("acl_async_stream"))
                    require(m.at("acl_async_stream").is_boolean(), "acl_async_stream must be boolean");
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
    if (layout.contains("transform")) {
        require(!models.at(layout.at("model").get<std::string>()).contains("input_resize"),
                "layout explicit transform cannot be combined with model input_resize");
        require(adapter == "paddle.doclayout_v3.http" && coordinates == "model_input" &&
                layout.contains("image_size"),
                "explicit transform requires V3 model_input and image_size");
        const auto& transform=layout.at("transform");
        require(transform.is_object() && transform.contains("matrix") &&
                transform.at("matrix").is_array() && transform.at("matrix").size()==9,
                "transform.matrix must have 9 numbers");
        for (const auto& value:transform.at("matrix"))
            require(value.is_number() && std::isfinite(value.get<double>()),
                    "transform.matrix contains invalid value");
    }
    const auto post=c.value("postprocess",Json::object());
    require(post.is_object(),"postprocess must be an object");
    auto ratio=[&](const Json& section,const char* key,double low,double high) {
        if (!section.contains(key)) return;
        const auto& value=section.at(key);
        require(value.is_number() && std::isfinite(value.get<double>()) &&
                value.get<double>()>=low && value.get<double>()<=high,
                std::string("postprocess.")+key+" out of range");
    };
    const std::map<std::string,std::set<std::string>> allowed={
        {"low_score",{"enabled","threshold"}},
        {"header_footer",{"enabled","types"}},
        {"overlap",{"enabled","iou_threshold","score_margin","label_priority"}},
        {"composite",{"enabled","outer_types","expand_inner_types","containment_threshold",
                      "retain_min_score","max_recovered_boxes","inspect_inner_text",
                      "expand_if_uncovered_text","min_inner_text_chars"}},
        {"cross_page_tables",{"enabled","edge_margin_ratio","x_overlap_threshold","require_header_match"}}};
    for (const auto& [name,section]:post.items()) {
        require(allowed.count(name)>0 && section.is_object(),"unknown or invalid postprocess section: "+name);
        for (const auto& [key,value]:section.items()) {
            require(allowed.at(name).count(key)>0,"unknown postprocess setting: "+name+"."+key);
            if (key=="enabled" || key=="require_header_match" || key=="inspect_inner_text" ||
                key=="expand_if_uncovered_text")
                require(value.is_boolean(),"postprocess "+key+" must be boolean");
            if (key=="types" || key=="label_priority" || key=="outer_types" || key=="expand_inner_types") {
                require(value.is_array(),"postprocess "+key+" must be an array");
                std::set<std::string> seen;
                for (const auto& item:value)
                    require(item.is_string() && !item.get<std::string>().empty() &&
                            seen.insert(item.get<std::string>()).second,
                            "postprocess "+key+" requires unique nonempty strings");
            }
        }
        for (const char* key:{"threshold","iou_threshold","score_margin","containment_threshold",
                              "retain_min_score","x_overlap_threshold"}) ratio(section,key,0,1);
        ratio(section,"edge_margin_ratio",0,0.5);
        bounded(section,"max_recovered_boxes",1,1000);
        bounded(section,"min_inner_text_chars",1,1000);
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
    const auto output = c.value("output", Json::object());
    require(output.is_object(), "output must be an object");
    if (output.contains("schema_version")) {
        const auto& schema=output.at("schema_version");
        require(schema.is_number_integer() &&
                (schema.get<int64_t>()==1 || schema.get<int64_t>()==2),
                "output.schema_version must be integer 1 or 2");
    }
    const auto exec = c.value("execution", Json::object());
    require(exec.is_object(), "execution must be an object");
    for (const auto& [key, value] : exec.items())
        require(std::set<std::string>{"workers", "page_workers", "box_workers", "document_workers",
                                      "max_queued_pages", "on_error"}.count(key), "unknown execution setting: " + key);
    positive(exec, "workers", 4, 128);
    positive(exec, "page_workers", exec.value("workers", 4), 128);
    positive(exec, "box_workers", 1, 128);
    positive(exec, "document_workers", 2, 32);
    positive(exec, "max_queued_pages", 2, 256);
    require(exec.value("on_error", "fail") == "fail" || exec.value("on_error", "fail") == "record", "invalid on_error");
    const auto server = c.value("server", Json::object());
    require(server.is_object(), "server must be an object");
    for (const auto& [key, value] : server.items())
        require(std::set<std::string>{"data_dir", "allowed_input_root", "host", "port", "api_key_env",
                                      "max_upload_bytes", "max_jobs", "max_active_jobs",
                                      "max_inflight_upload_bytes", "max_queued_page_bytes", "http_connections",
                                      "connection_timeout_seconds", "max_result_bytes", "max_asset_bytes"}.count(key),
                "unknown server setting: " + key);
    for (const auto* key : {"data_dir", "allowed_input_root", "api_key_env"})
        if (server.contains(key)) require(server.at(key).is_string() &&
            !server.at(key).get<std::string>().empty(), std::string("server.") + key + " must be a nonempty string");
    if (server.contains("host")) require(server.at("host").is_string() &&
        (server.at("host") == "127.0.0.1" || server.at("host") == "0.0.0.0"),
        "server.host must be 127.0.0.1 or 0.0.0.0");
    bounded(server, "port", 1, 65535);
    bounded(server, "max_upload_bytes", 1, 512ULL * 1024 * 1024);
    bounded(server, "max_jobs", 1, 100000);
    bounded(server, "max_active_jobs", 1, 100000);
    bounded(server, "max_inflight_upload_bytes", 1, 1ULL << 40);
    bounded(server, "max_queued_page_bytes", 1, 1ULL << 40);
    bounded(server, "http_connections", 16, 1024);
    bounded(server, "connection_timeout_seconds", 1, 3600);
    bounded(server, "max_result_bytes", 1, 1ULL << 40);
    bounded(server, "max_asset_bytes", 1, 1ULL << 40);
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
    if (c.contains("server"))
        for (const char* key : {"data_dir", "allowed_input_root"})
            if (c["server"].contains(key)) c["server"][key] =
                fs::absolute(path.parent_path() / c["server"][key].get<std::string>()).string();
    // Model assets are resolved relative to the config, never the process cwd.
    for (auto& m : c["models"]) {
        if (m.contains("path")) m["path"] = fs::absolute(path.parent_path() / m["path"].get<std::string>()).string();
        if (m.contains("decoder") && m["decoder"].contains("dictionary"))
            m["decoder"]["dictionary"] = fs::absolute(path.parent_path() / m["decoder"]["dictionary"].get<std::string>()).string();
    }
    return c;
}
} // namespace omniocr
