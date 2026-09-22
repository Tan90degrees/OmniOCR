#include "omniocr/core.hpp"
#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace omniocr {
std::string table_to_html(const std::string& text) {
    if (text.find("<fcel>") == std::string::npos && text.find("<ecel>") == std::string::npos) return text;
    struct Cell { size_t row, col, bottom, right; std::string text; };
    struct Token { std::string type, text; };
    std::vector<Token> tokens;
    const std::regex pattern(R"(<(fcel|ecel|lcel|ucel|xcel|nl)>)");
    size_t previous_end = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        if (!tokens.empty()) tokens.back().text = text.substr(previous_end, size_t(it->position()) - previous_end);
        tokens.push_back({(*it)[1].str(), ""});
        previous_end = size_t(it->position() + it->length());
    }
    if (!tokens.empty()) tokens.back().text = text.substr(previous_end);
    std::vector<Cell> cells;
    std::vector<std::vector<size_t>> grid(1);
    for (const auto& t : tokens) {
        if (t.type == "nl") {
            if (grid.back().empty()) throw std::runtime_error("OTSL has empty row");
            grid.emplace_back(); continue;
        }
        const size_t row = grid.size() - 1, col = grid.back().size();
        size_t owner;
        if (t.type == "fcel" || t.type == "ecel") {
            owner = cells.size();
            cells.push_back({row, col, row, col, t.type == "fcel" ? t.text : ""});
        } else if (t.type == "lcel") {
            if (!col) throw std::runtime_error("OTSL lcel has no left cell");
            owner = grid[row][col - 1];
            if (cells[owner].row != row) throw std::runtime_error("invalid OTSL horizontal span");
        } else if (t.type == "ucel") {
            if (!row || col >= grid[row - 1].size()) throw std::runtime_error("OTSL ucel has no upper cell");
            owner = grid[row - 1][col];
            if (cells[owner].col != col) throw std::runtime_error("invalid OTSL vertical span");
        } else {
            if (!row || !col || col >= grid[row - 1].size() || grid[row][col - 1] != grid[row - 1][col])
                throw std::runtime_error("invalid OTSL combined span");
            owner = grid[row - 1][col];
        }
        grid.back().push_back(owner);
        cells[owner].bottom = std::max(cells[owner].bottom, row);
        cells[owner].right = std::max(cells[owner].right, col);
    }
    if (grid.back().empty()) grid.pop_back();
    if (grid.empty()) throw std::runtime_error("empty OTSL table");
    for (const auto& row : grid) if (row.size() != grid[0].size()) throw std::runtime_error("ragged OTSL table");
    // All merged cells must occupy rectangles, not L-shaped or overlapping regions.
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& c = cells[i];
        for (size_t r = c.row; r <= c.bottom; ++r) for (size_t col = c.col; col <= c.right; ++col)
            if (grid[r][col] != i) throw std::runtime_error("non-rectangular OTSL span");
    }
    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '\n') out += "<br/>";
            else out += c;
        }
        return out;
    };
    std::ostringstream html; html << "<table>\n";
    for (size_t row = 0; row < grid.size(); ++row) {
        html << "<tr>";
        for (size_t col = 0; col < grid[row].size(); ++col) {
            const auto& c = cells[grid[row][col]];
            if (c.row != row || c.col != col) continue;
            html << "<td";
            if (c.bottom > row) html << " rowspan=\"" << c.bottom - row + 1 << "\"";
            if (c.right > col) html << " colspan=\"" << c.right - col + 1 << "\"";
            html << ">" << escape(c.text) << "</td>";
        }
        html << "</tr>\n";
    }
    html << "</table>";
    return html.str();
}
} // namespace omniocr
