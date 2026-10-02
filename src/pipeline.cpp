#include "omniocr/core.hpp"
#include "omniocr/plugins.hpp"
#include "recognition_pool.hpp"
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
    const auto settings = config_.value("execution", Json::object())
        .value("async_recognition", Json::object());
    if (settings.value("enabled", false))
        recognition_pool_ = std::make_unique<RecognitionTaskPool>(settings);
}
Pipeline::~Pipeline() = default;
Json Pipeline::recognition_metrics() {
    return recognition_pool_ ? recognition_pool_->metrics() : Json{{"enabled", false}};
}
Page Pipeline::process_page(int number, const Image& image, const fs::path& output_dir,
                            int box_workers, const BoxSubmit& submit, const CancellationToken& cancel) {
    CancellationScope page_scope(cancel.get());
    throw_if_cancelled();
    const auto execution = config_.value("execution", Json::object());
    const auto& layout = config_.at("layout");
    const auto& routes = config_.at("routes");
    const bool record = execution.value("on_error", "fail") == "record";
        auto detect = [&](const Image& original) {
            Image small;
            const Image* layout_image=&original;
            if (layout.contains("image_size")) {
                small=original.resize(layout.at("image_size").at(0),layout.at("image_size").at(1));
                layout_image=&small;
            }
            Image limited;
            const auto size=models_->input_size(layout.at("model"),*layout_image);
            if (size[0]!=layout_image->width || size[1]!=layout_image->height) {
                limited=layout_image->resize(size[0],size[1]);
                layout_image=&limited;
            }
            auto response=models_->infer(layout.at("model"),*layout_image,
                layout.value("prompt",layout.at("provider")=="mineru" ? "\nLayout Detection:" : ""));
            Json coordinates=layout;
            const auto space=layout.value("coordinates",std::string("pixel"));
            if (space=="model_input" && !layout.contains("transform"))
                coordinates["image_size"]={layout_image->width,layout_image->height};
            if (space!="pixel" || (layout_image->width==original.width && layout_image->height==original.height))
                return parse_layout(response,coordinates,original.width,original.height);
            // Pixel-space detector output belongs to the actual model input.
            // Map boxes and crop metadata back to the rendered page before OCR.
            auto boxes=parse_layout(response,coordinates,layout_image->width,layout_image->height);
            const double sx=double(original.width)/layout_image->width;
            const double sy=double(original.height)/layout_image->height;
            for (auto& box:boxes) {
                box.bbox={box.bbox[0]*sx,box.bbox[1]*sy,box.bbox[2]*sx,box.bbox[3]*sy};
                if (box.crop_bbox)
                    box.crop_bbox=std::array<double,4>{(*box.crop_bbox)[0]*sx,(*box.crop_bbox)[1]*sy,
                                                       (*box.crop_bbox)[2]*sx,(*box.crop_bbox)[3]*sy};
                for (auto& point:box.polygon) {point[0]*=sx;point[1]*=sy;}
            }
            return boxes;
        };
        auto boxes=postprocess_boxes(detect(image),
            config_.value("postprocess",Json::object()),image,detect);
        Page page; page.number = number; page.width = image.width; page.height = image.height;
        page.regions.resize(boxes.size());
        // Preparation rejoins the shared BOX pool after each region. Optional
        // recognition work has its own request/byte bounds and page lifetime.
        std::atomic<size_t> next{0}; std::atomic<bool> stop{false};
        std::exception_ptr error; std::mutex error_mutex;
        struct PendingRecognition {
            std::mutex mutex;
            std::condition_variable done;
            size_t count = 0;
            void add() { std::lock_guard<std::mutex> lock(mutex); ++count; }
            void finish() { std::lock_guard<std::mutex> lock(mutex); if (!--count) done.notify_all(); }
            void wait() { std::unique_lock<std::mutex> lock(mutex); done.wait(lock, [&] { return count == 0; }); }
            ~PendingRecognition() { wait(); }
        } pending;
        auto record_error = [&](size_t i, std::exception_ptr failure) {
            try { std::rethrow_exception(failure); }
            catch (const Cancelled&) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!error) error = failure;
                stop = true;
            } catch (const std::exception& e) {
                if (record) page.regions[i].error = e.what();
                else { std::lock_guard<std::mutex> lock(error_mutex); if (!error) error = failure; stop = true; }
            } catch (...) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!error) error = failure;
                stop = true;
            }
        };
        auto work_one = [&](size_t i) {
            CancellationScope box_scope(cancel.get());
            Region& region = page.regions[i];
            try {
                throw_if_cancelled();
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
                    const auto bytes = crop.rgb.size();
                    auto recognize = [&, i, route, owned = std::make_shared<Image>(std::move(crop))] {
                        CancellationScope recognition_scope(cancel.get());
                        throw_if_cancelled();
                        if (stop.load()) throw Cancelled();
                        auto& region = page.regions[i];
                        std::vector<std::string> candidates;
                        if (route->contains("models")) candidates = route->at("models").get<std::vector<std::string>>();
                        else candidates = {route->at("model").get<std::string>()};
                        std::string failures;
                        bool recognized = false;
                        for (const auto& candidate : candidates) {
                            try {
                                auto result = models_->infer(candidate, *owned, route->value("prompt", "Text Recognition:"));
                                throw_if_cancelled();
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
                            } catch (const Cancelled&) { throw; }
                            catch (const std::exception& e) {
                                if (!failures.empty()) failures += "; ";
                                failures += candidate + ": " + e.what();
                            }
                        }
                        if (!recognized) throw std::runtime_error("all recognition models failed: " + failures);
                    };
                    if (recognition_pool_) {
                        pending.add();
                        try {
                            recognition_pool_->submit(bytes,
                                [i, recognize = std::move(recognize), record_error] {
                                    try { recognize(); }
                                    catch (...) { record_error(i, std::current_exception()); throw; }
                                }, [&] { return stop.load(); }, [&] { pending.finish(); });
                        } catch (...) { pending.finish(); throw; }
                    } else recognize();
                }
            } catch (...) { record_error(i, std::current_exception()); }
        };
        auto work = [&] {
            while (!stop.load() && !cancellation_requested()) {
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
            pending.wait();
            throw_if_cancelled();
            if (error) std::rethrow_exception(error);
            finalize_composite_page(page,config_.value("postprocess",Json::object()),output_dir);
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
                if (!stop.load() && (!cancel || !cancel->load(std::memory_order_relaxed))) {
                    const size_t i = next.fetch_add(1);
                    if (i < boxes.size()) work_one(i);
                }
                if (!stop.load() && (!cancel || !cancel->load(std::memory_order_relaxed)) && next.load() < boxes.size()) {
                    outstanding.fetch_add(1);
                    try { (void)submit(task); }
                    catch (...) {
                        outstanding.fetch_sub(1);
                        stop = true;
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (!error) error = std::current_exception();
                    }
                }
                if (outstanding.fetch_sub(1) == 1) {
                    std::lock_guard<std::mutex> guard(done_mutex);
                    done.notify_one();
                }
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
            pending.wait();
            throw_if_cancelled();
            if (error) std::rethrow_exception(error);
            finalize_composite_page(page,config_.value("postprocess",Json::object()),output_dir);
            return page;
        }
        std::vector<std::thread> threads;
        // Join existing workers if thread creation throws, before any page storage is released.
        try { for (size_t i = 0; i < worker_count; ++i) threads.emplace_back(work); }
        catch (...) { stop = true; for (auto& t : threads) t.join(); throw; }
        for (auto& t : threads) t.join();
        pending.wait();
        throw_if_cancelled();
        if (error) std::rethrow_exception(error);
    finalize_composite_page(page,config_.value("postprocess",Json::object()),output_dir);
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
    postprocess_document(doc,config_.value("postprocess",Json::object()));
    return doc;
}
} // namespace omniocr
