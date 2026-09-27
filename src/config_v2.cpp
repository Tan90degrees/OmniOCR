#include "omniocr/core.hpp"
#include <stdexcept>

namespace omniocr {
// v2 is a declarative view of the same per-executor model pools. We normalize
// once, before any model is constructed, so CLI/batch/REST use one scheduler.
Json normalize_config(const Json& config) {
    if (!config.is_object() || !config.contains("version") || config.at("version") != 2)
        return config;
    const auto& executors=config.at("executors");
    const auto& bindings=config.at("models");
    const auto& pipeline=config.at("pipeline");
    if (!executors.is_object() || executors.empty() ||
        !bindings.is_object() || bindings.empty() || !pipeline.is_object())
        throw std::runtime_error("config v2 needs nonempty executors, models and pipeline");
    Json normalized={{"version",1},{"models",Json::object()}};
    if (config.contains("plugins")) normalized["plugins"]=config.at("plugins");
    if (config.contains("execution")) normalized["execution"]=config.at("execution");
    if (config.contains("server")) normalized["server"]=config.at("server");
    if (config.contains("document")) normalized["document"]=config.at("document");
    if (config.contains("output")) normalized["output"]=config.at("output");
    for (const auto& [id, executor] : executors.items()) {
        Json m=executor;
        if (!m.is_object()) throw std::runtime_error("config v2 executor must be an object: "+id);
        if (m.contains("max_inflight")) {
            if (m.contains("instances")) throw std::runtime_error("v2 executor sets both max_inflight and instances");
            m["instances"]=m.at("max_inflight");
            m.erase("max_inflight");
        }
        if (m.contains("served_model")) {
            m["model"]=m.at("served_model");
            m.erase("served_model");
        }
        if (m.value("backend",std::string{})=="openai_chat") m["backend"]="vllm";
        normalized["models"][id]=std::move(m);
    }
    auto binding=[&](const std::string& id) -> const Json& {
        if (!bindings.contains(id)) throw std::runtime_error("unknown v2 model binding: "+id);
        const auto& entry=bindings.at(id);
        if (!entry.is_object() || !entry.contains("executor") || !entry.contains("adapter"))
            throw std::runtime_error("v2 model binding needs adapter and executor: "+id);
        const auto executor=entry.at("executor").get<std::string>();
        if (!normalized["models"].contains(executor))
            throw std::runtime_error("unknown v2 executor: "+executor);
        return entry;
    };
    Json layout=pipeline.at("layout");
    const auto layout_binding=layout.at("model").get<std::string>();
    const auto& layout_model=binding(layout_binding);
    const std::string layout_adapter=layout_model.at("adapter").get<std::string>();
    layout["model"]=layout_model.at("executor");
    if (!layout.contains("provider")) layout["provider"]=layout_adapter;
    if (!layout.contains("adapter")) layout["adapter"]=layout_adapter;
    normalized["layout"]=std::move(layout);
    normalized["routes"]=pipeline.at("routes");
    for (auto& [type, route] : normalized["routes"].items()) {
        if (!route.is_object()) throw std::runtime_error("v2 route must be an object: "+type);
        if (route.value("action",std::string{})=="save_crop") route["action"]="image";
        if (route.contains("task")) {
            if (!route.contains("adapter") && route.at("task")=="table_recognition")
                route["adapter"]="table";
            route.erase("task");
        }
        if (route.contains("model")) {
            const std::string id=route.at("model").get<std::string>();
            const auto& selected=binding(id);
            if (!route.contains("adapter")) route["adapter"]=selected.at("adapter");
            route["binding_id"]=id;
            route["model"]=selected.at("executor");
        }
        if (route.contains("models")) {
            Json candidates=Json::array();
            Json aliases=Json::object();
            for (const auto& candidate : route.at("models")) {
                const auto name=candidate.get<std::string>();
                const auto& selected=binding(name);
                const auto executor=selected.at("executor").get<std::string>();
                candidates.push_back(executor);
                aliases[executor]=name;
                if (!route.contains("adapter")) route["adapter"]=selected.at("adapter");
                else if (route.at("adapter")!=selected.at("adapter"))
                    throw std::runtime_error("v2 route candidates have different task adapters: "+type);
            }
            route["models"]=std::move(candidates);
            route["binding_ids"]=std::move(aliases);
        }
    }
    return normalized;
}
} // namespace omniocr
