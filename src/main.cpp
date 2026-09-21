#include "omniocr/core.hpp"
#include <iostream>
#include <map>
#include <fstream>
#include <cstdint>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "omniocr --config CONFIG --input FILE --output EMPTY_DIR [--format both|json|markdown]\n"
                         "omniocr --config CONFIG --batch JOBS.json [--format both|json|markdown]\n"
                         "omniocr --config CONFIG --validate\n"; return 0;
        }
        std::map<std::string, std::string> args;
        bool validate = false;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--validate") { validate = true; continue; }
            if (key != "--config" && key != "--input" && key != "--output" && key != "--format" && key != "--batch") throw std::runtime_error("unknown argument " + key);
            if (++i >= argc || args.count(key)) throw std::runtime_error("missing or duplicate argument " + key);
            args[key] = argv[i];
        }
        if (!args.count("--config")) throw std::runtime_error("--config is required (see --help)");
        auto config = omniocr::load_config(args.at("--config"));
        if (validate) { std::cout << "Configuration valid\n"; return 0; }
        if (!args.count("--batch") && (!args.count("--input") || !args.count("--output"))) throw std::runtime_error("--input and --output are required");
        const auto format = args.count("--format") ? args.at("--format") : "both";
        if (format != "json" && format != "markdown" && format != "both") throw std::runtime_error("invalid --format");
        if (args.count("--batch")) {
            if (args.count("--input") || args.count("--output"))
                throw std::runtime_error("--batch cannot be combined with --input/--output");
            const omniocr::fs::path manifest_path = omniocr::fs::absolute(args.at("--batch"));
            std::ifstream file(manifest_path);
            if (!file) throw std::runtime_error("cannot open batch manifest: " + manifest_path.string());
            omniocr::Json manifest; file >> manifest;
            const auto& list = manifest.is_array() ? manifest : manifest.at("jobs");
            if (!list.is_array() || list.empty())
                throw std::runtime_error("batch manifest jobs must be a nonempty array");
            auto integer = [](const omniocr::Json& object, const char* key, int fallback,
                              int minimum, int maximum) -> int {
                if (!object.contains(key)) return fallback;
                const auto& v = object.at(key);
                if (!v.is_number_integer() && !v.is_number_unsigned())
                    throw std::runtime_error(std::string("batch ") + key + " must be an integer");
                if (v.is_number_unsigned()) {
                    const uint64_t n = v.get<uint64_t>();
                    if (n > uint64_t(maximum))
                        throw std::runtime_error(std::string("batch ") + key + " out of range");
                    return int(n);
                }
                const int64_t n = v.get<int64_t>();
                if (n < minimum || n > maximum)
                    throw std::runtime_error(std::string("batch ") + key + " out of range");
                return int(n);
            };
            omniocr::BatchOptions options;
            if (manifest.is_object() && manifest.contains("options")) {
                const auto& values = manifest.at("options");
                if (!values.is_object()) throw std::runtime_error("batch options must be an object");
                options.page_workers = integer(values, "page_workers", options.page_workers, 1, 128);
                options.max_active_documents = integer(values, "max_active_documents",
                                                       options.max_active_documents, 1, 32);
                options.max_queued_pages = integer(values, "max_queued_pages",
                                                   options.max_queued_pages, 1, 256);
            }
            std::vector<omniocr::BatchJob> jobs;
            for (const auto& item : list) {
                const auto base = manifest_path.parent_path();
                const auto input = base / item.at("input").get<std::string>();
                const auto output = base / item.at("output").get<std::string>();
                jobs.push_back({input, output, integer(item, "priority", 0, -1000000, 1000000)});
            }
            omniocr::Pipeline pipeline(std::move(config));
            const auto results = pipeline.run_batch(jobs, options);
            bool fatal = false, partial = false;
            for (size_t i = 0; i < results.size(); ++i) {
                if (!results[i].error.empty()) {
                    std::cerr << "Failed " << jobs[i].input << ": " << results[i].error << '\n';
                    fatal = true; continue;
                }
                try {
                    omniocr::write_outputs(results[i].document, jobs[i].output_dir, format);
                } catch (const std::exception& e) {
                    std::cerr << "Failed output " << jobs[i].input << ": " << e.what() << '\n';
                    fatal = true; continue;
                }
                size_t errors = 0;
                for (const auto& page : results[i].document.pages)
                    for (const auto& block : page.regions)
                        if (!block.error.empty()) ++errors;
                partial |= errors != 0;
                std::cout << "Processed " << jobs[i].input << ": " << results[i].document.pages.size()
                          << " pages, " << errors << " failed blocks\n";
            }
            return fatal ? 1 : partial ? 2 : 0;
        }
        omniocr::Pipeline pipeline(std::move(config));
        auto document = pipeline.run(args.at("--input"), args.at("--output"));
        omniocr::write_outputs(document, args.at("--output"), format);
        size_t errors = 0;
        for (const auto& page : document.pages) for (const auto& region : page.regions) if (!region.error.empty()) ++errors;
        std::cout << "Processed " << document.pages.size() << " pages, " << errors << " failed blocks\n";
        return errors ? 2 : 0;
    } catch (const std::exception& e) { std::cerr << "omniocr: " << e.what() << '\n'; return 1; }
}
