#include "omniocr/core.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omniocr {
Json document_json(const Document& doc) {
    bool schema_v2 = false;
    for (const auto& page : doc.pages) for (const auto& region : page.regions)
        if (!region.box.polygon.empty() || !region.box.reading_order ||
            region.box.provenance.value("adapter", std::string{}) == "paddle.doclayout_v3.http")
            schema_v2=true;
    Json out = {{"schema_version", schema_v2 ? 2 : 1},
                {"source", doc.source}, {"coordinate_system", "page_pixels_xyxy"},
                {"pages", Json::array()}};
    for (const auto& page : doc.pages) {
        Json p = {{"page", page.number}, {"width", page.width},
                  {"height", page.height}, {"blocks", Json::array()}};
        for (size_t i=0; i<page.regions.size(); ++i) {
            const auto& r=page.regions[i];
            Json block={{"id", schema_v2
                    ? "p"+std::to_string(page.number)+"-s"+std::to_string(r.box.source_index)
                    : "p"+std::to_string(page.number)+"-b"+std::to_string(i)},
                {"type",r.box.type},{"raw_type",r.box.raw_type},{"bbox",r.box.bbox},
                {"score",r.box.score},{"order",r.box.order},{"rotation",r.box.rotation},
                {"model",r.model},{"text",r.text},{"raw_text",r.raw_text},
                {"asset",r.asset},{"error",r.error}};
            if (schema_v2) {
                block["polygon"]=r.box.polygon;
                block["reading_order"]=r.box.reading_order ? Json(*r.box.reading_order):Json(nullptr);
                block["source_index"]=r.box.source_index;
                block["provenance"]=r.box.provenance;
                block["extensions"]=r.box.extensions;
                if (!r.box.reading_order) block["order"]=nullptr;
            }
            p["blocks"].push_back(std::move(block));
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
            // V3's explicit order:null means the region does not participate
            // in the body text sequence; it remains available in result JSON.
            if (!r.box.reading_order) continue;
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
