#include "omniocr/core.hpp"
#include <iostream>
#include <map>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "omniocr --config CONFIG --input FILE --output EMPTY_DIR [--format both|json|markdown]\n"
                         "omniocr --config CONFIG --validate\n"; return 0;
        }
        std::map<std::string, std::string> args;
        bool validate = false;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--validate") { validate = true; continue; }
            if (key != "--config" && key != "--input" && key != "--output" && key != "--format") throw std::runtime_error("unknown argument " + key);
            if (++i >= argc || args.count(key)) throw std::runtime_error("missing or duplicate argument " + key);
            args[key] = argv[i];
        }
        if (!args.count("--config")) throw std::runtime_error("--config is required (see --help)");
        auto config = omniocr::load_config(args.at("--config"));
        if (validate) { std::cout << "Configuration valid\n"; return 0; }
        if (!args.count("--input") || !args.count("--output")) throw std::runtime_error("--input and --output are required");
        const auto format = args.count("--format") ? args.at("--format") : "both";
        if (format != "json" && format != "markdown" && format != "both") throw std::runtime_error("invalid --format");
        omniocr::Pipeline pipeline(std::move(config));
        auto document = pipeline.run(args.at("--input"), args.at("--output"));
        omniocr::write_outputs(document, args.at("--output"), format);
        size_t errors = 0;
        for (const auto& page : document.pages) for (const auto& region : page.regions) if (!region.error.empty()) ++errors;
        std::cout << "Processed " << document.pages.size() << " pages, " << errors << " failed blocks\n";
        return errors ? 2 : 0;
    } catch (const std::exception& e) { std::cerr << "omniocr: " << e.what() << '\n'; return 1; }
}
