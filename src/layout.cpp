#include "omniocr/core.hpp"
#include <algorithm>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace omniocr {
std::vector<Box> parse_legacy_layout(const Json& response, const Json& layout, int width, int height) {
    std::vector<Box> boxes;
    const auto provider = layout.at("provider").get<std::string>();
    auto aliases = layout.value("type_map", Json::object());
    auto append = [&](Box b) {
        for (double x : b.bbox) if (!std::isfinite(x)) throw std::runtime_error("non-finite layout coordinate");
        if (!std::isfinite(b.score) || b.score < 0 || b.score > 1) throw std::runtime_error("invalid layout score");
        if (b.rotation != 0 && b.rotation != 90 && b.rotation != 180 && b.rotation != 270)
            throw std::runtime_error("invalid layout rotation");
        b.bbox[0] = std::clamp(b.bbox[0], 0., double(width));
        b.bbox[2] = std::clamp(b.bbox[2], 0., double(width));
        b.bbox[1] = std::clamp(b.bbox[1], 0., double(height));
        b.bbox[3] = std::clamp(b.bbox[3], 0., double(height));
        if (b.bbox[2] <= b.bbox[0] || b.bbox[3] <= b.bbox[1]) throw std::runtime_error("empty or inverted layout box");
        if (b.score < layout.value("score_threshold", 0.0)) return;
        b.raw_type = b.type;
        if (aliases.contains(b.type)) b.type = aliases.at(b.type).get<std::string>();
        b.source_index = boxes.size();
        b.reading_order = b.order;
        b.provenance = {{"adapter", provider}};
        boxes.push_back(std::move(b));
        if (boxes.size() > layout.value("max_boxes", size_t(2000))) throw std::runtime_error("too many layout boxes");
    };
    if (provider == "mineru") {
        const auto text = response.at("text").get<std::string>();
        size_t matched = 0;
        const std::regex token(R"(<\|box_start\|>(\d+)\s+(\d+)\s+(\d+)\s+(\d+)<\|box_end\|><\|ref_start\|>(\w+)<\|ref_end\|>(?:<\|rotate_(up|right|down|left)\|>)?)");
        for (std::sregex_iterator it(text.begin(), text.end(), token), end; it != end; ++it) {
            ++matched;
            Box b; b.type = (*it)[5].str(); b.order = int(boxes.size());
            for (int i = 0; i < 4; ++i) {
                const int n = std::stoi((*it)[i + 1].str());
                if (n > 1000) throw std::runtime_error("MinerU coordinate outside 0..1000");
                b.bbox[i] = double(n) * double(i % 2 ? height : width) / 1000.0;
            }
            if (b.bbox[0] > b.bbox[2]) std::swap(b.bbox[0], b.bbox[2]);
            if (b.bbox[1] > b.bbox[3]) std::swap(b.bbox[1], b.bbox[3]);
            const auto rotation = (*it)[6].str();
            b.rotation = rotation == "right" ? 90 : rotation == "down" ? 180 : rotation == "left" ? 270 : 0;
            append(b);
        }
        // Do not silently emit an empty document on a wrong model/protocol response.
        if (!matched && text.find_first_not_of(" \t\r\n") != std::string::npos)
            throw std::runtime_error("no valid MinerU layout tokens in response");
        size_t starts = 0, position = 0;
        while ((position = text.find("<|box_start|>", position)) != std::string::npos) { ++starts; position += 13; }
        if (starts != matched) throw std::runtime_error("malformed MinerU layout block");
    } else {
        const Json* root = &response;
        if (root->contains("res")) root = &root->at("res");
        const auto& list = root->at("boxes");
        if (!list.is_array()) throw std::runtime_error("layout boxes must be an array");
        for (const auto& item : list) {
            Box b;
            b.type = item.at(provider == "paddle" ? "label" : "type").get<std::string>();
            b.bbox = item.at(provider == "paddle" ? "coordinate" : "bbox").get<std::array<double, 4>>();
            b.score = item.value("score", 1.0);
            b.order = int(boxes.size());
            if (item.contains("order") && !item.at("order").is_null()) {
                b.order = item.at("order").get<int>();
            }
            b.rotation = item.value("rotation", 0);
            if (layout.value("coordinates", "pixel") == "normalized")
                for (int i = 0; i < 4; ++i) b.bbox[i] *= i % 2 ? height : width;
            append(b);
        }
    }
    std::stable_sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.order < b.order; });
    return boxes;
}
} // namespace omniocr
