#include "omniocr/core.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omniocr {
namespace {
Image allocate(int w, int h) {
    if (w <= 0 || h <= 0 || uint64_t(w) * h > std::numeric_limits<size_t>::max() / 3)
        throw std::runtime_error("invalid image dimensions");
    return {w, h, std::vector<uint8_t>(size_t(w) * h * 3)};
}
}
Image Image::load(const fs::path& p, uint64_t max_pixels) {
    int w, h, c;
    if (!stbi_info(p.c_str(), &w, &h, &c) || w <= 0 || h <= 0)
        throw std::runtime_error("cannot read image: " + p.string());
    if (uint64_t(w) * h > max_pixels) throw std::runtime_error("image exceeds max_pixels");
    auto pixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>(
        stbi_load(p.c_str(), &w, &h, &c, 4), stbi_image_free);
    if (!pixels) throw std::runtime_error("image decoding failed: " + p.string());
    auto out = allocate(w, h);
    // Composite transparency on white so transparent scans do not become black.
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (size_t j = 0; j < 3; ++j) {
            const unsigned a = pixels.get()[i * 4 + 3];
            out.rgb[i * 3 + j] = uint8_t((pixels.get()[i * 4 + j] * a + 255 * (255 - a) + 127) / 255);
        }
    return out;
}
Image Image::crop(const std::array<double, 4>& b) const {
    const int x1 = std::clamp(int(std::floor(b[0])), 0, width);
    const int y1 = std::clamp(int(std::floor(b[1])), 0, height);
    const int x2 = std::clamp(int(std::ceil(b[2])), 0, width);
    const int y2 = std::clamp(int(std::ceil(b[3])), 0, height);
    auto out = allocate(x2 - x1, y2 - y1);
    for (int y = 0; y < out.height; ++y)
        std::copy_n(rgb.data() + (size_t(y + y1) * width + x1) * 3,
                    size_t(out.width) * 3, out.rgb.data() + size_t(y) * out.width * 3);
    return out;
}
Image Image::resize(int w, int h) const {
    auto out = allocate(w, h);
    for (int y = 0; y < h; ++y) {
        const double sy = std::clamp((y + .5) * height / h - .5, 0., double(height - 1));
        int y0 = int(sy), y1 = std::min(y0 + 1, height - 1);
        for (int x = 0; x < w; ++x) {
            const double sx = std::clamp((x + .5) * width / w - .5, 0., double(width - 1));
            int x0 = int(sx), x1 = std::min(x0 + 1, width - 1);
            for (int c = 0; c < 3; ++c) {
                auto at = [&](int px, int py) { return rgb[(size_t(py) * width + px) * 3 + c]; };
                double a = at(x0, y0) * (1 - sx + x0) + at(x1, y0) * (sx - x0);
                double b = at(x0, y1) * (1 - sx + x0) + at(x1, y1) * (sx - x0);
                out.rgb[(size_t(y) * w + x) * 3 + c] = uint8_t(std::round(a * (1 - sy + y0) + b * (sy - y0)));
            }
        }
    }
    return out;
}
Image Image::rotate(int degrees) const {
    if (!degrees) return *this;
    if (degrees != 90 && degrees != 180 && degrees != 270) throw std::runtime_error("invalid rotation");
    // Public angle follows MinerU/PIL: positive values rotate counterclockwise.
    degrees = 360 - degrees;
    auto out = allocate(degrees == 180 ? width : height, degrees == 180 ? height : width);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        int nx = degrees == 90 ? height - 1 - y : degrees == 180 ? width - 1 - x : y;
        int ny = degrees == 90 ? x : degrees == 180 ? height - 1 - y : width - 1 - x;
        std::copy_n(&rgb[(size_t(y) * width + x) * 3], 3, &out.rgb[(size_t(ny) * out.width + nx) * 3]);
    }
    return out;
}
std::vector<uint8_t> Image::png() const {
    std::vector<uint8_t> bytes;
    auto append = [](void* ctx, void* data, int size) {
        auto& v = *static_cast<std::vector<uint8_t>*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        v.insert(v.end(), p, p + size);
    };
    if (!stbi_write_png_to_func(append, &bytes, width, height, 3, rgb.data(), width * 3))
        throw std::runtime_error("PNG encoding failed");
    return bytes;
}
std::string base64(const std::vector<uint8_t>& v) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((v.size() + 2) / 3 * 4);
    for (size_t i = 0; i < v.size(); i += 3) {
        uint32_t n = uint32_t(v[i]) << 16;
        if (i + 1 < v.size()) n |= uint32_t(v[i + 1]) << 8;
        if (i + 2 < v.size()) n |= v[i + 2];
        out += chars[(n >> 18) & 63]; out += chars[(n >> 12) & 63];
        out += i + 1 < v.size() ? chars[(n >> 6) & 63] : '=';
        out += i + 2 < v.size() ? chars[n & 63] : '=';
    }
    return out;
}
} // namespace omniocr
