#include "omniocr/tensor.hpp"
#include "omniocr/plugins.hpp"
#include "../src/backends/http_client.hpp"
#include "../src/visual_buckets.hpp"
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
        {"postprocess",{{"low_score",{{"enabled",true},{"threshold",.25}}}}},
        {"server",{{"port",18080},{"data_dir","service-state"}}},
        {"execution",{{"page_workers",2},{"box_workers",2},{"document_workers",2},{"max_queued_pages",4}}},
        {"executors",{
            {"layout_pool",{{"backend","mock"},{"response",{{"boxes",Json::array({
                {{"type","text"},{"bbox",{0,0,1,1}}}
            })}}}}},
            {"ocr_pool",{{"backend","mock"},{"max_inflight",1},{"max_concurrent_requests",2},{"batch_size",2},
                         {"input_resize",{{"max_width",1280},{"max_pixels",1000000},
                                          {"min_width",28},{"min_pixels",784},{"factor",28}}},
                         {"instance_overrides",Json::array({{{"batch_size",2}},Json::object()})},
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
           runtime["models"]["ocr_pool"]["input_resize"]["max_width"]==1280 &&
           runtime["models"]["ocr_pool"]["input_resize"]["factor"]==28 &&
           runtime["models"]["ocr_pool"]["input_resize"]["min_pixels"]==784 &&
           runtime["models"]["ocr_pool"]["instance_overrides"].size()==2 &&
           runtime["models"]["ocr_pool"]["instances"]==1 &&
           runtime["models"]["ocr_pool"]["max_concurrent_requests"]==2 &&
           runtime["execution"]["box_workers"]==2 && runtime["server"]["port"]==18080 &&
           runtime["layout"]["model"]=="layout_pool" &&
           runtime["routes"]["text"]["model"]=="ocr_pool" &&
           runtime["postprocess"]["low_score"]["threshold"]==.25 &&
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
            if (prompt == "overload") throw HttpStatusError(429);
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
    throws([&] { registry.infer("shared", image(), "overload"); });
    const auto fixed_metrics=registry.scheduler_metrics().at("shared");
    expect(fixed_metrics.at("completed_total")==22 && fixed_metrics.at("failed_total")==8 &&
           fixed_metrics.at("overload_total")==1 && fixed_metrics.at("inflight")==0 &&
           fixed_metrics.at("completed_normalized_work_total")>0,
           "fixed scalar pool must count successes, failures and overloads");
    throws([&] { registry.infer("missing", image(), ""); });
    State expanded;
    ModelRegistry extra({{"ocr", {{"instances", 1}, {"max_concurrent_requests", 4},
                                {"acquire_timeout_ms", 1000}}}},
        [&](const Json&, size_t) { return std::make_unique<Probe>(expanded); });
    std::promise<void> start_extra;
    auto go_extra = start_extra.get_future().share();
    std::vector<std::future<Json>> parallel;
    for (int i = 0; i < 8; ++i) parallel.push_back(std::async(std::launch::async, [&, i] {
        go_extra.wait();
        auto sample = image();
        return extra.infer("ocr", sample, std::to_string(i));
    }));
    start_extra.set_value();
    for (int i = 0; i < 8; ++i)
        expect(parallel[i].get().at("text") == std::to_string(i), "parallel result routing");
    expect(expanded.constructed == 4 && expanded.peak == 4 && expanded.active == 0,
           "max_concurrent_requests did not bound independent model slots");
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
    expect(single.scheduler_metrics()["m"]["inflight"]==1 &&
           single.scheduler_metrics()["m"]["completed_total"]==0,
           "fixed scalar inflight must be visible during inference");
    bool timed_out = false;
    try { single.infer("m", image(), ""); } catch (...) { timed_out = true; }
    release.set_value(); job.get(); expect(timed_out, "pool acquisition did not time out");
    expect(single.scheduler_metrics()["m"]["completed_total"]==1 &&
           single.scheduler_metrics()["m"]["failed_total"]==0 &&
           single.scheduler_metrics()["m"]["inflight"]==0 &&
           single.scheduler_metrics()["m"]["acquisition_timeout_total"]==1,
           "queued timeout must not count as backend failure");
}
void model_input_resize_test() {
    struct Dimensions : Model {
        Json infer(const Image& image,const std::string&) override {
            return {{"width",image.width},{"height",image.height},{"text","ok"}};
        }
        bool supports_batch() const override {return true;}
        std::vector<Json> infer_batch(const std::vector<BatchInput>& inputs) override {
            std::vector<Json> results;
            for (const auto& input:inputs) results.push_back(infer(*input.image,input.prompt));
            return results;
        }
    };
    Image large{100,60,std::vector<uint8_t>(100*60*3,255)};
    Json models={{"a",{{"input_resize",{{"max_width",40},{"max_height",40},{"max_pixels",1000}}}}},
                 {"b",{{"input_resize",{{"max_width",30}}}}}};
    ModelRegistry registry(models,[](const Json&,size_t){return std::make_unique<Dimensions>();});
    expect(registry.input_size("a",large)==std::array<int,2>{40,24} &&
           registry.infer("a",large,"")["width"]==40 &&
           registry.infer("b",large,"")["width"]==30 &&
           registry.infer("a",image(),"")["width"]==20,
           "model-specific resize must preserve aspect ratio and avoid upscaling");
    models["smart"]={{"input_resize",{{"factor",28},{"min_pixels",28*28*130},
                                       {"max_pixels",28*28*1280}}}};
    ModelRegistry smart(models,[](const Json&,size_t){return std::make_unique<Dimensions>();});
    const auto enlarged=smart.input_size("smart",image());
    expect(enlarged[0]%28==0 && enlarged[1]%28==0 &&
           enlarged[0]*enlarged[1]>=28*28*130 &&
           enlarged[0]*enlarged[1]<=28*28*1280 &&
           smart.infer("smart",image(),"")["width"]==enlarged[0],
           "smart resize must upscale small inputs and align dimensions to factor");
    Image narrow{10,2000,{}};
    expect(smart.input_size("smart",narrow)==std::array<int,2>{28,5600},
           "a sub-factor short edge must expand with its long edge before pixel constraints");
    models["exact"]={{"input_resize",{{"factor",28},{"min_pixels",28*28*2},
                                       {"max_pixels",28*28*2}}}};
    ModelRegistry exact(models,[](const Json&,size_t){return std::make_unique<Dimensions>();});
    expect(exact.input_size("exact",large)==std::array<int,2>{56,28},
           "pixel interval must hold even when factor quantization crosses a bound");
    models["constraints"]={{"input_resize",{{"factor",10},{"min_width",50},{"max_width",70},
                                             {"min_height",20},{"max_height",30},
                                             {"min_pixels",1600},{"max_pixels",2100}}}};
    ModelRegistry constrained(models,[](const Json&,size_t){return std::make_unique<Dimensions>();});
    expect(constrained.input_size("constraints",large)==std::array<int,2>{60,30},
           "all six optional bounds must apply to the final aligned size");
    models["a"]["batch_size"]=2;
    models["a"]["max_batch_wait_ms"]=1;
    ModelRegistry batched(models,[](const Json&,size_t){return std::make_unique<Dimensions>();});
    auto first=std::async(std::launch::async,[&]{return batched.infer("a",large,"");});
    auto second=std::async(std::launch::async,[&]{return batched.infer("a",large,"");});
    expect(first.get()["height"]==24 && second.get()["height"]==24,
           "queued batch inputs must own resized images until inference completes");

    TempDir temp;
    const auto png=large.png();
    {std::ofstream out(temp.path/"input.png",std::ios::binary);
     out.write(reinterpret_cast<const char*>(png.data()),std::streamsize(png.size()));}
    auto c=config();
    c["layout"]["coordinates"]="pixel";
    c["models"]["layout"]["input_resize"]={{"max_width",50}};
    c["models"]["layout"]["response"]={{"boxes",Json::array({
        {{"type","text"},{"bbox",{10,5,40,25}}}})}};
    c["models"]["shared"]["input_resize"]={{"max_width",20},{"max_pixels",200}};
    struct LayoutFixture : Model {
        Json response;std::atomic<int>& layout_width;std::atomic<int>& ocr_width;
        LayoutFixture(Json r,std::atomic<int>& l,std::atomic<int>& o):
            response(std::move(r)),layout_width(l),ocr_width(o){}
        Json infer(const Image& image,const std::string&) override {
            if (response.contains("boxes")) {layout_width=image.width;return response;}
            ocr_width=image.width;return {{"text","ok"}};
        }
    };
    std::atomic<int> layout_width{0},ocr_width{0};
    auto run=[&](const Json& settings,const char* out) {
        return Pipeline(settings,[&](const Json& model,size_t) {
            return std::make_unique<LayoutFixture>(model.at("response"),layout_width,ocr_width);
        }).run(temp.path/"input.png",temp.path/out);
    };
    validate_config(c);
    auto doc=run(c,"pixel");
    expect(layout_width==50 && ocr_width==17 && doc.pages[0].regions.size()==1 &&
           doc.pages[0].regions[0].box.bbox==std::array<double,4>{20,10,80,50},
           "pixel-space layout geometry must map from model input to original page");
    c["models"]["layout"]["input_resize"]={{"min_width",200}};
    c["models"]["layout"]["response"]={{"boxes",Json::array({
        {{"type","text"},{"bbox",{40,20,160,100}}}})}};
    validate_config(c);
    doc=run(c,"pixel-upscale");
    expect(layout_width==200 && doc.pages[0].regions[0].box.bbox==std::array<double,4>{20,10,80,50},
           "pixel-space layout boxes must map back after an upscale");
    c["models"]["layout"]["input_resize"]={{"max_width",50}};
    c["layout"]={{"provider","paddle.doclayout_v3.http"},{"adapter","paddle.doclayout_v3.http"},
        {"model","layout"},{"coordinates","model_input"},{"image_size",{100,60}}};
    c["models"]["layout"]["response"]={{"boxes",Json::array({
        {{"label","text"},{"coordinate",{10,5,40,25}},
         {"polygon_points",{{9,4},{41,4},{41,26},{9,26}}}}})}};
    validate_config(c);
    doc=run(c,"model_input");
    const auto& box=doc.pages[0].regions[0].box;
    expect(box.bbox==std::array<double,4>{20,10,80,50} &&
           box.polygon[0]==std::array<double,2>{18,8} &&
           box.crop_bbox==std::optional<std::array<double,4>>({18,8,82,52}),
           "V3 model_input geometry and crop metadata must use resized dimensions");
    c["layout"]["transform"]={{"matrix",{2,0,0,0,2,0,0,0,1}}};
    throws([&]{validate_config(c);});
    c["layout"].erase("transform");
    c["models"]["shared"]["input_resize"]["max_pixels"]=0;
    throws([&]{validate_config(c);});
    c["models"]["shared"]["input_resize"]["max_pixels"]=200;
    c["models"]["shared"]["input_resize"]["unknown"]=1;
    throws([&]{validate_config(c);});
    c["models"]["shared"]["input_resize"].erase("unknown");
    c["models"]["shared"]["input_resize"]["min_pixels"]=201;
    throws([&]{validate_config(c);});
    c["models"]["shared"]["input_resize"].erase("min_pixels");
    c["models"]["shared"]["input_resize"]["factor"]=32;
    c["models"]["shared"]["input_resize"]["max_width"]=20;
    throws([&]{validate_config(c);});
}
void cancellation_pool_test() {
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    struct Blocking : Model {
        std::promise<void>& entered;
        std::shared_future<void> gate;
        std::atomic<bool> first{true};
        Blocking(std::promise<void>& e, std::shared_future<void> g) : entered(e), gate(g) {}
        Json infer(const Image&, const std::string& prompt) override {
            if (first.exchange(false)) { entered.set_value(); gate.wait(); }
            return {{"text", prompt}};
        }
    };
    ModelRegistry registry({{"m", {{"instances", 1}, {"acquire_timeout_ms", 3000}}}},
        [&](const Json&, size_t) { return std::make_unique<Blocking>(entered, gate); });
    auto active = std::async(std::launch::async, [&] {
        auto sample = image(); return registry.infer("m", sample, "first");
    });
    entered.get_future().wait();
    auto token = std::make_shared<std::atomic<bool>>(false);
    auto waiting = std::async(std::launch::async, [&] {
        CancellationScope scope(token.get());
        auto sample = image();
        try { registry.infer("m", sample, "cancelled"); }
        catch (const Cancelled&) { return true; }
        return false;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (registry.scheduler_metrics()["m"]["queued"] != 1 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    token->store(true);
    const bool woke = waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    release.set_value();
    expect(woke && waiting.get() && active.get()["text"] == "first" &&
           registry.scheduler_metrics()["m"]["queued"] == 0,
           "cancelled model waiter retained its slot or BOX input");
    auto sample = image();
    expect(registry.infer("m", sample, "recovered")["text"] == "recovered",
           "model lease unavailable after cancellation");
}
void visual_bucket_scheduler_test() {
    std::vector<detail::VisualCandidate> skewed;
    for (size_t visual = 1; visual <= 16; ++visual)
        skewed.push_back({visual, visual, 20 + visual});
    const auto dense = detail::partition_visual_buckets(skewed, {1, 2, 4, 8}, 8, 320);
    expect(dense.capture_size == 8 && dense.groups.size() == 2 &&
           dense.groups[0].front().visual == 1 && dense.groups[0].back().visual == 8 &&
           dense.groups[1].front().visual == 9 && dense.groups[1].back().visual == 16,
           "skewed visual costs were not split into capture-sized quantile buckets");
    const auto tight = detail::partition_visual_buckets(skewed, {1, 2, 4, 8}, 8, 110);
    expect(tight.capture_size == 4 && tight.groups.size() == 5 &&
           tight.groups[2].size() == 4 && tight.groups[2].front().visual == 6 &&
           tight.groups[2].back().visual == 9,
           "prefill limit did not refine adaptive bucket granularity");
    std::vector<detail::VisualCandidate> offset;
    for (size_t visual = 1; visual <= 9; ++visual)
        offset.push_back({visual, visual, visual == 1 ? 80u : 10u});
    const auto aligned = detail::partition_visual_buckets(offset, {1, 2, 4}, 4, 40);
    expect(aligned.capture_size == 4 && aligned.groups.size() == 3 &&
           aligned.groups[0].size() == 1 && aligned.groups[1].size() == 4 &&
           aligned.groups[1].front().visual == 2 && aligned.groups[1].back().visual == 5,
           "adaptive buckets missed a feasible capture-sized window between quantile cuts");
    const auto singleton = detail::partition_visual_buckets({{0, 10, 1000}, {1, 20, 1000}},
        {1, 2}, 2, 100);
    expect(singleton.capture_size == 1 && singleton.groups.size() == 2,
           "oversized requests should be admitted singly");
    auto settings = config();
    settings["models"]["shared"] = {{"backend", "vllm"},
        {"endpoint", "http://127.0.0.1:1/v1/chat/completions"}, {"model", "fixture"},
        {"instances", 1}, {"max_concurrent_requests", 4}, {"acquire_timeout_ms", 5000},
        {"vllm_visual_scheduler", {{"enabled", true}, {"max_num_seqs", 2},
            {"max_model_len", 1000}, {"max_num_batched_tokens", 300},
            {"cudagraph_capture_sizes", Json::array({1, 2})},
            {"visual_pixels_per_token", 10}, {"visual_token_overhead", 2},
            {"max_visual_tokens", 500}, {"prompt_token_overhead", 3},
            {"expected_output_tokens", 10}, {"bucket_edges", Json::array({10, 50})},
            {"max_wait_ms", 2}}}};
    validate_config(settings);
    struct State { std::atomic<int> active{0}, peak{0}; } state;
    struct Probe : Model {
        State& state;
        explicit Probe(State& s) : state(s) {}
        Json infer(const Image&, const std::string& prompt) override {
            const int active = ++state.active;
            int previous = state.peak;
            while (previous < active && !state.peak.compare_exchange_weak(previous, active)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            --state.active;
            return {{"text", prompt}};
        }
    };
    ModelRegistry registry(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<Probe>(state);
    });
    std::vector<std::future<Json>> results;
    for (int i = 0; i < 18; ++i)
        results.push_back(std::async(std::launch::async, [&, i] {
            const int edge = i % 3 == 0 ? 5 : i % 3 == 1 ? 15 : 30;
            Image sample{edge, edge, {}};
            return registry.infer("shared", sample, std::to_string(i));
        }));
    for (int i = 0; i < 18; ++i)
        expect(results[i].get().at("text") == std::to_string(i), "visual wave misrouted result");
    const auto metrics = registry.scheduler_metrics().at("shared");
    uint64_t dynamic_dispatched = 0;
    for (const auto& count : metrics.at("visual_dynamic_bucket_dispatched"))
        dynamic_dispatched += count.get<uint64_t>();
    expect(state.peak == 2 && metrics.at("strategy") == "visual_bucket" &&
           metrics.at("visual_bucket_mode") == "adaptive" &&
           metrics.at("completed_total") == 18 && metrics.at("inflight") == 0 &&
           metrics.at("visual_wave_requests_total") == 18 &&
           metrics.at("visual_waves_total") > 0 &&
           metrics.at("visual_dynamic_splits_total") > 0 &&
           dynamic_dispatched == 18 &&
           metrics.at("visual_selected_capture_size") >= 1 &&
           metrics.at("visual_bucket_dispatched")[0] == 6 &&
           metrics.at("visual_bucket_dispatched")[1] == 6 &&
           metrics.at("visual_bucket_dispatched")[2] == 6,
           "visual scheduler concurrency, bucket counts or metrics incorrect");
    Image huge{100, 100, {}};
    expect(registry.infer("shared", huge, "oversized")["text"] == "oversized",
           "oversized prefill should run alone when max_model_len permits chunking");
    Image length{300, 300, {}};
    settings["models"]["shared"]["vllm_visual_scheduler"]["max_visual_tokens"] = 2000;
    ModelRegistry bounded(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<Probe>(state);
    });
    throws([&] { bounded.infer("shared", length, "too long"); });
    auto bad = settings;
    bad["models"]["shared"]["vllm_visual_scheduler"]["cudagraph_capture_sizes"] = Json::array({2, 1});
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["adaptive_concurrency"] = {{"enabled", true}};
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["vllm_visual_scheduler"]["max_num_seqs"] = 0;
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["vllm_visual_scheduler"]["bucket_mode"] = "unknown";
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["vllm_visual_scheduler"]["bucket_scan_limit"] = 0;
    throws([&] { validate_config(bad); });
    auto no_edges = settings;
    no_edges["models"]["shared"]["vllm_visual_scheduler"].erase("bucket_edges");
    validate_config(no_edges);
    no_edges["models"]["shared"]["vllm_visual_scheduler"]["bucket_mode"] = "static";
    throws([&] { validate_config(no_edges); });
    no_edges["models"]["shared"]["vllm_visual_scheduler"]["bucket_edges"] = Json::array({10, 50});
    validate_config(no_edges);
    ModelRegistry legacy(no_edges.at("models"), [&](const Json&, size_t) {
        return std::make_unique<Probe>(state);
    });
    Image legacy_image{10, 10, {}};
    expect(legacy.infer("shared", legacy_image, "legacy")["text"] == "legacy" &&
           legacy.scheduler_metrics()["shared"]["visual_bucket_mode"] == "static" &&
           legacy.scheduler_metrics()["shared"]["visual_dynamic_splits_total"] == 0,
           "static visual bucket configuration regressed");
    settings["models"]["shared"]["vllm_visual_scheduler"]["max_num_seqs"] = 1;
    settings["models"]["shared"]["vllm_visual_scheduler"]["cudagraph_capture_sizes"] = Json::array({1});
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    struct Blocking : Model {
        std::promise<void>& entered;
        std::shared_future<void> gate;
        Blocking(std::promise<void>& e, std::shared_future<void> g) : entered(e), gate(g) {}
        Json infer(const Image&, const std::string& prompt) override {
            if (prompt == "first") { entered.set_value(); gate.wait(); }
            return {{"text", prompt}};
        }
    };
    ModelRegistry cancel_pool(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<Blocking>(entered, gate);
    });
    auto first = std::async(std::launch::async, [&] {
        Image sample{10, 10, {}}; return cancel_pool.infer("shared", sample, "first");
    });
    entered.get_future().wait();
    auto token = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = std::async(std::launch::async, [&] {
        CancellationScope scope(token.get());
        Image sample{10, 10, {}};
        try { cancel_pool.infer("shared", sample, "cancelled"); }
        catch (const Cancelled&) { return true; }
        return false;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (cancel_pool.scheduler_metrics()["shared"]["queued"] != 1 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    token->store(true);
    expect(cancelled.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
           cancelled.get(), "visual scheduler failed to cancel queued request");
    release.set_value();
    expect(first.get()["text"] == "first", "visual scheduler blocked active inference");
    Image next{10, 10, {}};
    expect(cancel_pool.infer("shared", next, "recovered")["text"] == "recovered" &&
           cancel_pool.scheduler_metrics()["shared"]["queued"] == 0,
           "visual scheduler retained cancelled ticket");
}
void adaptive_concurrency_test() {
    auto settings = config();
    settings["models"]["shared"] = {{"backend", "vllm"}, {"endpoint", "http://127.0.0.1:1/v1/chat/completions"},
        {"model", "fixture"}, {"instances", 1}, {"max_concurrent_requests", 4},
        {"acquire_timeout_ms", 5000}, {"adaptive_concurrency", {
            {"enabled", true}, {"min_concurrency", 1}, {"initial_concurrency", 1},
            {"window_ms", 100}, {"min_samples", 2}, {"latency_target_ms", 1000},
            {"token_budget", 10000}, {"image_pixels_per_token", 1000},
            {"expected_output_tokens", 100}}}};
    validate_config(settings);
    struct State { std::atomic<int> active{0}, peak{0}; } state;
    struct Slow : Model {
        State& state;
        explicit Slow(State& s) : state(s) {}
        Json infer(const Image&, const std::string& prompt) override {
            const int now = ++state.active;
            int old = state.peak;
            while (old < now && !state.peak.compare_exchange_weak(old, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            --state.active;
            return {{"text", prompt}};
        }
    };
    auto burst = [&](ModelRegistry& pool, int count) {
        std::vector<std::future<Json>> jobs;
        for (int i = 0; i < count; ++i) jobs.push_back(std::async(std::launch::async, [&, i] {
            auto sample = image();
            return pool.infer("shared", sample, std::to_string(i));
        }));
        for (int i = 0; i < count; ++i)
            expect(jobs[i].get().at("text") == std::to_string(i), "adaptive result routing");
    };
    ModelRegistry adaptive(settings.at("models"), [&](const Json&, size_t) { return std::make_unique<Slow>(state); });
    burst(adaptive, 20);
    const auto metrics = adaptive.scheduler_metrics().at("shared");
    expect(state.peak >= 2 && state.peak <= 4 && metrics.at("concurrency_limit") >= 2 &&
           metrics.at("completed_total") == 20 && metrics.at("inflight") == 0,
           "adaptive concurrency failed to increase under sustained demand");
    settings["models"]["shared"]["adaptive_concurrency"]["initial_concurrency"] = 4;
    settings["models"]["shared"]["adaptive_concurrency"]["token_budget"] = 200;
    State budget_state;
    ModelRegistry budget(settings.at("models"), [&](const Json&, size_t) { return std::make_unique<Slow>(budget_state); });
    burst(budget, 8);
    expect(budget_state.peak == 1 && budget.scheduler_metrics()["shared"]["inflight_estimated_tokens"] == 0,
           "estimated token budget did not limit simultaneous requests");
    settings["models"]["shared"]["max_concurrent_requests"] = 3;
    auto& scheduling = settings["models"]["shared"]["adaptive_concurrency"];
    scheduling["initial_concurrency"] = 2;
    scheduling["expected_output_tokens"] = 1;
    scheduling["token_budget"] = 150;
    scheduling["initial_token_budget"] = 150;
    std::promise<void> first_started, release_first, small_started;
    auto release_gate = release_first.get_future().share();
    struct SizeProbe : Model {
        std::promise<void>& first, &small;
        std::shared_future<void> release;
        SizeProbe(std::promise<void>& a, std::promise<void>& b, std::shared_future<void> r)
            : first(a), small(b), release(r) {}
        Json infer(const Image&, const std::string& prompt) override {
            if (prompt == "first") { first.set_value(); release.wait(); }
            if (prompt == "small") small.set_value();
            return {{"text", prompt}};
        }
    };
    ModelRegistry mixed(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<SizeProbe>(first_started, small_started, release_gate);
    });
    auto first = std::async(std::launch::async, [&] { auto sample = image(); return mixed.infer("shared", sample, "first"); });
    first_started.get_future().wait();
    auto large = std::async(std::launch::async, [&] {
        Image sample{400, 400, {}};
        return mixed.infer("shared", sample, "large");
    });
    const auto queue_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (mixed.scheduler_metrics()["shared"]["queued"] != 1 &&
           std::chrono::steady_clock::now() < queue_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(mixed.scheduler_metrics()["shared"]["queued"] == 1, "large request was not queued");
    auto small = std::async(std::launch::async, [&] { auto sample = image(); return mixed.infer("shared", sample, "small"); });
    const bool bypassed = small_started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release_first.set_value();
    expect(bypassed && small.get()["text"] == "small" && first.get()["text"] == "first" &&
           large.get()["text"] == "large", "small BOX could not bypass token-blocked large BOX");
    expect(mixed.scheduler_metrics()["shared"]["completed_normalized_work_total"] == 363,
           "normalized work should count BOX area without mutable token calibration");
    settings["models"]["shared"]["adaptive_concurrency"]["token_budget"] = 10000;
    scheduling.erase("initial_token_budget");
    settings["models"]["shared"]["adaptive_concurrency"]["initial_concurrency"] = 3;
    struct Overload : Model {
        Json infer(const Image&, const std::string&) override { throw HttpStatusError(429); }
    };
    ModelRegistry overloaded(settings.at("models"), [](const Json&, size_t) { return std::make_unique<Overload>(); });
    throws([&] { overloaded.infer("shared", image(), ""); });
    expect(overloaded.scheduler_metrics()["shared"]["current_token_budget"] < 10000 &&
           overloaded.scheduler_metrics()["shared"]["overload_total"] == 1 &&
           overloaded.scheduler_metrics()["shared"]["cooldown_remaining_ms"] > 0,
           "HTTP overload should reduce the adaptive token budget");
    settings["models"]["shared"]["max_concurrent_requests"] = 4;
    scheduling["initial_concurrency"] = 4;
    scheduling["cooldown_ms"] = 200;
    struct SilentStall : Model {
        std::atomic<int>& calls;
        explicit SilentStall(std::atomic<int>& c) : calls(c) {}
        Json infer(const Image&, const std::string& prompt) override {
            const int index = calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(index < 16 ? 15 : 160));
            return {{"text", prompt}};
        }
    };
    std::atomic<int> calls{0};
    ModelRegistry stall(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<SilentStall>(calls);
    });
    burst(stall, 80);
    expect(stall.scheduler_metrics()["shared"]["window_mean_latency_ms"] >= 100 &&
           stall.scheduler_metrics()["shared"]["overload_total"] == 0 &&
           stall.scheduler_metrics()["shared"]["current_token_budget"] > 5000,
           "backend slowdown should not trigger a latency-only half backoff");
    scheduling["expected_output_tokens"] = 1;
    scheduling["token_budget"] = 2000;
    scheduling["min_token_budget"] = 70;
    scheduling["initial_token_budget"] = 70;
    settings["models"]["shared"]["max_concurrent_requests"] = 8;
    scheduling["initial_concurrency"] = 1;
    struct GrowingLatency : Model {
        std::atomic<int>& active;
        explicit GrowingLatency(std::atomic<int>& a) : active(a) {}
        Json infer(const Image&, const std::string& prompt) override {
            const int concurrent = ++active;
            std::this_thread::sleep_for(std::chrono::milliseconds(35 + concurrent * 3));
            --active;
            return {{"text", prompt}};
        }
    };
    std::atomic<int> growing_active{0};
    ModelRegistry increasing(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<GrowingLatency>(growing_active);
    });
    burst(increasing, 80);
    expect(increasing.scheduler_metrics()["shared"]["current_token_budget"] > 70 &&
           increasing.scheduler_metrics()["shared"]["window_normalized_work_per_second"] > 0,
           "throughput controller failed to grow despite higher latency and useful work gain");
    scheduling["initial_token_budget"] = 350;
    scheduling["min_token_budget"] = 100;
    struct Congested : Model {
        std::atomic<int>& active;
        explicit Congested(std::atomic<int>& a) : active(a) {}
        Json infer(const Image&, const std::string& prompt) override {
            const int concurrent = ++active;
            std::this_thread::sleep_for(std::chrono::milliseconds(5 + concurrent * concurrent * 4));
            --active;
            return {{"text", prompt}};
        }
    };
    std::atomic<int> congested_active{0};
    ModelRegistry congestion(settings.at("models"), [&](const Json&, size_t) {
        return std::make_unique<Congested>(congested_active);
    });
    burst(congestion, 80);
    expect(congestion.scheduler_metrics()["shared"]["budget_pressure_total"] > 0 &&
           congestion.scheduler_metrics()["shared"]["current_token_budget"] < 2000,
           "controller kept expanding token budget after normalized throughput saturated");
    validate_config(settings);
    auto bad = settings;
    bad["models"]["shared"]["adaptive_concurrency"]["backoff_ratio"] = 1.0;
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["adaptive_concurrency"]["initial_token_budget"] = 20000;
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["adaptive_concurrency"]["token_budget"] = 0;
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["adaptive_concurrency"]["initial_concurrency"] = 9;
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["batch_size"] = 2;
    throws([&] { validate_config(bad); });
    bad = settings;
    bad["models"]["shared"]["backend"] = "mock";
    throws([&] { validate_config(bad); });
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
    const auto batch_metrics=shared.scheduler_metrics().at("ocr");
    expect(batch_metrics.at("completed_total")==5 && batch_metrics.at("failed_total")==0 &&
           batch_metrics.at("inflight")==0 &&
           batch_metrics.at("completed_normalized_work_total")>0,
           "fixed native batch must count each completed BOX");
    struct FailBatch : Model {
        Json infer(const Image&, const std::string&) override { throw std::runtime_error("scalar called"); }
        bool supports_batch() const override { return true; }
        std::vector<Json> infer_batch(const std::vector<BatchInput>&) override { throw HttpStatusError(503); }
    };
    ModelRegistry failing({{"ocr",{{"batch_size",2},{"max_batch_wait_ms",1}}}},
        [](const Json&,size_t){return std::make_unique<FailBatch>();});
    throws([&]{failing.infer("ocr",img,"bad");});
    const auto failed_batch=failing.scheduler_metrics().at("ocr");
    expect(failed_batch.at("completed_total")==1 && failed_batch.at("failed_total")==1 &&
           failed_batch.at("overload_total")==1 && failed_batch.at("inflight")==0,
           "fixed native batch must count failed BOXes and backend overload");
    // Each instance takes work from the same FIFO, even when batch profiles
    // differ. Verify native batch sizing, instance affinity and result routing.
    std::promise<void> scalar_started, release_scalar, batch_started;
    auto scalar_released = release_scalar.get_future().share();
    auto scalar_seen = scalar_started.get_future();
    auto batch_seen = batch_started.get_future();
    std::atomic<bool> scalar_entered{false}, batch_entered{false};
    struct SlotBatch : Model {
        size_t slot;
        int configured_batch;
        std::atomic<int>& active;
        std::atomic<int>& peak;
        std::promise<void>& scalar_started;
        std::shared_future<void> scalar_released;
        std::promise<void>& batch_started;
        std::atomic<bool>& scalar_entered;
        std::atomic<bool>& batch_entered;
        SlotBatch(size_t i, int b, std::atomic<int>& a, std::atomic<int>& p,
                  std::promise<void>& s, std::shared_future<void> r, std::promise<void>& t,
                  std::atomic<bool>& se, std::atomic<bool>& be)
            : slot(i), configured_batch(b), active(a), peak(p), scalar_started(s),
              scalar_released(r), batch_started(t), scalar_entered(se), batch_entered(be) {}
        Json infer(const Image&, const std::string& prompt) override {
            expect(configured_batch == 1, "scalar slot did not receive its own batch configuration");
            // Hold the first scalar call so a fast mock cannot drain the whole
            // queue before the native batch worker gets scheduled on CI.
            if (!scalar_entered.exchange(true)) { scalar_started.set_value(); scalar_released.wait(); }
            return {{"text", prompt}, {"slot", slot}};
        }
        bool supports_batch() const override { return true; }
        std::vector<Json> infer_batch(const std::vector<BatchInput>& inputs) override {
            expect(configured_batch == 2 && inputs.size() <= 2,
                   "batch slot exceeded its configured batch size");
            if (!batch_entered.exchange(true)) batch_started.set_value();
            const int current = ++active;
            int old = peak.load();
            while (old < current && !peak.compare_exchange_weak(old, current)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            --active;
            std::vector<Json> out;
            for (const auto& input : inputs) out.push_back({{"text", input.prompt}, {"slot", slot}});
            return out;
        }
    };
    std::atomic<int> active{0}, peak{0};
    ModelRegistry profiled({{"ocr", {{"instances", 1}, {"max_concurrent_requests", 2},
        {"batch_size", 2}, {"max_batch_wait_ms", 5},
        {"instance_overrides", Json::array({{{"batch_size", 1}}, {{"batch_size", 2}}})}}}},
        [&](const Json& settings, size_t slot) {
            return std::make_unique<SlotBatch>(slot, settings.at("batch_size").get<int>(), active, peak,
                                               scalar_started, scalar_released, batch_started,
                                               scalar_entered, batch_entered);
        });
    std::vector<std::future<Json>> routed;
    for (int i = 0; i < 16; ++i) routed.push_back(std::async(std::launch::async, [&, i] {
        auto img = image();
        return profiled.infer("ocr", img, "document-" + std::to_string(i));
    }));
    const bool both_active = scalar_seen.wait_for(std::chrono::seconds(3)) == std::future_status::ready &&
                             batch_seen.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    release_scalar.set_value();
    expect(both_active, "global queue did not dispatch to both instances under contention");
    int batch_slot = 0, scalar_slot = 0;
    for (int i = 0; i < 16; ++i) {
        const auto value = routed[i].get();
        expect(value.at("text") == "document-" + std::to_string(i), "instance result routed to wrong BOX");
        (value.at("slot") == 0 ? scalar_slot : batch_slot)++;
    }
    expect(scalar_slot > 0 && batch_slot > 0, "global queue did not balance across instances");
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
    const auto recovered=blocked.scheduler_metrics().at("m");
    expect(recovered.at("completed_total")==2 && recovered.at("failed_total")==0 &&
           recovered.at("inflight")==0 && recovered.at("acquisition_timeout_total")==1,
           "native batch timeout must remain separate from backend completion counts");
    auto invalid_config = config();
    invalid_config["models"]["shared"]["max_concurrent_requests"] = 0;
    throws([&] { validate_config(invalid_config); });
    invalid_config["models"]["shared"]["max_concurrent_requests"] = 129;
    throws([&] { validate_config(invalid_config); });
    invalid_config["models"]["shared"].erase("max_concurrent_requests");
    invalid_config["models"]["shared"]["batch_size"] = 2;
    invalid_config["models"]["shared"]["max_batch_wait_ms"] = 1.5;
    throws([&] { validate_config(invalid_config); });
    invalid_config["models"]["shared"]["max_batch_wait_ms"] = 500;
    invalid_config["models"]["shared"]["acquire_timeout_ms"] = 200;
    throws([&] { validate_config(invalid_config); });
    invalid_config = config();
    invalid_config["models"]["shared"]["instance_overrides"] = Json::array({{{"batch_size", 2}}});
    invalid_config["models"]["shared"]["max_pending_requests"] = 1;
    throws([&] { validate_config(invalid_config); });
    invalid_config["models"]["shared"]["max_pending_requests"] = 3;
    invalid_config["models"]["shared"]["instance_overrides"] = Json::array({{{"batch_size", 2}, {"max_batch_wait_ms", 1.5}}});
    throws([&] { validate_config(invalid_config); });
    invalid_config["models"]["shared"]["instance_overrides"] = Json::array({Json::object(), Json::object(), Json::object()});
    throws([&] { validate_config(invalid_config); });
    auto acl_config = config();
    acl_config["models"]["shared"] = {{"backend", "acl"}, {"path", "fake.om"},
        {"preprocess", {{"width", 20}, {"height", 10}}},
        {"inputs", Json::array({{{"name", "input"}, {"source", "image"}}})},
        {"decoder", {{"type", "ctc"}}}, {"acl_async_stream", true}};
    validate_config(acl_config);
    acl_config["models"]["shared"]["acl_async_stream"] = "true";
    throws([&] { validate_config(acl_config); });
}
void box_pool_fairness_test() {
    std::promise<void> entered, release, second_layout;
    auto gate = release.get_future().share();
    std::mutex mutex;
    std::vector<int> visited;
    std::atomic<bool> first_seen{false};
    struct Fixture : Model {
        bool layout;
        std::promise<void>& entered, &second_layout;
        std::shared_future<void> gate;
        std::mutex& mutex;
        std::vector<int>& visited;
        std::atomic<bool>& first_seen;
        Fixture(bool l, std::promise<void>& e, std::promise<void>& s, std::shared_future<void> g,
                std::mutex& m, std::vector<int>& v, std::atomic<bool>& f)
            : layout(l), entered(e), second_layout(s), gate(g), mutex(m), visited(v), first_seen(f) {}
        Json infer(const Image& img, const std::string&) override {
            const int value = img.rgb.front();
            if (layout) {
                Json boxes = Json::array();
                const int count = value == 1 ? 8 : 1;
                for (int i = 0; i < count; ++i)
                    boxes.push_back({{"type", "text"}, {"bbox", {0, double(i) / count, 1, double(i + 1) / count}}});
                if (value == 2) second_layout.set_value();
                return {{"boxes", boxes}};
            }
            if (value == 1 && !first_seen.exchange(true)) entered.set_value();
            if (value == 1) gate.wait();
            { std::lock_guard<std::mutex> lock(mutex); visited.push_back(value); }
            return {{"text", std::to_string(value)}};
        }
    };
    auto c = config();
    Pipeline pipeline(c, [&](const Json& settings, size_t) {
        return std::make_unique<Fixture>(settings.contains("response") &&
            settings["response"].contains("boxes"), entered, second_layout, gate, mutex, visited, first_seen);
    });
    TempDir output;
    Image a{16,16,std::vector<uint8_t>(16*16*3,1)};
    Image b{16,16,std::vector<uint8_t>(16*16*3,2)};
    for (auto [file, img] : {std::pair{"a.png", a}, std::pair{"b.png", b}}) {
        auto png = img.png();
        std::ofstream out(output.path / file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    }
    auto jobs = std::async(std::launch::async, [&] {
        return pipeline.run_batch({{output.path / "a.png", output.path / "a-out"},
                                   {output.path / "b.png", output.path / "b-out"}}, {2, 2, 2, 2});
    });
    entered.get_future().wait();
    second_layout.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    release.set_value();
    auto results = jobs.get();
    expect(results[0].error.empty() && results[1].error.empty() &&
           results[0].document.pages[0].regions.size() == 8 && results[1].document.pages[0].regions.size() == 1,
           "page result lost during fair BOX scheduling");
    const auto b_position = std::find(visited.begin(), visited.end(), 2);
    expect(b_position != visited.end() && b_position < visited.end() - 1,
           "a dense page monopolized the global BOX pool");
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
void postprocess_test() {
    auto make_box=[](std::string type,std::array<double,4> bounds,double score,size_t index) {
        Box box; box.type=type; box.raw_type=type; box.bbox=bounds;
        box.score=score; box.source_index=index; box.order=int(index);
        box.reading_order=int(index); return box;
    };
    Image page{100,100,std::vector<uint8_t>(100*100*3,7)};
    Json overlap={{"overlap",{{"enabled",true},{"iou_threshold",0.5},{"score_margin",0.3}}}};
    auto table=make_box("table",{0,0,20,20},.65,0);
    auto text=make_box("text",{1,1,19,19},.85,1);
    auto merged=postprocess_boxes({table,text},overlap,page);
    expect(merged.size()==1 && merged[0].type=="table","overlap priority within score margin");
    text.score=.96;
    merged=postprocess_boxes({table,text},overlap,page);
    expect(merged.size()==1 && merged[0].type=="text","score gap above 0.3 should override label priority");
    text.score=.95;
    merged=postprocess_boxes({table,text},overlap,page);
    expect(merged.size()==1 && merged[0].type=="text",
           "score gap exactly 0.3 should override label priority");
    Json settings={{"low_score",{{"enabled",true},{"threshold",0.3}}},
        {"header_footer",{{"enabled",true}}},
        {"overlap",{{"enabled",true}}},
        {"composite",{{"enabled",true},{"retain_min_score",0.7}}}};
    auto chart=make_box("chart",{10,10,70,70},.9,0);
    auto inner=make_box("text",{20,20,30,30},.85,1);
    auto header=make_box("header",{0,0,10,5},.95,2);
    auto noise=make_box("text",{71,50,80,60},.2,3);
    int redetections=0;
    auto boxes=postprocess_boxes({chart,inner,header,noise},settings,page,
        [&](const Image& whitened) {
            ++redetections;
            expect(whitened.rgb[(20*100+20)*3]==255 && whitened.rgb[(80*100+80)*3]==7,
                   "composite whiteout must cover outer chart only");
            return std::vector<Box>{make_box("text",{75,5,95,20},.8,0),
                                    make_box("text",{20,20,30,30},.9,1),
                                    make_box("header",{0,0,10,5},.9,2)};
        });
    expect(redetections==1 && boxes.size()==2 &&
           boxes[0].type=="chart" && boxes[0].extensions["omniocr.composite_children"].size()==1 &&
           boxes[1].extensions.value("omniocr.recovered",false),
           "composite should retain confident chart and discover unmasked content once");
    chart.score=.5;
    boxes=postprocess_boxes({chart,inner},settings,page);
    expect(boxes.size()==1 && boxes[0].type=="text" &&
           boxes[0].extensions["omniocr.parent_source_index"]==0,
           "low-confidence composite should expand its children");
    chart.score=.95;
    auto formula=make_box("formula",{25,25,29,29},.9,2);
    boxes=postprocess_boxes({chart,inner,formula},settings,page);
    expect(boxes.size()==2 && boxes[0].type=="text" && boxes[1].type=="formula",
           "semantic descendant should expand chart through the containment hierarchy");
    settings["composite"]["inspect_inner_text"]=true;
    boxes=postprocess_boxes({chart,inner},settings,page);
    expect(boxes.size()==2 && boxes[0].extensions.contains("omniocr.provisional_children"),
           "content inspection must keep child OCR provisionally");
    Page inspected; inspected.regions.resize(boxes.size());
    for (size_t i=0;i<boxes.size();++i) inspected.regions[i].box=boxes[i];
    inspected.regions[0].text="chart explanation";
    inspected.regions[1].text="missing data";
    finalize_composite_page(inspected,settings,{});
    expect(inspected.regions.size()==1 && inspected.regions[0].box.type=="text" &&
           inspected.regions[0].box.extensions["omniocr.parent_source_index"]==0,
           "uncovered inner OCR should expand composite");
    inspected.regions.resize(boxes.size());
    for (size_t i=0;i<boxes.size();++i) inspected.regions[i].box=boxes[i];
    inspected.regions[0].text="chart includes missing data";
    inspected.regions[1].text="missing data";
    finalize_composite_page(inspected,settings,{});
    expect(inspected.regions.size()==1 && inspected.regions[0].box.type=="chart" &&
           inspected.regions[0].box.extensions["omniocr.composite_children"].size()==1,
           "covered inner OCR should retain composite");

    const std::string header_row="<tr><th>A</th><th>B</th></tr>";
    auto html=[&](const std::string& value) {
        return "<table>\n"+header_row+"<tr><td>"+value+"</td><td>2</td></tr>\n</table>";
    };
    Document doc; doc.source="test";
    for (int number=1;number<=3;++number) {
        Page p; p.number=number;p.width=p.height=100;
        Region r; r.box=make_box("table",{10,double(number==1 ? 80 : 1),90,
                         double(number==3 ? 60 : 99)},.9,size_t(number));
        r.text=html(std::to_string(number));p.regions.push_back(std::move(r));
        doc.pages.push_back(std::move(p));
    }
    Json table_settings={{"cross_page_tables",{{"enabled",true},{"require_header_match",true}}}};
    postprocess_document(doc,table_settings);
    const auto& result=doc.pages[0].regions[0];
    expect(result.text.find("<td>3</td>")!=std::string::npos &&
           result.box.extensions["omniocr.merged_pages"].size()==2 &&
           doc.pages[1].regions[0].text.empty() && doc.pages[2].regions[0].text.empty() &&
           document_json(doc)["schema_version"]==2,
           "three-page table continuation must merge into one block with provenance");
    doc.pages[1].regions[0].text=html("other");
    doc.pages[1].regions[0].box.extensions=Json::object();
    doc.pages[1].regions[0].box.bbox={50,1,99,99};
    doc.pages[0].regions[0].text=html("1");
    doc.pages[0].regions[0].box.extensions=Json::object();
    postprocess_document(doc,table_settings);
    expect(doc.pages[1].regions[0].text==html("other"),"misaligned tables must remain separate");
    auto c=config();c["postprocess"]=settings;
    validate_config(c);
    c["postprocess"]["overlap"]["score_margin"]=1.2;
    throws([&]{validate_config(c);});
    c["postprocess"]["overlap"]["score_margin"]=0.3;
    c["postprocess"]["composite"]["max_recovered_boxes"]=0;
    throws([&]{validate_config(c);});
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

    auto filtered=config();
    filtered["models"]["layout"]["response"]["boxes"] = Json::array({
        {{"type","header"},{"bbox",{0,0,1,.1}},{"score",.95}},
        {{"type","title"},{"bbox",{0,.1,1,.5}},{"score",.8}},
        {{"type","text"},{"bbox",{0,.1,1,.5}},{"score",.6}},
        {{"type","text"},{"bbox",{0,.5,1,1}},{"score",.1}}});
    filtered["postprocess"]={{"low_score",{{"enabled",true},{"threshold",.3}}},
        {"header_footer",{{"enabled",true}}},
        {"overlap",{{"enabled",true},{"score_margin",.3}}}};
    auto cleaned=Pipeline(filtered).run(temp.path/"input.png",temp.path/"filtered");
    expect(cleaned.pages[0].regions.size()==1 && cleaned.pages[0].regions[0].box.type=="title",
           "page postprocess must suppress header, low score and overlap before routing");
}
void composite_pipeline_test() {
    TempDir temp;
    Image page{100,100,std::vector<uint8_t>(100*100*3,7)};
    auto png=page.png();
    { std::ofstream out(temp.path/"input.png",std::ios::binary);
      out.write(reinterpret_cast<const char*>(png.data()),std::streamsize(png.size())); }
    auto c=config();
    c["models"]["layout"]["response"]={{"boxes",Json::array()}};
    c["routes"]["chart"]={{"model","shared"}};
    c["postprocess"]={{"composite",{{"enabled",true}}}};
    struct Fixture : Model {
        bool layout;std::atomic<int>& passes;std::atomic<int>& recognition;
        Fixture(bool l,std::atomic<int>& p,std::atomic<int>& r):layout(l),passes(p),recognition(r){}
        Json infer(const Image& image,const std::string&) override {
            if (!layout) {++recognition; return {{"text","recognized"}};}
            ++passes;
            if (image.rgb[(20*100+20)*3]==255)
                return {{"boxes",Json::array({{{"type","text"},{"bbox",{.75,.1,.95,.2}}}})}};
            return {{"boxes",Json::array({
                {{"type","chart"},{"bbox",{.1,.1,.7,.7}},{"score",.95}},
                {{"type","text"},{"bbox",{.2,.2,.3,.3}},{"score",.8}}
            })}};
        }
    };
    std::atomic<int> passes{0},recognition{0};
    Pipeline pipeline(c,[&](const Json& settings,size_t) {
        const bool layout=settings.at("response").contains("boxes");
        return std::make_unique<Fixture>(layout,passes,recognition);
    });
    auto result=pipeline.run(temp.path/"input.png",temp.path/"output");
    expect(passes==2 && recognition==2 && result.pages[0].regions.size()==2 &&
           result.pages[0].regions[0].box.type=="chart" &&
           result.pages[0].regions[1].box.extensions.value("omniocr.recovered",false),
           "pipeline must run exactly one whiteout layout pass before OCR routing");
    c["postprocess"]["composite"]["inspect_inner_text"]=true;
    struct ContentFixture : Model {
        bool layout;
        explicit ContentFixture(bool l):layout(l){}
        Json infer(const Image& crop,const std::string&) override {
            if (!layout) return {{"text",crop.width>30 ? "chart summary" : "inner details"}};
            if (crop.rgb[(20*100+20)*3]==255) return {{"boxes",Json::array()}};
            return {{"boxes",Json::array({
                {{"type","chart"},{"bbox",{.1,.1,.7,.7}},{"score",.95}},
                {{"type","text"},{"bbox",{.2,.2,.3,.3}},{"score",.8}}
            })}};
        }
    };
    auto inspected=Pipeline(c,[&](const Json& settings,size_t) {
        return std::make_unique<ContentFixture>(settings.at("response").contains("boxes"));
    }).run(temp.path/"input.png",temp.path/"inspected");
    expect(inspected.pages[0].regions.size()==1 &&
           inspected.pages[0].regions[0].box.type=="text" &&
           inspected.pages[0].regions[0].text=="inner details",
           "pipeline must choose inner content after OCR when composite misses text");
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
void scheduler_config_test() {
    auto settings = config();
    settings["execution"].update({{"page_workers", 2}, {"box_workers", 4},
                                  {"document_workers", 3}, {"max_queued_pages", 8}});
    settings["server"] = {{"host", "127.0.0.1"}, {"port", 18080},
        {"data_dir", "service-state"}, {"allowed_input_root", "documents"},
        {"max_active_jobs", 12}, {"max_queued_page_bytes", 1ULL << 32},
        {"http_connections", 128}, {"connection_timeout_seconds", 30},
        {"max_result_bytes", 64 * 1024 * 1024}, {"max_asset_bytes", 128 * 1024 * 1024}};
    validate_config(settings);
    TempDir temp;
    const auto path = temp.path / "settings.json";
    { std::ofstream out(path); out << settings.dump(); }
    const auto loaded = load_config(path);
    expect(loaded["server"]["data_dir"] == (temp.path / "service-state").string() &&
           loaded["server"]["allowed_input_root"] == (temp.path / "documents").string(),
           "service paths must resolve relative to the configuration file");
    auto bad = settings;
    bad["execution"]["box_workers"] = 0;
    throws([&] { validate_config(bad); });
    bad = settings; bad["execution"]["page_workers"] = 1.5;
    throws([&] { validate_config(bad); });
    bad = settings; bad["execution"]["box_worker"] = 8;
    throws([&] { validate_config(bad); });
    bad = settings; bad["server"]["http_connections"] = 15;
    throws([&] { validate_config(bad); });
    bad = settings; bad["server"]["max_inflight_upload_bytes"] = -1;
    throws([&] { validate_config(bad); });
    bad = settings; bad["server"]["max_active_jobs"] = 1.5;
    throws([&] { validate_config(bad); });
    bad = settings; bad["server"]["connection_timeout_seconds"] = 0;
    throws([&] { validate_config(bad); });
    bad = settings; bad["server"]["max_result_bytes"] = 0;
    throws([&] { validate_config(bad); });
    bad = settings; bad["server"]["max_jobs_typo"] = 1;
    throws([&] { validate_config(bad); });
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
        input_format_test(); layout_test(); v3_plugin_test(); v2_config_test(); pool_test(); model_input_resize_test(); cancellation_pool_test(); visual_bucket_scheduler_test(); adaptive_concurrency_test(); dynamic_batch_test(); box_pool_fairness_test(); codec_test(); postprocess_test(); pipeline_test(); composite_pipeline_test(); fallback_test(); table_fallback_test(); batch_test(); scheduler_config_test(); image_process_test();
        std::cout << "PASS: layout, shared pool, timeout/recovery, codecs, pipeline, output, subprocess\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
