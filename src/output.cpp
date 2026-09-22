#include "omniocr/core.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omniocr {
Json document_json(const Document& doc) {
    Json out = {{"schema_version", 1}, {"source", doc.source}, {"coordinate_system", "page_pixels_xyxy"}, {"pages", Json::array()}};
    for (const auto& page : doc.pages) {
        Json p = {{"page", page.number}, {"width", page.width}, {"height", page.height}, {"blocks", Json::array()}};
        for (size_t i = 0; i < page.regions.size(); ++i) {
            const auto& r = page.regions[i];
            p["blocks"].push_back({{"id", "p" + std::to_string(page.number) + "-b" + std::to_string(i)},
                {"type", r.box.type}, {"raw_type", r.box.raw_type}, {"bbox", r.box.bbox},
                {"score", r.box.score}, {"order", r.box.order}, {"rotation", r.box.rotation},
                {"model", r.model}, {"text", r.text}, {"raw_text", r.raw_text}, {"asset", r.asset}, {"error", r.error}});
        }
        out["pages"].push_back(std::move(p));
    }
    return out;
}
std::string document_markdown(const Document& doc) {
    std::ostringstream out;
    for (const auto& page : doc.pages) {
        out << "<!-- page: " << page.number << " -->\n\n";
        for (const auto& r : page.regions) {
            if (!r.error.empty()) { out << "**[OCR block failed]**\n\n"; continue; }
            if (!r.asset.empty()) out << "![document region](" << r.asset << ")\n\n";
            if (r.text.empty()) continue;
            if (r.box.type == "title") out << "# " << r.text;
            else if (r.box.type == "heading") out << "## " << r.text;
            else if (r.box.type == "formula" || r.box.type == "equation") {
                if (r.text.rfind("$$", 0) == 0 || r.text.rfind("\\[", 0) == 0) out << r.text;
                else out << "$$\n" << r.text << "\n$$";
            } else out << r.text;
            out << "\n\n";
        }
    }
    return out.str();
}
void write_outputs(const Document& doc, const fs::path& dir, const std::string& format) {
    if (format != "json" && format != "markdown" && format != "both") throw std::runtime_error("format must be json, markdown or both");
    fs::create_directories(dir);
    auto write = [&](const char* name, const std::string& content) {
        const fs::path path = dir / name, tmp = path.string() + ".tmp";
        { std::ofstream out(tmp); out << content; out.close(); if (!out) throw std::runtime_error("cannot write output"); }
        fs::rename(tmp, path);
    };
    if (format != "markdown") write("result.json", document_json(doc).dump(2));
    if (format != "json") write("result.md", document_markdown(doc));
}
} // namespace omniocr
