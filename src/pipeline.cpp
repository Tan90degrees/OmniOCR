#include "omniocr/core.hpp"
#include "omniocr/plugins.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace omniocr {
Pipeline::Pipeline(Json config, ModelFactory factory) : config_(normalize_config(config)) {
    validate_config(config_);
    models_ = std::make_unique<ModelRegistry>(config_.at("models"), std::move(factory));
}
Page Pipeline::process_page(int number, const Image& image, const fs::path& output_dir,
                            int box_workers, const BoxSubmit& submit) {
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
        // Only worker_count crops exist simultaneously. BOX tasks rejoin the
        // global pool after each region so a dense page cannot monopolize it.
        std::atomic<size_t> next{0}; std::atomic<bool> stop{false};
        std::exception_ptr error; std::mutex error_mutex;
        auto work_one = [&](size_t i) {
            Region& region = page.regions[i];
            try {
                region.box = boxes[i];
                const Json* route = nullptr;
                if (routes.contains(region.box.type)) route = &routes.at(region.box.type);
                else if (routes.contains("*")) route = &routes.at("*");
                else throw std::runtime_error("no route for box type: " + region.box.type);
                const auto action = route->value("action", std::string("recognize"));
                if (action == "skip") return;
                if (route->value("cropper", std::string("bbox_crop")) == "polygon_mask_crop" &&
                    region.box.polygon.empty() && route->value("crop_fallback", std::string{}) == "bbox_crop")
                    region.box.extensions["omniocr.crop_fallback"] = "bbox_crop";
                auto crop = crop_region(image, region.box, *route);
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
                            // A candidate is successful only after task-specific adapter decoding.
                            // Malformed model output is handled by the same candidate fallback.
                            auto decoded = decode_recognition(result, *route, region.box.type);
                            region.raw_text = std::move(decoded.raw_text);
                            region.text = std::move(decoded.text);
                            // Expose the configured v2 model binding while leasing its
                            // shared executor pool internally by candidate ID.
                            if (route->contains("binding_ids") && route->at("binding_ids").contains(candidate))
                                region.model = route->at("binding_ids").at(candidate).get<std::string>();
                            else region.model = route->value("binding_id", candidate);
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
        };
        auto work = [&] {
            while (!stop.load()) {
                const size_t i = next.fetch_add(1);
                if (i >= boxes.size()) break;
                work_one(i);
            }
        };
        const size_t worker_count = std::min(size_t(box_workers), boxes.size());
        // Batch/REST already execute on a bounded page worker. Avoid creating
        // and immediately joining another OS thread for every serial page.
        if (worker_count <= 1) {
            work();
            if (error) std::rethrow_exception(error);
            return page;
        }
        if (submit) {
            // Keep at most worker_count tasks in flight per page. Each task
            // processes one BOX then queues its successor at the tail, which
            // lets other pages take turns without growing the global queue
            // by one task per BOX on very dense pages.
            std::atomic<size_t> outstanding{0};
            std::mutex done_mutex;
            std::condition_variable done;
            std::function<void()> task;
            task = [&] {
                if (!stop.load()) {
                    const size_t i = next.fetch_add(1);
                    if (i < boxes.size()) work_one(i);
                }
                if (!stop.load() && next.load() < boxes.size()) {
                    outstanding.fetch_add(1);
                    try { (void)submit(task); }
                    catch (...) {
                        outstanding.fetch_sub(1);
                        stop = true;
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (!error) error = std::current_exception();
                    }
                }
                if (outstanding.fetch_sub(1) == 1) done.notify_one();
            };
            try {
                for (size_t i = 0; i < worker_count && !stop.load(); ++i) {
                    outstanding.fetch_add(1);
                    try { (void)submit(task); }
                    catch (...) { outstanding.fetch_sub(1); throw; }
                }
            } catch (...) {
                stop = true;
                std::unique_lock<std::mutex> lock(done_mutex);
                done.wait(lock, [&] { return outstanding.load() == 0; });
                throw;
            }
            std::unique_lock<std::mutex> lock(done_mutex);
            done.wait(lock, [&] { return outstanding.load() == 0; });
            if (error) std::rethrow_exception(error);
            return page;
        }
        std::vector<std::thread> threads;
        // Join existing workers if thread creation throws, before any page storage is released.
        try { for (size_t i = 0; i < worker_count; ++i) threads.emplace_back(work); }
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
