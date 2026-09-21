#include "omniocr/core.hpp"
#include <tiffio.h>
#include <algorithm>
#include <stdexcept>

namespace omniocr {
void read_tiff(const fs::path& input, const Json& settings,
               const std::function<void(int, const Image&)>& consume) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(input.c_str(), "r"), TIFFClose);
    if (!file) throw std::runtime_error("cannot open TIFF");
    const auto max_pixels = settings.value("max_pixels", uint64_t(40000000));
    const int max_pages = settings.value("max_pages", 1000);
    int pages = 0;
    // Validate all primary image directories before emitting pages; never truncate.
    while (true) {
        if (++pages > max_pages) throw std::runtime_error("TIFF page count exceeds limit");
        uint32_t w = 0, h = 0;
        if (!TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &w) ||
            !TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &h) || !w || !h ||
            uint64_t(w) * h > max_pixels || w > INT32_MAX || h > INT32_MAX)
            throw std::runtime_error("invalid TIFF dimensions or image exceeds max_pixels");
        if (TIFFLastDirectory(file.get())) break;
        if (!TIFFReadDirectory(file.get())) throw std::runtime_error("invalid TIFF page directory");
    }
    if (!TIFFSetDirectory(file.get(), 0)) throw std::runtime_error("cannot rewind TIFF");
    for (int page = 1; page <= pages; ++page) {
        uint32_t w = 0, h = 0;
        TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &h);
        std::vector<uint32_t> rgba(size_t(w) * h);
        if (!TIFFReadRGBAImageOriented(file.get(), w, h, rgba.data(), ORIENTATION_TOPLEFT, 1))
            throw std::runtime_error("TIFF page decoding failed");
        Image image{int(w), int(h), std::vector<uint8_t>(size_t(w) * h * 3)};
        for (size_t i = 0; i < rgba.size(); ++i) {
            // libtiff returns associated (premultiplied) alpha. Composite on white.
            unsigned a = TIFFGetA(rgba[i]);
            image.rgb[i * 3] = uint8_t(std::min(255u, unsigned(TIFFGetR(rgba[i])) + 255 - a));
            image.rgb[i * 3 + 1] = uint8_t(std::min(255u, unsigned(TIFFGetG(rgba[i])) + 255 - a));
            image.rgb[i * 3 + 2] = uint8_t(std::min(255u, unsigned(TIFFGetB(rgba[i])) + 255 - a));
        }
        consume(page, image);
        if (page < pages && !TIFFReadDirectory(file.get())) throw std::runtime_error("missing TIFF page");
    }
}
} // namespace omniocr
