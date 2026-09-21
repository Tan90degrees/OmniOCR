#include "omniocr/core.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace omniocr {
Pipeline::Pipeline(Json config, ModelFactory factory) : config_(std::move(config)) {
    validate_config(config_);
    models_ = std::make_unique<ModelRegistry>(config_.at("models"), std::move(factory));
}
Page Pipeline::process_page(int number, const Image& image, const fs::path& output_dir,
                            int box_workers) {
    const auto execution = config_.value("execution", Json::object());
    const auto& layout = config_.at("layout");
    const auto& routes = config_.at("routes");
    const bool record = execution.value("on_error", "fail") == "record";
        Image small;
        const Image* layout_image = &image;
        if (layout.contains("image_size")) {
            small = image.resize(layout.at("image_size").at(0), layout.at("image_size").at(1));
            layout_image = &small;
        }
        auto response = models_->infer(layout.at("model"), *layout_image,
            layout.value("prompt", layout.at("provider") == "mineru" ? "\nLayout Detection:" : ""));
        auto boxes = parse_layout(response, layout, image.width, image.height);
        Page page; page.number = number; page.width = image.width; page.height = image.height;
        page.regions.resize(boxes.size());
        // Only worker_count crops exist simultaneously; no per-box async or unbounded queue.
        std::atomic<size_t> next{0}; std::atomic<bool> stop{false};
        std::exception_ptr error; std::mutex error_mutex;
        auto work = [&] {
            while (!stop.load()) {
                const size_t i = next.fetch_add(1);
                if (i >= boxes.size()) break;
                Region& region = page.regions[i]; region.box = boxes[i];
                try {
                    const Json* route = nullptr;
                    if (routes.contains(region.box.type)) route = &routes.at(region.box.type);
                    else if (routes.contains("*")) route = &routes.at("*");
                    else throw std::runtime_error("no route for box type: " + region.box.type);
                    const auto action = route->value("action", std::string("recognize"));
                    if (action == "skip") continue;
                    auto crop = image.crop(region.box.bbox);
                    if (region.box.rotation) crop = crop.rotate(region.box.rotation);
                    if (action == "image" || route->value("save_crop", false)) {
                        fs::create_directories(output_dir / "assets");
                        region.asset = "assets/page-" + std::to_string(number) + "-box-" + std::to_string(i) + ".png";
                        const auto bytes = crop.png();
                        std::ofstream out(output_dir / region.asset, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                        if (!out) throw std::runtime_error("cannot write crop asset");
                    }
                    if (action == "recognize") {
                        std::vector<std::string> candidates;
                        if (route->contains("models")) candidates = route->at("models").get<std::vector<std::string>>();
                        else candidates = {route->at("model").get<std::string>()};
                        std::string failures;
                        bool recognized = false;
                        for (const auto& candidate : candidates) {
                            try {
                                auto result = models_->infer(candidate, crop, route->value("prompt", "Text Recognition:"));
                                auto text = result.at("text").get<std::string>();
                                // Decode inside the candidate boundary: malformed table tokens
                                // must trigger the next model, not bypass route fallback.
                                std::string decoded = region.box.type == "table" ? table_to_html(text) : text;
                                region.raw_text = region.box.type == "table" ? text : "";
                                region.text = std::move(decoded);
                                region.model = candidate;
                                recognized = true;
                                break;
                            } catch (const std::exception& e) {
                                if (!failures.empty()) failures += "; ";
                                failures += candidate + ": " + e.what();
                            }
                        }
                        if (!recognized) throw std::runtime_error("all recognition models failed: " + failures);
                    }
                } catch (const std::exception& e) {
                    if (record) region.error = e.what();
                    else { std::lock_guard<std::mutex> lock(error_mutex); if (!error) error = std::current_exception(); stop = true; }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(error_mutex); if (!error) error = std::current_exception(); stop = true;
                }
            }
        };
        std::vector<std::thread> threads;
        // Join existing workers if thread creation throws, before any page storage is released.
        try { for (size_t i = 0; i < std::min(size_t(box_workers), boxes.size()); ++i) threads.emplace_back(work); }
        catch (...) { stop = true; for (auto& t : threads) t.join(); throw; }
        for (auto& t : threads) t.join();
        if (error) std::rethrow_exception(error);
    return page;
}
Document Pipeline::run(const fs::path& input, const fs::path& output_dir) {
    if (fs::exists(output_dir) && !fs::is_empty(output_dir))
        throw std::runtime_error("output directory must be empty");
    fs::create_directories(output_dir);
    Document doc; doc.source = fs::absolute(input).string();
    const int box_workers = config_.value("execution", Json::object()).value("workers", 4);
    read_document(input, config_.value("document", Json::object()),
        [&](int number, const Image& image) {
            doc.pages.push_back(process_page(number, image, output_dir, box_workers));
        });
    return doc;
}
} // namespace omniocr
