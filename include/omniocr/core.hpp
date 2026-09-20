#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace omniocr {
using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
    static Image load(const fs::path&, uint64_t max_pixels);
    Image crop(const std::array<double, 4>&) const;
    Image resize(int w, int h) const;
    Image rotate(int degrees) const;
    std::vector<uint8_t> png() const;
};
struct Box {
    std::string type, raw_type;
    std::array<double, 4> bbox{};
    double score = 1;
    int order = 0, rotation = 0;
};
struct Region {
    Box box;
    std::string model, text, raw_text, error, asset;
};
struct Page {
    int number = 0, width = 0, height = 0;
    std::vector<Region> regions;
};
struct Document {
    std::string source;
    std::vector<Page> pages;
};

class Model {
public:
    virtual ~Model() = default;
    virtual Json infer(const Image&, const std::string& prompt) = 0;
};
using ModelFactory = std::function<std::unique_ptr<Model>(const Json&, size_t)>;
std::unique_ptr<Model> make_model(const Json&, size_t instance);

// Registry owns one bounded pool per model ID. Calls using the same ID share it.
class ModelRegistry {
public:
    explicit ModelRegistry(const Json& models, ModelFactory factory = make_model);
    ~ModelRegistry();
    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;
    Json infer(const std::string& id, const Image&, const std::string& prompt);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Json load_config(const fs::path&);
void validate_config(const Json&);
std::vector<Box> parse_layout(const Json& response, const Json& layout, int width, int height);
std::string base64(const std::vector<uint8_t>&);
Json post_json(const Json& settings, const Json& payload);

// Linux process isolation, argv only (no shell), timeout kills/reaps process group.
std::string run_process(const std::vector<std::string>& argv, int timeout_seconds);
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    fs::path path;
};
void read_document(const fs::path&, const Json& settings,
                   const std::function<void(int, const Image&)>& consume);

class Pipeline {
public:
    explicit Pipeline(Json config, ModelFactory factory = make_model);
    Document run(const fs::path& input, const fs::path& output_dir);
private:
    Json config_;
    std::unique_ptr<ModelRegistry> models_;
};
Json document_json(const Document&);
std::string document_markdown(const Document&);
std::string table_to_html(const std::string&);
void write_outputs(const Document&, const fs::path& directory, const std::string& format);
} // namespace omniocr
