#include "omniocr/plugins.hpp"
#include "omniocr/plugin_abi.h"
#include <dlfcn.h>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

namespace omniocr {
std::vector<Box> parse_doclayout_v3(const Json&, const Json&, int, int);
namespace {
struct SharedLibrary {
    void* handle = nullptr;
    const omniocr_plugin_api_v1* api = nullptr;
    ~SharedLibrary() { if (handle) dlclose(handle); }
};
std::map<std::string, std::shared_ptr<SharedLibrary>>& libraries() {
    static std::map<std::string,std::shared_ptr<SharedLibrary>> result; return result;
}
std::mutex& library_mutex() { static std::mutex result; return result; }
class Instance {
    std::shared_ptr<SharedLibrary> library_;
    void* handle_ = nullptr;
    std::mutex mutex_;
public:
    Instance(std::shared_ptr<SharedLibrary> library, const Json& config, size_t index)
        : library_(std::move(library)) {
        const auto payload=config.dump();
        handle_=library_->api->create(payload.data(),payload.size(),index);
        if (!handle_) throw std::runtime_error("plugin instance creation failed: "+
                                                   std::string(library_->api->plugin_id));
    }
    ~Instance() { if (handle_) library_->api->destroy(handle_); }
    Json execute(const Json& input) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto request=input.dump();
        char* output=nullptr; size_t length=0;
        char* error=nullptr; size_t error_length=0;
        struct Release {
            const omniocr_plugin_api_v1* api; void* instance;
            char*& output; char*& error;
            ~Release() {
                if (output) api->release(instance,output);
                if (error && error!=output) api->release(instance,error);
            }
        } cleanup{library_->api,handle_,output,error};
        const int rc=library_->api->execute(handle_,request.data(),request.size(),
                                          &output,&length,&error,&error_length);
        constexpr size_t limit=32u*1024u*1024u;
        if (length>limit || error_length>4096) throw std::runtime_error("plugin response exceeds size cap");
        if (rc!=0) {
            const std::string details=error && error_length
                ? std::string(error,error_length) : "unknown error";
            throw std::runtime_error("plugin execution failed: "+details);
        }
        if (!output) throw std::runtime_error("plugin returned empty response buffer");
        return Json::parse(output,output+length);
    }
};
class DynamicModel final : public Model {
    std::unique_ptr<Instance> instance_;
public:
    DynamicModel(std::shared_ptr<SharedLibrary> library,const Json& config,size_t index)
        : instance_(std::make_unique<Instance>(std::move(library),config,index)) {}
    Json infer(const Image& image,const std::string& prompt) override {
        return instance_->execute({{"image","data:image/png;base64,"+base64(image.png())},
                                    {"width",image.width},{"height",image.height},{"prompt",prompt}});
    }
};
void install(const std::shared_ptr<SharedLibrary>& library) {
    const std::string id=library->api->plugin_id;
    const std::string kind=library->api->kind;
    if (id.empty() || id.size()>128) throw std::runtime_error("invalid plugin identifier");
    if (kind=="backend") {
        register_backend(id,[library](const Json& config,size_t index) {
            return std::make_unique<DynamicModel>(library,config,index);
        });
    } else if (kind=="layout") {
        auto instance=std::make_shared<Instance>(library,Json::object(),0);
        register_layout_adapter(id,[instance,id](const Json& response,const Json& settings,int w,int h) {
            const auto normalized=instance->execute({{"response",response},{"settings",settings},
                                                       {"width",w},{"height",h}});
            // External layout plugin returns the common Paddle-style region shape
            // containing label/coordinate/polygon_points/order; not raw tensor heads.
            Json context=settings;
            context["adapter"]=id;
            return parse_doclayout_v3(normalized,context,w,h);
        });
    } else if (kind=="recognition") {
        auto instance=std::make_shared<Instance>(library,Json::object(),0);
        register_recognition_adapter(id,[instance](const Json& response,const Json& settings) {
            auto result=instance->execute({{"response",response},{"settings",settings}});
            return RecognitionResult{result.at("text").get<std::string>(),
                result.value("raw_text",std::string{})};
        });
    } else throw std::runtime_error("unsupported plugin kind: "+kind);
}
} // namespace
void load_plugins(const Json& config) {
    if (!config.contains("plugins")) return;
    const auto& declared=config.at("plugins");
    if (!declared.is_array()) throw std::runtime_error("plugins must be an array");
    std::lock_guard<std::mutex> guard(library_mutex());
    for (const auto& item:declared) {
        const auto expected=item.at("id").get<std::string>();
        const auto library_path=fs::canonical(item.at("library").get<std::string>()).string();
        auto existing=libraries().find(library_path);
        if (existing!=libraries().end()) {
            if (expected!=existing->second->api->plugin_id)
                throw std::runtime_error("plugin ID does not match previously loaded library");
            continue;
        }
        auto library=std::make_shared<SharedLibrary>();
        library->handle=dlopen(library_path.c_str(),RTLD_NOW|RTLD_LOCAL);
        if (!library->handle) {
            const char* reason=dlerror();
            throw std::runtime_error("cannot load plugin library: "+std::string(reason?reason:"unknown"));
        }
        auto entry=reinterpret_cast<omniocr_plugin_entry_v1_fn>(
            dlsym(library->handle,"omniocr_plugin_entry_v1"));
        if (!entry) throw std::runtime_error("plugin missing omniocr_plugin_entry_v1");
        library->api=entry();
        const auto api=library->api;
        if (!api || api->abi_version!=OMNIOCR_PLUGIN_ABI_V1 ||
            api->struct_size<sizeof(omniocr_plugin_api_v1))
            throw std::runtime_error("incompatible plugin ABI");
        if (!api->plugin_id || !api->kind || expected!=api->plugin_id ||
            !api->create || !api->execute || !api->release || !api->destroy)
            throw std::runtime_error("invalid plugin descriptor or mismatched ID");
        install(library);
        libraries().emplace(library_path,std::move(library));
    }
}
} // namespace omniocr
