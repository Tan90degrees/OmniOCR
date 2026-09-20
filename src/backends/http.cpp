#include "omniocr/core.hpp"
#include <curl/curl.h>
#include <cstdlib>
#include <stdexcept>

namespace omniocr {
Json post_json(const Json& settings, const Json& payload) {
    struct CurlRuntime {
        CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT)) throw std::runtime_error("curl init failed"); }
        ~CurlRuntime() { curl_global_cleanup(); }
    };
    static CurlRuntime runtime;
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl handle allocation failed");
    const auto url = settings.at("endpoint").get<std::string>();
    const auto body = payload.dump();
    struct Response { std::string body; size_t limit; } response{{}, settings.value("max_response_bytes", size_t(16777216))};
    auto receive = +[](char* ptr, size_t size, size_t nmemb, void* user) -> size_t {
        auto& r = *static_cast<Response*>(user);
        const size_t bytes = size * nmemb;
        if (bytes > r.limit - r.body.size()) return 0;
        try { r.body.append(ptr, bytes); } catch (...) { return 0; }
        return bytes;
    };
    curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
    auto key_env = settings.value("api_key_env", std::string{});
    if (!key_env.empty()) {
        auto* key = std::getenv(key_env.c_str());
        if (!key || !*key) { curl_slist_free_all(raw_headers); throw std::runtime_error("missing API key environment variable: " + key_env); }
        std::string header = "Authorization: Bearer " + std::string(key);
        if (header.find_first_of("\r\n") != std::string::npos) { curl_slist_free_all(raw_headers); throw std::runtime_error("invalid API key"); }
        raw_headers = curl_slist_append(raw_headers, header.c_str());
    }
    auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(raw_headers, curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, long(settings.value("connect_timeout_seconds", 10)));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, long(settings.value("timeout_seconds", 120)));
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    const auto rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) throw std::runtime_error(std::string("HTTP transport failed: ") + curl_easy_strerror(rc));
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw std::runtime_error("HTTP status " + std::to_string(status));
    return Json::parse(response.body);
}
} // namespace omniocr
