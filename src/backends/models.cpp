#include "omniocr/tensor.hpp"
#include "http_client.hpp"
#include <stdexcept>

namespace omniocr {
namespace {
class HttpModel final : public Model {
    Json config_;
    HttpClient client_;
public:
    explicit HttpModel(Json config) : config_(std::move(config)), client_(config_) {}
    Json infer(const Image& image, const std::string& prompt) override {
        const auto data = "data:image/png;base64," + base64(image.png());
        if (config_.at("backend") == "http_json")
            return client_.post({{"image", data}, {"prompt", prompt}, {"width", image.width}, {"height", image.height}});
        Json messages = Json::array();
        if (config_.contains("system_prompt")) messages.push_back({{"role", "system"}, {"content", config_.at("system_prompt")}});
        messages.push_back({{"role", "user"}, {"content", Json::array({
            {{"type", "image_url"}, {"image_url", {{"url", data}}}},
            {{"type", "text"}, {"text", prompt}}
        })}});
        Json payload = config_.value("parameters", Json::object());
        payload["model"] = config_.at("model"); payload["messages"] = messages; payload["stream"] = false;
        if (!payload.contains("temperature")) payload["temperature"] = 0;
        if (!payload.contains("max_tokens")) payload["max_tokens"] = 4096;
        auto response = client_.post(payload);
        const auto& choice = response.at("choices").at(0);
        if (choice.value("finish_reason", std::string{}) == "length") throw std::runtime_error("vLLM output truncated; raise max_tokens");
        return {{"text", choice.at("message").at("content").get<std::string>()}};
    }
};
class MockModel final : public Model {
    Json config_;
public:
    explicit MockModel(Json c) : config_(std::move(c)) {}
    Json infer(const Image&, const std::string&) override { return config_.at("response"); }
};
}
std::unique_ptr<Model> make_legacy_model(const Json& config, size_t instance) {
    const auto backend = config.at("backend").get<std::string>();
    if (backend == "vllm" || backend == "http_json") return std::make_unique<HttpModel>(config);
    if (backend == "mock") return std::make_unique<MockModel>(config);
    if (backend == "onnx") return make_local_model(config, make_onnx_engine(config));
    if (backend == "acl") return make_local_model(config, make_acl_engine(config, instance));
    throw std::runtime_error("unknown backend: " + backend);
}
} // namespace omniocr
