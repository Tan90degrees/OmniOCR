#include "omniocr/core.hpp"
#include "../src/recognition_pool.hpp"
#include <fstream>
#include <iostream>
using namespace omniocr;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void throws(F f) { bool thrown=false; try { f(); } catch (const std::exception&) { thrown=true; } expect(thrown,"expected exception"); }
void test_pipeline() {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        int active=0, peak=0;
        bool release=false;
    } state;
    class BlockingModel final : public Model {
        State& state;
    public:
        explicit BlockingModel(State& state) : state(state) {}
        Json infer(const Image&, const std::string& prompt) override {
            std::unique_lock<std::mutex> lock(state.mutex);
            ++state.active; state.peak=std::max(state.peak,state.active);
            state.changed.notify_all();
            state.changed.wait(lock,[&]{return state.release;});
            --state.active;
            return {{"text",prompt}};
        }
    };
    TempDir temp;
    const auto source=temp.path/"page.ppm";
    {std::ofstream out(source,std::ios::binary);out<<"P6\n20 10\n255\n"<<std::string(600,'x');}
    Json c={{"version",1},{"execution",{{"workers",1},{"box_workers",1},
        {"async_recognition",{{"enabled",true},{"workers",8},{"max_requests",8},{"max_bytes",2400}}}}},
        {"layout",{{"provider","normalized"},{"model","layout"},{"coordinates","normalized"}}},
        {"models",{{"layout",{{"backend","mock"},{"response",{{"boxes",Json::array()}}}}},
                   {"ocr",{{"backend","mock"},{"max_concurrent_requests",4}}}}},
        {"routes",Json::object()}};
    for(int i=0;i<12;++i){
        const auto label="box-"+std::to_string(i);
        c["models"]["layout"]["response"]["boxes"].push_back({{"type",label},{"bbox",{0,0,1,1}},{"order",i}});
        c["routes"][label]={{"model","ocr"},{"prompt",label}};
    }
    Pipeline pipeline(c,[&](const Json& settings,size_t index)->std::unique_ptr<Model>{
        if(settings.contains("response"))return make_model(settings,index);
        return std::make_unique<BlockingModel>(state);
    });
    auto result=std::async(std::launch::async,[&]{return pipeline.run(source,temp.path/"out");});
    bool saturated;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        saturated=state.changed.wait_for(lock,std::chrono::seconds(5),[&]{return state.active==4;});
        state.release=true;
    }
    state.changed.notify_all();
    const auto document=result.get();
    expect(saturated&&state.peak==4,"one preparation worker must saturate four model slots");
    expect(document.pages[0].regions.size()==12,"async BOX output count");
    for(int i=0;i<12;++i)expect(document.pages[0].regions[size_t(i)].text=="box-"+std::to_string(i),"async BOX result attribution");
    const auto metrics=pipeline.recognition_metrics();
    expect(metrics.at("completed_total")==12&&metrics.at("admitted")==0&&metrics.at("admitted_bytes")==0&&
        metrics.at("peak_admitted_bytes")==2400&&metrics.at("peak_admitted")<=4,"async bounds cover queued and running crops");
    for(const auto& invalid:{Json{{"workers",0}},Json{{"max_bytes",-1}},Json{{"max_requests",1.5}},Json{{"enabled",1}}}){
        c["execution"]["async_recognition"]=invalid;
        throws([&]{validate_config(c);});
    }
}
void test_queue(){
    std::promise<void> release,entered,finished;
    auto gate=release.get_future().share();
    RecognitionTaskPool queue({{"workers",1},{"max_requests",1},{"max_bytes",10},{"enqueue_timeout_ms",25}});
    queue.submit(10,[&]{entered.set_value();gate.wait();},[]{return false;},[&]{finished.set_value();});
    entered.get_future().wait();
    throws([&]{queue.submit(1,[]{},[]{return false;},[]{});});
    throws([&]{queue.submit(11,[]{},[]{return false;},[]{});});
    auto token=std::make_shared<std::atomic<bool>>(true);
    {
        CancellationScope scope(token.get());
        bool cancelled=false;
        try{queue.submit(1,[]{},[]{return false;},[]{});}catch(const Cancelled&){cancelled=true;}
        expect(cancelled,"async admission must observe cancellation");
    }
    release.set_value();finished.get_future().wait();
    expect(queue.metrics().at("admission_timeout_total")==1&&queue.metrics().at("oversized_total")==1&&
        queue.metrics().at("admitted_bytes")==0,"rejected tasks must not leak credits");
}
int main(){try{test_pipeline();test_queue();std::cout<<"PASS: async preparation, attribution, byte bounds and admission\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
