#include "omniocr/tensor.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace omniocr;
void expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void throws(F f) {
    bool thrown = false; try { f(); } catch (const std::exception&) { thrown = true; }
    expect(thrown, "expected an exception");
}
Image image() { return {20, 10, std::vector<uint8_t>(600, 255)}; }
Json config() {
    return {{"version", 1}, {"execution", {{"workers", 4}}},
        {"layout", {{"provider", "normalized"}, {"model", "layout"}, {"coordinates", "normalized"}}},
        {"models", {
            {"layout", {{"backend", "mock"}, {"response", {{"boxes", Json::array({
                {{"type", "text"}, {"bbox", {0, 0, 1, .5}}, {"order", 1}},
                {{"type", "title"}, {"bbox", {0, .5, 1, 1}}, {"order", 0}}
            })}}}}},
            {"shared", {{"backend", "mock"}, {"instances", 2}, {"response", {{"text", "中文 test"}}}}}
        }},
        {"routes", {{"text", {{"model", "shared"}}}, {"title", {{"model", "shared"}}}}}};
}
void layout_test() {
    auto c = config();
    auto boxes = parse_layout(c["models"]["layout"]["response"], c["layout"], 200, 100);
    expect(boxes[0].type == "title" && boxes[1].bbox[2] == 200, "normalized coordinates/order");
    auto mineru = parse_layout({{"text", "<|box_start|>10 20 900 800<|box_end|><|ref_start|>equation<|ref_end|><|rotate_right|>"}},
        {{"provider", "mineru"}, {"type_map", {{"equation", "formula"}}}}, 200, 100);
    expect(mineru.size() == 1 && mineru[0].bbox[0] == 2 && mineru[0].rotation == 90 && mineru[0].type == "formula", "MinerU parsing");
    throws([&] { parse_layout({{"text", "wrong model"}}, {{"provider", "mineru"}}, 20, 10); });
    auto bad = c["models"]["layout"]["response"];
    bad["boxes"][0]["bbox"] = {1, 1, 0, 0};
    throws([&] { parse_layout(bad, c["layout"], 20, 10); });
    auto paddle = parse_layout({{"res", {{"boxes", Json::array({{{"label", "text"}, {"coordinate", {-2, 0, 25, 8}}, {"score", .9}}})}}}},
        {{"provider", "paddle"}}, 20, 10);
    expect(paddle[0].bbox[0] == 0 && paddle[0].bbox[2] == 20, "box clipping");
}
void pool_test() {
    struct State { std::atomic<int> active{0}, peak{0}, constructed{0}; } state;
    struct Probe : Model {
        State& s; bool entered = false;
        explicit Probe(State& state) : s(state) { ++s.constructed; }
        Json infer(const Image&, const std::string& prompt) override {
            expect(!entered, "instance used concurrently"); entered = true;
            int current = ++s.active;
            int peak = s.peak.load(); while (peak < current && !s.peak.compare_exchange_weak(peak, current)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            --s.active; entered = false;
            if (prompt == "fail") throw std::runtime_error("injected failure");
            return {{"text", prompt}};
        }
    };
    ModelRegistry registry({{"shared", {{"instances", 2}, {"acquire_timeout_ms", 1000}}}},
        [&](const Json&, size_t) { return std::make_unique<Probe>(state); });
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 20; ++i) futures.push_back(std::async(std::launch::async, [&, i] {
        if (i % 3 == 0) throws([&] { registry.infer("shared", image(), "fail"); });
        else registry.infer("shared", image(), "ok");
    }));
    for (auto& future : futures) future.get();
    expect(state.constructed == 2 && state.peak == 2 && state.active == 0, "pool bound or lease leak");
    expect(registry.infer("shared", image(), "after")["text"] == "after", "lease not returned after exception");
    throws([&] { registry.infer("missing", image(), ""); });
    // Acquisition has its own timeout, independent of backend inference timeout.
    struct Gate : Model {
        std::promise<void>& started; std::shared_future<void> release;
        Gate(std::promise<void>& s, std::shared_future<void> r) : started(s), release(r) {}
        Json infer(const Image&, const std::string&) override { started.set_value(); release.wait(); return {{"text", ""}}; }
    };
    std::promise<void> started, release;
    auto released = release.get_future().share();
    ModelRegistry single({{"m", {{"instances", 1}, {"acquire_timeout_ms", 10}}}},
        [&](const Json&, size_t) { return std::make_unique<Gate>(started, released); });
    auto job = std::async(std::launch::async, [&] { single.infer("m", image(), ""); });
    started.get_future().wait();
    bool timed_out = false;
    try { single.infer("m", image(), ""); } catch (...) { timed_out = true; }
    release.set_value(); job.get(); expect(timed_out, "pool acquisition did not time out");
}
void codec_test() {
    auto table = table_to_html("<fcel>A&B<lcel><nl><ucel><xcel><nl>");
    expect(table.find("rowspan=\"2\" colspan=\"2\">A&amp;B") != std::string::npos, "OTSL merged cells");
    throws([&] { table_to_html("<fcel>A<nl><xcel><nl>"); });
    Json c = {{"preprocess", {{"width", 2}, {"height", 2}}}, {"inputs", Json::array({{{"name", "image"}, {"source", "image"}}})},
        {"decoder", {{"type", "ctc"}, {"vocabulary", {"", "A", "中"}}}}};
    auto inputs = preprocess(image(), c);
    expect(inputs[0].shape == std::vector<int64_t>({1, 3, 2, 2}) && inputs[0].data[0] == 1, "NCHW preprocessing");
    Tensor out{"logits", {1, 5, 3}, {0,1,0, 0,1,0, 1,0,0, 0,1,0, 0,0,1}};
    expect(decode_tensors({out}, image(), c)["text"] == "AA中", "CTC collapse/blank/UTF8");
    c["decoder"]["vocabulary"] = {"", "A"};
    throws([&] { decode_tensors({out}, image(), c); });
    c["decoder"] = {{"type", "paddle_layout"}, {"labels", {"text"}}, {"coordinates", "input"}};
    auto result = decode_tensors({{"boxes", {1, 6}, {0, .9f, 0, 0, 2, 2}}}, image(), c);
    expect(result["boxes"][0]["coordinate"][2] == 20, "local layout scaling");
    expect(decode_tensors({{"boxes", {0, 6}, {}}}, image(), c)["boxes"].empty(), "empty layout tensor");
}
void pipeline_test() {
    TempDir temp;
    auto png = image().png();
    { std::ofstream f(temp.path / "input.png", std::ios::binary); f.write(reinterpret_cast<const char*>(png.data()), png.size()); }
    auto c = config();
    Pipeline pipeline(c);
    auto doc = pipeline.run(temp.path / "input.png", temp.path / "out");
    expect(doc.pages.size() == 1 && doc.pages[0].regions[0].box.type == "title", "pipeline reading order");
    expect(doc.pages[0].regions[0].model == "shared" && doc.pages[0].regions[1].model == "shared", "shared routing");
    write_outputs(doc, temp.path / "out", "both");
    expect(document_markdown(doc).find("# 中文 test") != std::string::npos, "markdown title");
    std::ifstream result(temp.path / "out/result.json"); Json parsed; result >> parsed;
    expect(parsed["pages"][0]["blocks"].size() == 2, "JSON output");
    throws([&] { pipeline.run(temp.path / "input.png", temp.path / "out"); });
    c["routes"]["title"] = {{"action", "image"}};
    c["routes"].erase("text"); c["execution"]["on_error"] = "record";
    auto partial = Pipeline(c).run(temp.path / "input.png", temp.path / "partial");
    expect(!partial.pages[0].regions[1].error.empty(), "record error policy");
    expect(fs::is_regular_file(temp.path / "partial" / partial.pages[0].regions[0].asset), "saved asset missing");
    c["execution"]["on_error"] = "fail";
    throws([&] { Pipeline(c).run(temp.path / "input.png", temp.path / "fail"); });
    c["models"]["shared"]["instances"] = 0; throws([&] { validate_config(c); });
}
void fallback_test() {
    TempDir temp;
    const auto png = image().png();
    { std::ofstream f(temp.path / "input.png", std::ios::binary);
      f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size())); }
    auto c = config();
    c["models"]["broken"] = {{"backend", "mock"}, {"response", Json::object()}};
    c["routes"]["title"].erase("model");
    c["routes"]["title"]["models"] = {"broken", "shared"};
    validate_config(c);
    const auto doc = Pipeline(c).run(temp.path / "input.png", temp.path / "fallback");
    expect(doc.pages[0].regions[0].model == "shared" &&
           doc.pages[0].regions[0].text == "中文 test", "ordered model fallback failed");
    c["routes"]["title"]["models"] = {"broken"};
    c["execution"]["on_error"] = "record";
    const auto failed = Pipeline(c).run(temp.path / "input.png", temp.path / "all-failed");
    expect(failed.pages[0].regions[0].error.find("broken") != std::string::npos,
           "failed model IDs must be recorded");
    c["routes"]["title"]["models"] = Json::array();
    throws([&] { validate_config(c); });
    c["routes"]["title"]["models"] = {"shared", "shared"};
    throws([&] { validate_config(c); });
    c["routes"]["title"]["models"] = {"missing"};
    throws([&] { validate_config(c); });
    c["routes"]["title"]["model"] = "shared";
    throws([&] { validate_config(c); });
}
void image_process_test() {
    Image i{2, 1, {255,0,0, 0,0,255}};
    auto clipped = i.crop({-1e300, 0, 1e300, 1});
    expect(clipped.width == 2 && clipped.height == 1 && clipped.rgb == i.rgb,
           "finite out-of-range crop must clamp before integer conversion");
    throws([&] { i.crop({std::numeric_limits<double>::quiet_NaN(), 0, 1, 1}); });
    throws([&] { i.crop({2, 0, 1, 1}); });
    throws([&] { i.crop({3, 0, 4, 1}); });
    throws([&] { Image{2, 1, {255}}.crop({0, 0, 1, 1}); });
    auto rotated = i.rotate(90);
    expect(rotated.width == 1 && rotated.height == 2 && rotated.rgb[2] == 255, "CCW rotation");
    expect(base64({0, 1, 2, 3}) == "AAECAw==", "base64 padding");
    expect(run_process({"/usr/bin/printf", "%s", "a; $(echo bad) '中文'"}, 2) == "a; $(echo bad) '中文'", "argv shell injection");
    throws([&] { run_process({"/bin/false"}, 2); });
    auto begin = std::chrono::steady_clock::now();
    throws([&] { run_process({"/bin/sleep", "10"}, 1); });
    expect(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3), "subprocess timeout/reap");
}
int main() {
    try {
        layout_test(); pool_test(); codec_test(); pipeline_test(); fallback_test(); image_process_test();
        std::cout << "PASS: layout, shared pool, timeout/recovery, codecs, pipeline, output, subprocess\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
