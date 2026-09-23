#include "omniocr/tensor.hpp"
#include "omniocr/plugins.hpp"
#include <algorithm>
#include <mutex>
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
    auto wide = parse_layout({{"text", "<|box_start|>0 0 1000 1000<|box_end|><|ref_start|>text<|ref_end|>"}},
        {{"provider", "mineru"}}, 3000000, 1);
    expect(wide.size() == 1 && wide[0].bbox[2] == 3000000 && wide[0].bbox[3] == 1,
           "MinerU wide-image coordinates must not overflow");
    throws([&] { parse_layout({{"text", "wrong model"}}, {{"provider", "mineru"}}, 20, 10); });
    auto bad = c["models"]["layout"]["response"];
    bad["boxes"][0]["bbox"] = {1, 1, 0, 0};
    throws([&] { parse_layout(bad, c["layout"], 20, 10); });
    auto paddle = parse_layout({{"res", {{"boxes", Json::array({{{"label", "text"}, {"coordinate", {-2, 0, 25, 8}}, {"score", .9}}})}}}},
        {{"provider", "paddle"}}, 20, 10);
    expect(paddle[0].bbox[0] == 0 && paddle[0].bbox[2] == 20, "box clipping");
}
void v3_plugin_test() {
    const Json settings={{"provider","paddle.doclayout_v3.http"},
        {"coordinates","pixel"},{"type_map",{{"paragraph_title","heading"}}}};
    const auto raw=Json::parse(R"({
        "res":{"boxes":[
            {"label":"ignored_header","coordinate":[0,0,6,4],"order":null,
             "polygon_points":[[0,0],[6,0],[6,4],[0,4]],"score":0.8},
            {"label":"paragraph_title","coordinate":[5,0,15,9],"order":2,
             "polygon_points":[[5,0],[15,0],[10,9]],"score":0.95},
            {"label":"text","coordinate":[1,1,4,4],"order":0,"score":0.9}
        ]}
    })");
    auto boxes=parse_layout(raw,settings,20,10);
    expect(boxes.size()==3 && boxes[0].type=="text" &&
           boxes[1].type=="heading" && boxes[1].source_index==1 &&
           boxes[1].polygon.size()==3 && boxes[2].source_index==0 &&
           !boxes[2].reading_order, "V3 polygon and null order");
    const auto raw_bad=Json::parse(R"({"boxes":[{"label":"text","polygon_points":[[0,0],[1,1],[2,2]]}]})");
    throws([&] { parse_layout(raw_bad,settings,20,10); });
    Image img{20,10,std::vector<uint8_t>(600,40)};
    auto crop=crop_region(img,boxes[1],{{"cropper","polygon_mask_crop"},{"mask_background",255}});
    expect(crop.width==10 && crop.height==9 &&
           crop.rgb[(size_t(8)*crop.width)*3]==255 && crop.rgb[(size_t(2)*crop.width+5)*3]==40,
           "V3 polygon masking must preserve inside pixels");
    const Json mapping={{"coordinates","model_input"},{"image_size",{10,5}},
        {"transform",{{"matrix",{2,0,-4, 0,2,-6, 0,0,1}}}}};
    const auto context=make_transform_context(mapping,20,10);
    expect(context.to_page(2,3)==std::array<double,2>{0.,0.} &&
           context.to_page(7,4)==std::array<double,2>{10.,2.},
           "request-scoped scale/padding inverse transform");
    const Json model_space={{"provider","paddle.doclayout_v3.http"},
        {"coordinates","model_input"},{"image_size",{10,5}},
        {"transform",{{"matrix",{2,0,-4, 0,2,-6, 0,0,1}}}}};
    auto transformed=parse_layout(Json::parse(R"({"boxes":[{
        "label":"text","coordinate":[2,3,7,4],"order":0,
        "polygon_points":[[2,3],[7,3],[7,4],[2,4]]} ]})"),model_space,20,10);
    expect(transformed.size()==1 && transformed[0].bbox==std::array<double,4>{0.,0.,10.,2.} &&
           transformed[0].polygon[2]==std::array<double,2>{10.,2.},
           "V3 transform maps all geometry to original page");
    throws([&] { make_transform_context({{"coordinates","model_input"},{"image_size",{10,5}},
        {"transform",{{"matrix",{0,0,0,0,0,0,0,0,0}}}}},20,10).to_page(1,1); });
    throws([&] { crop_region(img,boxes[0],{{"cropper","polygon_mask_crop"}}); });
    auto fallback=crop_region(img,boxes[0],{{"cropper","polygon_mask_crop"},{"crop_fallback","bbox_crop"}});
    expect(fallback.width==3, "explicit mask fallback");
    Document document; document.source="fixture";
    Page page; page.number=1; page.width=20; page.height=10;
    for (auto& box:boxes) { Region r; r.box=box; r.text=box.type; page.regions.push_back(r); }
    document.pages.push_back(page);
    auto output=document_json(document);
    const auto forced_legacy=document_json(document,1);
    expect(forced_legacy["schema_version"]==1 &&
           !forced_legacy["pages"][0]["blocks"][1].contains("polygon") &&
           forced_legacy["pages"][0]["blocks"][1]["id"]=="p1-b1",
           "explicit v1 output drops V3 geometry only by caller request");
    expect(output["schema_version"]==2 && output["pages"][0]["blocks"][1]["polygon"].size()==3 &&
           output["pages"][0]["blocks"][2]["reading_order"].is_null() &&
           output["pages"][0]["blocks"][1]["id"]=="p1-s1" &&
           document_markdown(document).find("ignored_header")==std::string::npos,
           "V3 output schema, stable source identity, nullable Markdown order");
    Document legacy; Page legacy_page; legacy_page.number=1;
    Box legacy_box; legacy_box.reading_order=0; legacy_page.regions.push_back({legacy_box});
    legacy.pages.push_back(legacy_page);
    expect(document_json(legacy)["schema_version"]==1,"legacy output schema remains v1");
}
void v2_config_test() {
    Json c={{"version",2},
        {"executors",{
            {"layout_pool",{{"backend","mock"},{"response",{{"boxes",Json::array({
                {{"type","text"},{"bbox",{0,0,1,1}}}
            })}}}}},
            {"ocr_pool",{{"backend","mock"},{"max_inflight",1},{"batch_size",2},
                         {"response",{{"text","v2 recognition"}}}}}
        }},
        {"models",{
            {"page_layout",{{"adapter","normalized"},{"executor","layout_pool"}}},
            {"text_model",{{"adapter","vlm.ovisocr2"},{"executor","ocr_pool"}}},
            {"title_model",{{"adapter","vlm.ovisocr2"},{"executor","ocr_pool"}}}
        }},
        {"pipeline",{{"layout",{{"model","page_layout"},{"coordinates","normalized"}}},
                     {"routes",{{"text",{{"model","text_model"},{"cropper","bbox_crop"}}}}}}}};
    auto runtime=normalize_config(c);
    expect(runtime["version"]==1 && runtime["models"].size()==2 &&
           runtime["models"]["ocr_pool"]["batch_size"]==2 &&
           runtime["layout"]["model"]=="layout_pool" &&
           runtime["routes"]["text"]["model"]=="ocr_pool" &&
           runtime["routes"]["text"]["adapter"]=="vlm.ovisocr2",
           "v2 executor sharing and adapter binding");
    validate_config(c);
    TempDir temp;
    const auto bytes=image().png();
    { std::ofstream out(temp.path/"v2.png",std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); }
    const auto doc=Pipeline(c).run(temp.path/"v2.png",temp.path/"out");
    expect(doc.pages.size()==1 && doc.pages[0].regions.size()==1 &&
           doc.pages[0].regions[0].model=="text_model" &&
           doc.pages[0].regions[0].text=="v2 recognition",
           "v2 bound model uses shared executor while recording binding ID");
    c["models"]["text_model"]["executor"]="missing";
    throws([&] { normalize_config(c); });
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
void dynamic_batch_test() {
    struct State { std::mutex mutex; std::vector<std::vector<std::string>> groups; } state;
    struct Batched : Model {
        State& state;
        explicit Batched(State& s) : state(s) {}
        Json infer(const Image&, const std::string&) override {
            throw std::runtime_error("batch path incorrectly used scalar infer");
        }
        bool supports_batch() const override { return true; }
        std::vector<Json> infer_batch(const std::vector<BatchInput>& inputs) override {
            std::vector<std::string> prompts;
            std::vector<Json> results;
            for (const auto& input : inputs) {
                expect(input.image && input.image->width == 20, "batch image lifetime");
                prompts.push_back(input.prompt);
                results.push_back({{"text", input.prompt}});
            }
            { std::lock_guard<std::mutex> lock(state.mutex); state.groups.push_back(prompts); }
            return results;
        }
    };
    ModelRegistry shared({{"ocr", { {"instances", 1}, {"batch_size", 4},
        {"max_batch_wait_ms", 250}, {"acquire_timeout_ms", 1000}}}},
        [&](const Json&, size_t) { return std::make_unique<Batched>(state); });
    std::promise<void> start;
    auto go = start.get_future().share();
    std::vector<std::future<Json>> jobs;
    for (int i = 0; i < 4; ++i) jobs.push_back(std::async(std::launch::async, [&, i] {
        go.wait();
        auto img = image();
        return shared.infer("ocr", img, "file-" + std::to_string(i));
    }));
    start.set_value();
    for (int i = 0; i < 4; ++i)
        expect(jobs[i].get().at("text") == "file-" + std::to_string(i),
               "batch results reordered across files");
    expect(state.groups.size() == 1 && state.groups[0].size() == 4,
           "requests from different files were not coalesced");
    auto img = image();
    expect(shared.infer("ocr", img, "partial").at("text") == "partial",
           "partial batch did not flush by deadline");
    expect(state.groups.size() == 2 && state.groups[1].size() == 1,
           "partial batch should be one native invocation");
    struct Scalar : Model {
        Json infer(const Image&, const std::string&) override { return Json::object(); }
    };
    throws([&] { ModelRegistry invalid({{"m", {{"batch_size", 2}}}},
        [](const Json&, size_t) { return std::make_unique<Scalar>(); }); });
    struct GateBatch : Model {
        std::promise<void>& started;
        std::shared_future<void> released;
        std::atomic<bool> entered{false};
        GateBatch(std::promise<void>& s, std::shared_future<void> r) : started(s), released(r) {}
        Json infer(const Image&, const std::string&) override { throw std::runtime_error("scalar called"); }
        bool supports_batch() const override { return true; }
        std::vector<Json> infer_batch(const std::vector<BatchInput>& items) override {
            if (!entered.exchange(true)) { started.set_value(); released.wait(); }
            return std::vector<Json>(items.size(), {{"text", "recovered"}});
        }
    };
    std::promise<void> started, release;
    ModelRegistry blocked({{"m", {{"instances", 1}, {"batch_size", 2},
        {"max_batch_wait_ms", 1}, {"acquire_timeout_ms", 30}}}},
        [&](const Json&, size_t) {
            return std::make_unique<GateBatch>(started, release.get_future().share());
        });
    auto running = std::async(std::launch::async, [&] {
        auto owned = image();
        return blocked.infer("m", owned, "first");
    });
    started.get_future().wait();
    throws([&] { blocked.infer("m", img, "timed out while queued"); });
    release.set_value();
    expect(running.get()["text"] == "recovered" &&
           blocked.infer("m", img, "after timeout")["text"] == "recovered",
           "timed out request leaked into the next batch");
    auto invalid_config = config();
    invalid_config["models"]["shared"]["batch_size"] = 2;
    invalid_config["models"]["shared"]["max_batch_wait_ms"] = 1.5;
    throws([&] { validate_config(invalid_config); });
    invalid_config["models"]["shared"]["max_batch_wait_ms"] = 500;
    invalid_config["models"]["shared"]["acquire_timeout_ms"] = 200;
    throws([&] { validate_config(invalid_config); });
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
    c["models"]["shared"]["instances"] = 4294967297ULL; throws([&] { validate_config(c); });
    c["models"]["shared"]["instances"] = 1.8; throws([&] { validate_config(c); });
    c["models"]["shared"]["instances"] = 2;
    c["execution"]["workers"] = 1.5; throws([&] { validate_config(c); });
    c["execution"]["workers"] = 4294967297ULL; throws([&] { validate_config(c); });
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
void table_fallback_test() {
    TempDir temp;
    const auto bytes = image().png();
    { std::ofstream out(temp.path / "table.png", std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
    auto c = config();
    c["models"]["layout"]["response"] = {{"boxes", Json::array({
        {{"type", "table"}, {"bbox", {0, 0, 1, 1}}}
    })}};
    c["models"]["invalid_table"] = {{"backend", "mock"},
        {"response", {{"text", "<fcel>A<nl><xcel><nl>"}}}};
    c["models"]["valid_table"] = {{"backend", "mock"},
        {"response", {{"text", "| item | value |\\n|---|---|\\n| a | b |"}}}};
    c["routes"] = {{"table", {{"models", {"invalid_table", "valid_table"}}}}};
    auto doc = Pipeline(c).run(temp.path / "table.png", temp.path / "recovered");
    auto& region = doc.pages.at(0).regions.at(0);
    expect(region.model == "valid_table" && region.text.find("| a | b |") != std::string::npos &&
        region.raw_text == region.text && region.error.empty(), "F1 table decoder fallback");
    c["routes"]["table"]["models"] = {"invalid_table"};
    c["execution"]["on_error"] = "record";
    doc = Pipeline(c).run(temp.path / "table.png", temp.path / "record");
    expect(doc.pages.at(0).regions.at(0).error.find("invalid_table") != std::string::npos &&
        doc.pages.at(0).regions.at(0).model.empty() &&
        doc.pages.at(0).regions.at(0).text.empty(), "F1 record failure must not commit invalid table");
    c["execution"]["on_error"] = "fail";
    throws([&] { Pipeline(c).run(temp.path / "table.png", temp.path / "fail"); });
}
void batch_test() {
    TempDir temp;
    auto make_image = [&](const char* name, uint8_t pixel) {
        auto source = image();
        std::fill(source.rgb.begin(), source.rgb.end(), pixel);
        auto bytes = source.png();
        std::ofstream out(temp.path / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    };
    make_image("low.png", 20);
    make_image("high.png", 200);
    auto conf = config();
    struct State { std::mutex mutex; std::vector<uint8_t> layout_order; } state;
    struct Instrumented : Model {
        Json answer;
        State& state;
        Instrumented(Json response, State& s) : answer(std::move(response)), state(s) {}
        Json infer(const Image& img, const std::string&) override {
            if (answer.contains("boxes")) {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.layout_order.push_back(img.rgb.front());
            }
            return answer;
        }
    };
    Pipeline pipeline(conf, [&](const Json& model, size_t) {
        return std::make_unique<Instrumented>(model.at("response"), state);
    });
    const std::vector<BatchJob> jobs{
        {temp.path / "low.png", temp.path / "out-low", 1},
        {temp.path / "high.png", temp.path / "out-high", 10},
        {temp.path / "missing.png", temp.path / "out-missing", -5}
    };
    const auto results = pipeline.run_batch(jobs, {1, 1, 1});
    expect(results.size() == 3 && state.layout_order.size() == 2 &&
           state.layout_order[0] == 200 && state.layout_order[1] == 20,
           "batch high-priority document admission");
    expect(results[0].error.empty() && results[1].error.empty() &&
           results[0].document.pages.size() == 1 && results[1].document.pages.size() == 1 &&
           results[0].document.pages[0].number == 1 &&
           results[1].document.pages[0].regions[0].model == "shared",
           "batch must preserve input result order and independent documents");
    expect(results[2].error.find("regular file") != std::string::npos,
           "invalid document must not fail other batch jobs");
    write_outputs(results[0].document, jobs[0].output_dir, "both");
    write_outputs(results[1].document, jobs[1].output_dir, "both");
    expect(fs::exists(jobs[0].output_dir / "result.json") &&
           fs::exists(jobs[1].output_dir / "result.json"), "batch output isolation");
    throws([&] { pipeline.run_batch({
        {temp.path / "low.png", temp.path / "collision", 1},
        {temp.path / "high.png", temp.path / "collision" / "child", 10}});
    });
    throws([&] { pipeline.run_batch(jobs, {129, 1, 1}); });
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
    Image malformed{2, 1, {255}};
    throws([&] { malformed.resize(2, 1); });
    throws([&] { malformed.rotate(90); });
    throws([&] { malformed.rotate(0); });
    throws([&] { malformed.png(); });
    auto rotated = i.rotate(90);
    expect(rotated.width == 1 && rotated.height == 2 && rotated.rgb[2] == 255, "CCW rotation");
    expect(base64({0, 1, 2, 3}) == "AAECAw==", "base64 padding");
    expect(run_process({"/usr/bin/printf", "%s", "a; $(echo bad) '中文'"}, 2) == "a; $(echo bad) '中文'", "argv shell injection");
    throws([&] { run_process({"/bin/false"}, 2); });
    auto begin = std::chrono::steady_clock::now();
    throws([&] { run_process({"/bin/sleep", "10"}, 1); });
    expect(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3), "subprocess timeout/reap");
}
void input_format_test() {
    for (const auto* ext : {".PDF", ".TIFF", ".rtf", ".odt", ".ods", ".odp", ".epub", ".ofd", ".html", ".htm", ".csv"})
        expect(supports_input_extension(ext), "missing input extension");
    expect(!supports_input_extension(".exe") && !supports_input_extension("../csv"), "invalid extension accepted");
    TempDir temp;
    const auto input = temp.path / "cells.csv", output = temp.path / "cells.html";
    auto render = [&](const std::string& content, Json settings = Json::object()) {
        { std::ofstream f(input, std::ios::binary); f << content; }
        csv_to_html(input, output, settings);
        std::ifstream f(output); return std::string(std::istreambuf_iterator<char>(f), {});
    };
    auto html = render("\xef\xbb\xbfID,Value,Note\r\n00123,=1+2,\"a,b\n\"\"quoted\"\" <&>\"\r\n");
    expect(html.find("00123</td>") != std::string::npos && html.find("=1+2</td>") != std::string::npos, "CSV literal values");
    expect(html.find("a,b<br>&quot;quoted&quot; &lt;&amp;&gt;") != std::string::npos, "CSV quotes/newlines/HTML escape");
    expect(render("a;b;", {{"csv_delimiter", ";"}}).find("a</td><td>b</td><td></td>") != std::string::npos, "CSV trailing empty field");
    throws([&] { render("a,\"unterminated"); });
    throws([&] { render("\"a\"x,b"); });
    throws([&] { render("a\"b,c"); });
    throws([&] { render("a,b", {{"max_csv_bytes", 2}}); });
    throws([&] { render(std::string(1, char(0xff))); });
    throws([&] { render(""); });
    auto c = config(); c["document"]["csv_delimiter"] = "::";
    throws([&] { validate_config(c); });
    c["document"] = {{"max_csv_bytes", 1.5}};
    throws([&] { validate_config(c); });
    c["document"] = {{"ofd_converter", ""}};
    throws([&] { validate_config(c); });
}
int main() {
    try {
        input_format_test(); layout_test(); v3_plugin_test(); v2_config_test(); pool_test(); dynamic_batch_test(); codec_test(); pipeline_test(); fallback_test(); table_fallback_test(); batch_test(); image_process_test();
        std::cout << "PASS: layout, shared pool, timeout/recovery, codecs, pipeline, output, subprocess\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
