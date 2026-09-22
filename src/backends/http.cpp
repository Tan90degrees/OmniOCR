#include "http_client.hpp"
#include <curl/curl.h>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace omniocr {
namespace {
void initialize_curl() {
    struct CurlRuntime {
        CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT)) throw std::runtime_error("curl init failed"); }
        ~CurlRuntime() { curl_global_cleanup(); }
    };
    static CurlRuntime runtime;
}
} // namespace
struct HttpClient::Impl {
    Json settings;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{nullptr, curl_easy_cleanup};
    explicit Impl(Json value) : settings(std::move(value)) {
        initialize_curl();
        curl.reset(curl_easy_init());
        if (!curl) throw std::runtime_error("curl handle allocation failed");
    }
};
HttpClient::HttpClient(Json settings) : impl_(std::make_unique<Impl>(std::move(settings))) {}
HttpClient::~HttpClient() = default;

Json HttpClient::post(const Json& payload) {
    const auto& settings = impl_->settings;
    const auto& curl = impl_->curl;
    const auto url = settings.at("endpoint").get<std::string>();
    const auto body = payload.dump();
    struct Response { std::string body; size_t limit; } response{{}, settings.value("max_response_bytes", size_t(16777216))};
    auto receive = +[](char* ptr, size_t size, size_t nmemb, void* user) -> size_t {
        auto& r = *static_cast<Response*>(user);
        if (size && nmemb > std::numeric_limits<size_t>::max() / size) return 0;
        const size_t bytes = size * nmemb;
        if (bytes > r.limit - r.body.size()) return 0;
        try { r.body.append(ptr, bytes); } catch (...) { return 0; }
        return bytes;
    };
    auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(nullptr, curl_slist_free_all);
    auto add_header = [&](const char* value) {
        auto* next = curl_slist_append(headers.get(), value);
        if (!next) throw std::runtime_error("HTTP header allocation failed");
        headers.release();
        headers.reset(next);
    };
    add_header("Content-Type: application/json");
    auto key_env = settings.value("api_key_env", std::string{});
    if (!key_env.empty()) {
        auto* key = std::getenv(key_env.c_str());
        if (!key || !*key) { throw std::runtime_error("missing API key environment variable: " + key_env); }
        std::string header = "Authorization: Bearer " + std::string(key);
        if (header.find_first_of("\r\n") != std::string::npos) { throw std::runtime_error("invalid API key"); }
        add_header(header.c_str());
    }
    // Reset request pointers before local bodies/headers disappear, including on
    // exceptions. easy_reset retains the live connection, DNS and TLS caches.
    struct ResetOptions {
        CURL* handle;
        ~ResetOptions() { curl_easy_reset(handle); }
    } reset{curl.get()};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXCONNECTS, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, long(settings.value("connect_timeout_seconds", 10)));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, long(settings.value("timeout_seconds", 120)));
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    const auto rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) throw std::runtime_error(std::string("HTTP transport failed: ") + curl_easy_strerror(rc));
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw std::runtime_error("HTTP status " + std::to_string(status));
    return Json::parse(response.body);
}
// Preserve the public one-shot helper for existing custom models.
Json post_json(const Json& settings, const Json& payload) {
    return HttpClient(settings).post(payload);
}
} // namespace omniocr
