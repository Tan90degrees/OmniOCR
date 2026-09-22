#pragma once
#include "omniocr/core.hpp"

namespace omniocr {
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
