#include "omniocr/plugins.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omniocr {
namespace {
double numeric(const Json& j) {
    if (!j.is_number()) throw std::runtime_error("V3 geometry coordinate must be numeric");
    const double v = j.get<double>();
    if (!std::isfinite(v)) throw std::runtime_error("V3 geometry coordinate is non-finite");
    return v;
}
std::vector<std::array<double,2>> polygon(const Json& source) {
    if (!source.is_array() || source.empty()) throw std::runtime_error("V3 polygon must be a nonempty array");
    std::vector<std::array<double,2>> p;
    if (source.front().is_number()) {
        if (source.size() % 2 != 0) throw std::runtime_error("V3 polygon has odd coordinate count");
        for (size_t i=0; i<source.size(); i+=2) p.push_back({numeric(source[i]),numeric(source[i+1])});
    } else {
        for (const auto& point : source) {
            if (!point.is_array() || point.size()!=2) throw std::runtime_error("V3 polygon point must be [x,y]");
            p.push_back({numeric(point[0]),numeric(point[1])});
        }
    }
    if (p.size()<3 || p.size()>4096) throw std::runtime_error("V3 polygon requires 3..4096 points");
    return p;
}
double signed_area(const std::vector<std::array<double,2>>& p) {
    double total=0.;
    for (size_t i=0; i<p.size(); ++i) {
        const auto& a=p[i]; const auto& b=p[(i+1)%p.size()];
        total+=a[0]*b[1]-b[0]*a[1];
    }
    return total*.5;
}
}
std::vector<Box> parse_doclayout_v3(const Json& response, const Json& layout, int width, int height) {
    if (width<=0 || height<=0) throw std::runtime_error("invalid page dimensions");
    const Json* root=&response;
    if (root->contains("res")) root=&root->at("res");
    const auto& items=root->at("boxes");
    if (!items.is_array()) throw std::runtime_error("V3 boxes must be an array");
    const std::string coordinates=layout.value("coordinates",std::string("pixel"));
    const bool normalized=coordinates=="normalized";
    const bool input_coords=coordinates=="model_input";
    double sx=1., sy=1.;
    if (normalized) { sx=width; sy=height; }
    if (input_coords) {
        const auto& dims=layout.at("image_size");
        sx=double(width)/dims.at(0).get<int>();
        sy=double(height)/dims.at(1).get<int>();
    }
    const auto aliases=layout.value("type_map",Json::object());
    const double threshold=layout.value("score_threshold",0.0);
    const size_t max_boxes=layout.value("max_boxes",size_t(2000));
    std::vector<Box> result; result.reserve(std::min(items.size(),max_boxes));
    for (size_t index=0; index<items.size(); ++index) {
        const auto& item=items[index];
        Box b;
        b.source_index=index;
        b.type=item.at("label").get<std::string>();
        b.raw_type=b.type;
        if (aliases.contains(b.type)) b.type=aliases.at(b.type).get<std::string>();
        b.score=item.contains("score") && !item.at("score").is_null() ? numeric(item.at("score")) : 1.0;
        if (b.score<0 || b.score>1) throw std::runtime_error("invalid V3 score");
        b.rotation=item.value("rotation",0);
        if (b.rotation!=0 && b.rotation!=90 && b.rotation!=180 && b.rotation!=270)
            throw std::runtime_error("invalid V3 rotation");
        if (item.contains("order") && !item.at("order").is_null())
            b.reading_order=item.at("order").get<int>();
        // A missing order is not the same as a null order: preserve source
        // sequence when the server does not run a reading-order head.
        if (!item.contains("order")) b.reading_order=int(index);
        b.order=b.reading_order.value_or(int(index));
        if (item.contains("polygon_points") && !item.at("polygon_points").is_null()) {
            b.polygon=polygon(item.at("polygon_points"));
            for (auto& point : b.polygon) {
                point[0]=std::clamp(point[0]*sx,0.,double(width));
                point[1]=std::clamp(point[1]*sy,0.,double(height));
            }
            if (std::abs(signed_area(b.polygon))<1e-6)
                throw std::runtime_error("degenerate V3 polygon");
        }
        if (item.contains("coordinate") && !item.at("coordinate").is_null()) {
            const auto& coordinate=item.at("coordinate");
            if (!coordinate.is_array() || coordinate.size()!=4) throw std::runtime_error("invalid V3 bbox");
            for (int i=0;i<4;++i) {
                const double v=numeric(coordinate[i])*(i%2?sy:sx);
                b.bbox[i]=std::clamp(v,0.,double(i%2?height:width));
            }
        } else if (!b.polygon.empty()) {
            b.bbox={double(width),double(height),0.,0.};
            for (const auto& point : b.polygon) {
                b.bbox[0]=std::min(b.bbox[0],point[0]);
                b.bbox[1]=std::min(b.bbox[1],point[1]);
                b.bbox[2]=std::max(b.bbox[2],point[0]);
                b.bbox[3]=std::max(b.bbox[3],point[1]);
            }
        } else throw std::runtime_error("V3 region missing bbox and polygon");
        if (b.bbox[2]<=b.bbox[0] || b.bbox[3]<=b.bbox[1])
            throw std::runtime_error("empty or inverted V3 bbox");
        b.provenance={{"adapter","paddle.doclayout_v3.http"}};
        if (item.contains("mask_ref")) {
            if (!item.at("mask_ref").is_string()) throw std::runtime_error("V3 mask_ref must be string");
            b.extensions["paddle.mask_ref"]=item.at("mask_ref");
        }
        if (b.score<threshold) continue;
        result.push_back(std::move(b));
        if (result.size()>max_boxes) throw std::runtime_error("too many layout boxes");
    }
    std::stable_sort(result.begin(),result.end(),[](const Box& a,const Box& b) {
        if (bool(a.reading_order)!=bool(b.reading_order)) return bool(a.reading_order);
        if (a.reading_order && b.reading_order && *a.reading_order!=*b.reading_order)
            return *a.reading_order<*b.reading_order;
        return a.source_index<b.source_index;
    });
    return result;
}
} // namespace omniocr
