#include "omniocr/core.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace omniocr {
namespace {
using Rect = std::array<double, 4>;
double area(const Rect& b) { return std::max(0., b[2]-b[0])*std::max(0., b[3]-b[1]); }
double intersection(const Rect& a, const Rect& b) {
    return std::max(0., std::min(a[2],b[2])-std::max(a[0],b[0])) *
           std::max(0., std::min(a[3],b[3])-std::max(a[1],b[1]));
}
double iou(const Rect& a, const Rect& b) {
    const double common=intersection(a,b), total=area(a)+area(b)-common;
    return total>0 ? common/total : 0;
}
bool contains(const Box& outer, const Box& inner, double ratio) {
    return area(inner.bbox)>0 && area(outer.bbox)>area(inner.bbox)*1.05 &&
           intersection(outer.bbox,inner.bbox)/area(inner.bbox)>=ratio;
}
bool enabled(const Json& c, const char* key) {
    return c.contains(key) && c.at(key).value("enabled",false);
}
std::set<std::string> types(const Json& config, const char* key, std::vector<std::string> defaults) {
    const auto list=config.value(key,defaults);
    return {list.begin(),list.end()};
}
bool ordered(const Box& a, const Box& b) {
    if (bool(a.reading_order)!=bool(b.reading_order)) return bool(a.reading_order);
    if (a.reading_order && b.reading_order && *a.reading_order!=*b.reading_order)
        return *a.reading_order<*b.reading_order;
    return a.source_index<b.source_index;
}
int priority(const Box& b, const std::vector<std::string>& list) {
    const auto it=std::find(list.begin(),list.end(),b.type);
    return it==list.end() ? 0 : int(list.size()-(it-list.begin()));
}
bool wins(const Box& a, const Box& b, double margin, const std::vector<std::string>& list) {
    if (std::abs(a.score-b.score)+1e-9>=margin && a.score!=b.score) return a.score>b.score;
    if (priority(a,list)!=priority(b,list)) return priority(a,list)>priority(b,list);
    if (a.score!=b.score) return a.score>b.score;
    return a.source_index<b.source_index;
}
std::vector<Box> deduplicate(std::vector<Box> boxes, const Json& c) {
    if (!enabled(c,"overlap")) return boxes;
    const auto& settings=c.at("overlap");
    const auto list=settings.value("label_priority",std::vector<std::string>{
        "table","chart","figure","title","heading","formula","equation",
        "text","header","page_header","footer","page_footer"});
    const double threshold=settings.value("iou_threshold",0.5);
    const double margin=settings.value("score_margin",0.3);
    std::vector<bool> removed(boxes.size(),false);
    for (size_t i=0;i<boxes.size();++i) {
        if (removed[i]) continue;
        for (size_t j=i+1;j<boxes.size();++j) {
            auto related=[](const Box& parent, const Box& child) {
                const auto it=parent.extensions.find("omniocr.provisional_children");
                if (it==parent.extensions.end()) return false;
                return std::find(it->begin(),it->end(),child.source_index)!=it->end();
            };
            if (related(boxes[i],boxes[j]) || related(boxes[j],boxes[i])) continue;
            if (removed[j] || iou(boxes[i].bbox,boxes[j].bbox)<threshold) continue;
            if (wins(boxes[i],boxes[j],margin,list)) removed[j]=true;
            else { removed[i]=true; break; }
        }
    }
    std::vector<Box> result;
    for (size_t i=0;i<boxes.size();++i) if (!removed[i]) result.push_back(std::move(boxes[i]));
    return result;
}
struct HtmlTable {
    std::string open, close;
    std::vector<std::string> rows;
};
std::optional<HtmlTable> table_html(const std::string& text) {
    static const std::regex whole(R"(^\s*(<table\b[^>]*>)([\s\S]*)(</table>)\s*$)",std::regex::icase);
    static const std::regex row(R"(<tr\b[^>]*>[\s\S]*?</tr>)",std::regex::icase);
    static const std::regex nested(R"(<table\b)",std::regex::icase);
    std::smatch match;
    if (!std::regex_match(text,match,whole) ||
        std::distance(std::sregex_iterator(text.begin(),text.end(),nested),std::sregex_iterator())!=1)
        return std::nullopt;
    HtmlTable result{match[1].str(),match[3].str(),{}};
    const std::string body=match[2].str();
    size_t end_of_last=0;
    auto whitespace=[](const std::string& part) {
        return std::all_of(part.begin(),part.end(),[](unsigned char c){return std::isspace(c)!=0;});
    };
    for (std::sregex_iterator it(body.begin(),body.end(),row),end;it!=end;++it) {
        if (!whitespace(body.substr(end_of_last,size_t(it->position())-end_of_last))) return std::nullopt;
        result.rows.push_back(it->str());
        end_of_last=size_t(it->position()+it->length());
    }
    if (result.rows.empty() || !whitespace(body.substr(end_of_last))) return std::nullopt;
    return result;
}
size_t columns(const std::string& row) {
    static const std::regex cell(R"(<t[dh]\b)",std::regex::icase);
    return size_t(std::distance(std::sregex_iterator(row.begin(),row.end(),cell),std::sregex_iterator()));
}
std::string compact(const std::string& value) {
    std::string result;
    for (unsigned char c:value) if (!std::isspace(c)) result+=char(std::tolower(c));
    return result;
}
Region* edge_region(Page& page, bool last) {
    if (last) {
        for (auto it=page.regions.rbegin();it!=page.regions.rend();++it)
            if (it->box.reading_order && (!it->text.empty() || !it->asset.empty() ||
                it->box.extensions.contains("omniocr.merged_into")))
                return &*it;
    } else {
        for (auto& region:page.regions)
            if (region.box.reading_order && (!region.text.empty() || !region.asset.empty()))
                return &region;
    }
    return nullptr;
}
} // namespace

std::vector<Box> postprocess_boxes(std::vector<Box> boxes, const Json& config, const Image& image,
    const std::function<std::vector<Box>(const Image&)>& redetect) {
    if (!config.is_object()) throw std::runtime_error("postprocess must be an object");
    if (enabled(config,"low_score")) {
        const double threshold=config.at("low_score").value("threshold",0.3);
        boxes.erase(std::remove_if(boxes.begin(),boxes.end(),[&](const Box& b){return b.score<threshold;}),boxes.end());
    }
    if (enabled(config,"header_footer")) {
        const auto ignored=types(config.at("header_footer"),"types",
            {"header","footer","page_header","page_footer","ignored_header","ignored_footer"});
        boxes.erase(std::remove_if(boxes.begin(),boxes.end(),[&](const Box& b){return ignored.count(b.type)>0;}),boxes.end());
    }
    if (enabled(config,"composite")) {
        const auto& c=config.at("composite");
        const auto outer_types=types(c,"outer_types",{"chart","figure","table"});
        const auto expand_types=types(c,"expand_inner_types",{"table","formula","equation","title","heading"});
        const double containment=c.value("containment_threshold",0.9);
        const double retain_score=c.value("retain_min_score",0.7);
        const bool inspect_text=c.value("inspect_inner_text",false);
        const size_t original=boxes.size();
        // A child belongs to its smallest enclosing parent. This produces a
        // forest even with nested tables/figures and preserves detector order.
        std::vector<size_t> parent(original,original);
        for (size_t child=0;child<original;++child) {
            double smallest=std::numeric_limits<double>::infinity();
            for (size_t outer=0;outer<original;++outer) {
                if (outer==child || !contains(boxes[outer],boxes[child],containment)) continue;
                if (area(boxes[outer].bbox)<smallest) {parent[child]=outer;smallest=area(boxes[outer].bbox);}
            }
        }
        std::vector<size_t> children_count(original,0);
        for (size_t child=0;child<original;++child)
            if (parent[child]!=original) ++children_count[parent[child]];
        std::vector<Rect> masked;
        for (size_t i=0;i<original;++i)
            if (outer_types.count(boxes[i].type) && children_count[i])
                masked.push_back(boxes[i].bbox);
        if (redetect && !masked.empty()) {
            Image white=image;
            for (const auto& b:masked) {
                const int x0=std::clamp(int(std::floor(b[0])),0,image.width);
                const int x1=std::clamp(int(std::ceil(b[2])),0,image.width);
                const int y0=std::clamp(int(std::floor(b[1])),0,image.height);
                const int y1=std::clamp(int(std::ceil(b[3])),0,image.height);
                for (int y=y0;y<y1;++y)
                    std::fill(white.rgb.begin()+(size_t(y)*image.width+x0)*3,
                              white.rgb.begin()+(size_t(y)*image.width+x1)*3,uint8_t(255));
            }
            size_t next_source=0;
            for (const auto& b:boxes) next_source=std::max(next_source,b.source_index+1);
            auto recovered=redetect(white);
            const size_t max_extra=c.value("max_recovered_boxes",size_t(100));
            const auto ignored=enabled(config,"header_footer") ?
                types(config.at("header_footer"),"types",
                    {"header","footer","page_header","page_footer","ignored_header","ignored_footer"}) :
                std::set<std::string>{};
            const double minimum=enabled(config,"low_score") ?
                config.at("low_score").value("threshold",0.3) : 0;
            size_t added=0;
            for (auto& b:recovered) {
                if (added>=max_extra) break;
                if (b.score<minimum || ignored.count(b.type) || area(b.bbox)<=0) continue;
                bool occluded=false;
                for (const auto& mask:masked)
                    if (intersection(b.bbox,mask)/area(b.bbox)>=0.2) {occluded=true;break;}
                if (occluded) continue;
                b.source_index=next_source++;
                b.provenance["omniocr.pass"]=2;
                b.extensions["omniocr.recovered"]=true;
                boxes.push_back(std::move(b)); ++added;
            }
        }
        std::vector<bool> semantic_descendant(original,false);
        for (size_t child=0;child<original;++child) {
            if (!expand_types.count(boxes[child].type)) continue;
            for (size_t ancestor=parent[child];ancestor!=original;ancestor=parent[ancestor])
                semantic_descendant[ancestor]=true;
        }
        std::vector<bool> keep(original,true),retain(original,false);
        for (size_t i=0;i<original;++i) {
            if (!outer_types.count(boxes[i].type) || !children_count[i]) continue;
            retain[i]=boxes[i].score>=retain_score && !semantic_descendant[i];
            if (!retain[i]) {
                keep[i]=false;
                for (size_t child=0;child<original;++child)
                    if (parent[child]==i)
                        boxes[child].extensions["omniocr.parent_source_index"]=boxes[i].source_index;
            }
        }
        for (size_t child=0;child<original;++child)
            for (size_t ancestor=parent[child];ancestor!=original;ancestor=parent[ancestor])
                if (retain[ancestor]) {
                    if (!inspect_text) keep[child]=false;
                    boxes[ancestor].extensions[inspect_text ? "omniocr.provisional_children" :
                        "omniocr.composite_children"].push_back(boxes[child].source_index);
                    break;
                }
        std::vector<Box> chosen;
        for (size_t i=0;i<boxes.size();++i)
            if (i>=original || keep[i]) chosen.push_back(std::move(boxes[i]));
        boxes=std::move(chosen);
    }
    boxes=deduplicate(std::move(boxes),config);
    std::stable_sort(boxes.begin(),boxes.end(),ordered);
    return boxes;
}

void finalize_composite_page(Page& page, const Json& config, const fs::path& output_dir) {
    if (!enabled(config,"composite") || !config.at("composite").value("inspect_inner_text",false)) return;
    const auto& settings=config.at("composite");
    const bool uncovered=settings.value("expand_if_uncovered_text",true);
    const size_t minimum=settings.value("min_inner_text_chars",size_t(4));
    std::vector<size_t> parents;
    for (size_t i=0;i<page.regions.size();++i)
        if (page.regions[i].box.extensions.contains("omniocr.provisional_children")) parents.push_back(i);
    std::stable_sort(parents.begin(),parents.end(),[&](size_t a,size_t b) {
        return area(page.regions[a].box.bbox)>area(page.regions[b].box.bbox);
    });
    std::vector<bool> removed(page.regions.size(),false);
    for (size_t parent:parents) {
        if (removed[parent]) continue;
        auto& outer=page.regions[parent];
        const auto children=outer.box.extensions.at("omniocr.provisional_children");
        std::vector<size_t> indices;
        for (size_t i=0;i<page.regions.size();++i)
            if (i!=parent && !removed[i] &&
                std::find(children.begin(),children.end(),page.regions[i].box.source_index)!=children.end())
                indices.push_back(i);
        bool expand=false;
        for (size_t i:indices) {
            const auto& inner=page.regions[i];
            if (!inner.error.empty() || inner.text.size()<minimum) continue;
            if (outer.text.empty() || !outer.error.empty() ||
                (uncovered && compact(outer.text).find(compact(inner.text))==std::string::npos)) {
                expand=true;break;
            }
        }
        outer.box.extensions.erase("omniocr.provisional_children");
        if (expand) {
            removed[parent]=true;
            for (size_t i:indices)
                page.regions[i].box.extensions["omniocr.parent_source_index"]=outer.box.source_index;
        } else {
            for (size_t i:indices) {
                removed[i]=true;
                outer.box.extensions["omniocr.composite_children"].push_back(
                    page.regions[i].box.source_index);
            }
        }
    }
    std::vector<Region> selected;
    selected.reserve(page.regions.size());
    for (size_t i=0;i<page.regions.size();++i) {
        if (!removed[i]) {selected.push_back(std::move(page.regions[i]));continue;}
        if (!page.regions[i].asset.empty()) {
            const auto& asset=page.regions[i].asset;
            if (asset.rfind("assets/page-",0)==0 && asset.find("..") == std::string::npos) {
                std::error_code error;
                fs::remove(output_dir/asset,error);
            }
        }
    }
    page.regions=std::move(selected);
}

void postprocess_document(Document& doc, const Json& config) {
    if (!enabled(config,"cross_page_tables")) return;
    const auto& c=config.at("cross_page_tables");
    const double margin=c.value("edge_margin_ratio",0.1);
    const double alignment=c.value("x_overlap_threshold",0.75);
    const bool match_header=c.value("require_header_match",false);
    for (size_t i=1;i<doc.pages.size();++i) {
        auto& previous=doc.pages[i-1]; auto& current=doc.pages[i];
        if (current.number!=previous.number+1 || previous.height<=0 || current.height<=0 ||
            previous.width<=0 || current.width<=0) continue;
        Region* a=edge_region(previous,true), *b=edge_region(current,false);
        if (!a || !b || a->box.type!="table" || b->box.type!="table" ||
            !a->error.empty() || !b->error.empty() || !a->asset.empty() || !b->asset.empty() ||
            a->box.bbox[3]/previous.height<1-margin || b->box.bbox[1]/current.height>margin) continue;
        const double ax0=a->box.bbox[0]/previous.width,ax1=a->box.bbox[2]/previous.width;
        const double bx0=b->box.bbox[0]/current.width,bx1=b->box.bbox[2]/current.width;
        const double overlap=std::max(0.,std::min(ax1,bx1)-std::max(ax0,bx0));
        if (overlap/std::max(ax1-ax0,bx1-bx0)<alignment) continue;
        Region* root=a;
        int root_page=previous.number;
        if (a->box.extensions.contains("omniocr.merged_into")) {
            const auto& link=a->box.extensions.at("omniocr.merged_into");
            root_page=link.at("page").get<int>();
            root=nullptr;
            for (auto& page:doc.pages) {
                if (page.number!=root_page) continue;
                for (auto& region:page.regions)
                    if (region.box.source_index==link.at("source_index").get<size_t>()) root=&region;
            }
            if (!root) continue;
        }
        auto first=table_html(root->text), second=table_html(b->text);
        if (!first || !second || columns(first->rows.back())==0 ||
            columns(first->rows.back())!=columns(second->rows.front())) continue;
        const bool repeated=second->rows.size()>1 &&
            compact(first->rows.front())==compact(second->rows.front());
        if (match_header && !repeated) continue;
        // Only merge simple HTML tables with consistent cell counts. Colspan,
        // rowspan and nested tables need a structural table model instead.
        const size_t count=columns(first->rows.back());
        auto simple=[&](const HtmlTable& table) {
            for (const auto& row:table.rows)
                if (columns(row)!=count || compact(row).find("colspan")!=std::string::npos ||
                    compact(row).find("rowspan")!=std::string::npos) return false;
            return true;
        };
        if (!simple(*first) || !simple(*second)) continue;
        std::string merged=first->open+"\n";
        for (const auto& row:first->rows) merged+=row+"\n";
        for (size_t row=repeated ? 1 : 0;row<second->rows.size();++row) merged+=second->rows[row]+"\n";
        merged+=first->close;
        root->text=std::move(merged);
        root->box.extensions["omniocr.merged_pages"].push_back(current.number);
        b->box.extensions["omniocr.merged_into"]={{"page",root_page},
                                                    {"source_index",root->box.source_index}};
        b->text.clear();
    }
}
} // namespace omniocr
