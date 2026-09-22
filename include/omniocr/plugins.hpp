#pragma once
#include "omniocr/core.hpp"
#include <functional>
#include <string>
#include <vector>

namespace omniocr {
// Built-in adapters are registered once per process. Startup registration is
// permitted; all registration must finish before Pipeline workers start.
using LayoutAdapter = std::function<std::vector<Box>(const Json&, const Json&, int, int)>;
using CropAdapter = std::function<Image(const Image&, const Box&, const Json&)>;
struct RecognitionResult { std::string text, raw_text; };
using RecognitionAdapter = std::function<RecognitionResult(const Json&, const Json&)>;
using BackendFactory = std::function<std::unique_ptr<Model>(const Json&, size_t)>;

void register_layout_adapter(std::string id, LayoutAdapter adapter);
void register_crop_adapter(std::string id, CropAdapter adapter);
void register_recognition_adapter(std::string id, RecognitionAdapter adapter);
void register_backend(std::string id, BackendFactory factory);
bool has_layout_adapter(const std::string& id);
bool has_crop_adapter(const std::string& id);
bool has_recognition_adapter(const std::string& id);
bool has_backend(const std::string& id);
std::vector<Box> parse_layout(const Json&, const Json&, int width, int height);
Image crop_region(const Image&, const Box&, const Json& route);
RecognitionResult decode_recognition(const Json&, const Json& route);
// Built-in model executors are registered here; individual model adapters do
// not own or recreate model weights. ModelRegistry controls leasing.
std::unique_ptr<Model> create_backend_model(const Json&, size_t index);
} // namespace omniocr
