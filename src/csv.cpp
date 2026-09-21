#include "omniocr/core.hpp"
#include <fstream>
#include <stdexcept>

namespace omniocr {
void csv_to_html(const fs::path& input, const fs::path& output, const Json& settings) {
    // CSV has no page geometry. Render literal UTF-8 cells as a paginated table;
    // do not evaluate formulas or infer numeric types (e.g. identifiers 00123).
    const auto limit = settings.value("max_csv_bytes", uint64_t(16777216));
    if (fs::file_size(input) > limit) throw std::runtime_error("CSV exceeds max_csv_bytes");
    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open CSV");
    std::string text;
    char buffer[8192];
    while (in.read(buffer, sizeof(buffer)) || in.gcount()) {
        if (text.size() + size_t(in.gcount()) > limit) throw std::runtime_error("CSV exceeds max_csv_bytes");
        text.append(buffer, size_t(in.gcount()));
    }
    if (!in.eof()) throw std::runtime_error("cannot read CSV");
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    if (text.empty()) throw std::runtime_error("empty CSV");
    // nlohmann's strict serializer validates UTF-8; never silently replace bytes.
    try { (void)Json(text).dump(); }
    catch (const Json::type_error&) { throw std::runtime_error("CSV must be UTF-8 (optional BOM)"); }
    for (unsigned char c : text)
        if (c < 32 && c != '\r' && c != '\n' && c != '\t')
            throw std::runtime_error("unsupported control character in CSV");
    const auto delimiter = settings.value("csv_delimiter", std::string(","));
    if (delimiter != "," && delimiter != ";" && delimiter != "\t" && delimiter != "|")
        throw std::runtime_error("invalid CSV delimiter");
    std::ofstream out(output, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create CSV rendering");
    out << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<style>@page { size: A4 landscape; margin: 12mm; }"
           "table { border-collapse: collapse; width: 100%; table-layout: fixed; }"
           "td { border: 1px solid #777; padding: 4px; white-space: pre-wrap; overflow-wrap: anywhere; }"
           "</style></head><body><table><tr><td>";
    enum class State { start, plain, quoted, closed } state = State::start;
    auto emit = [&](char c) {
        switch (c) {
        case '&': out << "&amp;"; break;
        case '<': out << "&lt;"; break;
        case '>': out << "&gt;"; break;
        case '"': out << "&quot;"; break;
        case '\n': out << "<br>"; break;
        default: out.put(c);
        }
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (state == State::quoted) {
            if (c == '"') state = State::closed;
            else if (c == '\r') {
                emit('\n'); if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            } else emit(c);
            continue;
        }
        if (state == State::closed && c == '"') { emit(c); state = State::quoted; continue; }
        if (c == delimiter[0]) { out << "</td><td>"; state = State::start; continue; }
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (i + 1 < text.size()) out << "</td></tr><tr><td>";
            state = State::start; continue;
        }
        if (c == '"' && state == State::start) { state = State::quoted; continue; }
        if (c == '"' || state == State::closed) throw std::runtime_error("malformed CSV quoting");
        emit(c); state = State::plain;
    }
    if (state == State::quoted) throw std::runtime_error("unterminated CSV quoted field");
    out << "</td></tr></table></body></html>";
    out.close();
    if (!out) throw std::runtime_error("cannot write CSV rendering");
}
} // namespace omniocr
