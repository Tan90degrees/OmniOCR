#include "omniocr/plugins.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omniocr {
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
    const double x0=std::floor(std::clamp(box.bbox[0],0.,double(image.width)));
    const double y0=std::floor(std::clamp(box.bbox[1],0.,double(image.height)));
    Image crop=image.crop(box.bbox); // validates source and bounds before masking
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
