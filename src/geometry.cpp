#include "omniocr/plugins.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omniocr {
std::array<double,2> TransformContext::to_page(double x,double y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("nonfinite input point");
    const auto& m=model_to_page;
    const double z=m[6]*x+m[7]*y+m[8];
    if (!std::isfinite(z) || std::abs(z)<1e-12) throw std::runtime_error("invalid perspective transform");
    const double px=(m[0]*x+m[1]*y+m[2])/z;
    const double py=(m[3]*x+m[4]*y+m[5])/z;
    if (!std::isfinite(px) || !std::isfinite(py)) throw std::runtime_error("nonfinite mapped point");
    return {std::clamp(px,0.,double(page_width)),std::clamp(py,0.,double(page_height))};
}
TransformContext make_transform_context(const Json& layout,int width,int height) {
    if (width<=0 || height<=0) throw std::runtime_error("invalid rendered-page dimensions");
    TransformContext result; result.page_width=width; result.page_height=height;
    const auto coordinates=layout.value("coordinates",std::string("pixel"));
    if (coordinates=="normalized") {
        result.model_to_page={double(width),0,0, 0,double(height),0, 0,0,1};
    } else if (coordinates=="model_input") {
        const auto size=layout.at("image_size");
        if (!size.is_array() || size.size()!=2 || size[0].get<int>()<=0 || size[1].get<int>()<=0)
            throw std::runtime_error("invalid model input dimensions");
        result.model_to_page={double(width)/size[0].get<int>(),0,0,
                              0,double(height)/size[1].get<int>(),0, 0,0,1};
    } else if (coordinates!="pixel") throw std::runtime_error("unsupported layout coordinate system");
    if (layout.contains("transform")) {
        if (coordinates!="model_input") throw std::runtime_error("explicit transform requires model_input coordinates");
        const auto& t=layout.at("transform").at("matrix");
        if (!t.is_array() || t.size()!=9) throw std::runtime_error("transform matrix must contain 9 numbers");
        for (size_t i=0;i<9;++i) {
            if (!t[i].is_number()) throw std::runtime_error("transform matrix element must be numeric");
            result.model_to_page[i]=t[i].get<double>();
            if (!std::isfinite(result.model_to_page[i]))
                throw std::runtime_error("transform matrix contains nonfinite value");
        }
    }
    return result;
}
namespace {
bool contains(const std::vector<std::array<double,2>>& polygon, double x, double y) {
    bool inside=false;
    for (size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++) {
        const auto& a=polygon[i]; const auto& b=polygon[j];
        if ((a[1]>y)!=(b[1]>y)) {
            const double intersect=(b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0];
            if (x<intersect) inside=!inside;
        }
    }
    return inside;
}
}
Image polygon_mask_crop(const Image& image, const Box& box, const Json& route) {
    if (box.polygon.empty()) {
        if (route.value("crop_fallback",std::string{})=="bbox_crop") return image.crop(box.bbox);
        throw std::runtime_error("polygon_mask_crop requires polygon; set crop_fallback=bbox_crop explicitly");
    }
    if (box.polygon.size()<3) throw std::runtime_error("invalid polygon");
    const auto& crop_box=box.crop_bbox ? *box.crop_bbox : box.bbox;
    const double x0=std::floor(std::clamp(crop_box[0],0.,double(image.width)));
    const double y0=std::floor(std::clamp(crop_box[1],0.,double(image.height)));
    Image crop=image.crop(crop_box); // validates source and bounds before masking
    // Compute coordinates in original page space; crop origin follows Image::crop floor/clamp.
    const uint8_t background=uint8_t(route.value("mask_background",255));
    for (int y=0;y<crop.height;++y) for (int x=0;x<crop.width;++x) {
        if (contains(box.polygon,x0+x+0.5,y0+y+0.5)) continue;
        const size_t offset=(size_t(y)*crop.width+x)*3;
        crop.rgb[offset]=crop.rgb[offset+1]=crop.rgb[offset+2]=background;
    }
    return crop;
}
} // namespace omniocr
