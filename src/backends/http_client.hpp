#pragma once
#include "omniocr/core.hpp"
#include <stdexcept>

namespace omniocr {
class HttpStatusError : public std::runtime_error {
public:
    explicit HttpStatusError(long status)
        : std::runtime_error("HTTP status " + std::to_string(status)), status(status) {}
    long status;
};
// One client per exclusively leased model instance. May move between worker
// threads, but must never execute concurrent requests on the same client.
class HttpClient {
public:
    explicit HttpClient(Json settings);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    Json post(const Json& payload);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace omniocr
