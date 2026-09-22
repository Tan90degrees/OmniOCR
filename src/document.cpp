#include "omniocr/core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <regex>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace omniocr {
TempDir::TempDir() {
    std::string pattern = (fs::temp_directory_path() / "omniocr-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("cannot create temporary directory");
    path = pattern;
}
TempDir::~TempDir() { std::error_code ec; fs::remove_all(path, ec); }

std::string run_process(const std::vector<std::string>& args, int timeout_seconds) {
    if (args.empty() || timeout_seconds <= 0) throw std::runtime_error("invalid process arguments");
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) throw std::runtime_error("pipe failed");
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(fds[1]);
    if (rc) { close(fds[0]); throw std::runtime_error("cannot start " + args[0] + ": " + std::strerror(rc)); }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    struct Cleanup {
        pid_t pid; int fd; bool reaped = false;
        ~Cleanup() {
            kill(-pid, SIGKILL);
            if (!reaped) { int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
            close(fd);
        }
    } cleanup{pid, fds[0]};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    std::string output;
    auto drain = [&] {
        char buf[4096]; ssize_t n;
        // Limit work per drain, even if the child writes continuously.
        for (int i = 0; i < 256 && (n = read(fds[0], buf, sizeof(buf))) > 0; ++i)
            if (output.size() < 1048576) output.append(buf, std::min(size_t(n), size_t(1048576) - output.size()));
    };
    int status = 0;
    while (true) {
        drain();
        auto done = waitpid(pid, &status, WNOHANG);
        if (done == pid) { cleanup.reaped = true; drain(); break; }
        if (done < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(args[0] + " timed out");
        pollfd fd{fds[0], POLLIN, 0}; poll(&fd, 1, 20);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error(args[0] + " failed: " + output.substr(0, 4096));
    return output;
}

const std::vector<std::string>& supported_input_extensions() {
    static const std::vector<std::string> extensions = {
        ".png", ".jpg", ".jpeg", ".bmp", ".ppm", ".pgm", ".tga", ".tif", ".tiff",
        ".pdf", ".doc", ".docx", ".ppt", ".pptx", ".xls", ".xlsx", ".rtf",
        ".odt", ".ods", ".odp", ".epub", ".ofd", ".html", ".htm", ".csv"
    };
    return extensions;
}
bool supports_input_extension(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto& extensions = supported_input_extensions();
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

void read_document(const fs::path& input, const Json& settings, const std::function<void(int, const Image&)>& consume) {
    const fs::path source = fs::absolute(input);
    if (!fs::is_regular_file(source)) throw std::runtime_error("input is not a regular file");
    std::string ext = source.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    uint64_t max_pixels = settings.value("max_pixels", uint64_t(40000000));
    if (std::set<std::string>{".png", ".jpg", ".jpeg", ".bmp", ".ppm", ".pgm", ".tga"}.count(ext)) {
        consume(1, Image::load(source, max_pixels)); return;
    }
    if (ext == ".tif" || ext == ".tiff") {
        read_tiff(source, settings, consume); return;
    }
    if (!supports_input_extension(ext)) throw std::runtime_error("unsupported input extension: " + ext);
    TempDir temp;
    fs::path pdf = source;
    int timeout = settings.value("timeout_seconds", 120);
    if (ext != ".pdf") {
        pdf = temp.path / "input.pdf";
        if (ext == ".epub" || ext == ".ofd") {
            const std::string key = ext == ".epub" ? "ebook_convert" : "ofd_converter";
            const std::string executable = settings.value(key,
                ext == ".epub" ? "ebook-convert" : "omniocr-ofd-to-pdf");
            try {
                run_process({executable, source.string(), pdf.string()}, timeout);
            } catch (const std::exception& e) {
                throw std::runtime_error(ext + " conversion failed (document." + key + "): " + e.what());
            }
        } else {
            fs::path local = temp.path / ("input" + ext);
            if (ext == ".csv") {
                local = temp.path / "input.html";
                csv_to_html(source, local, settings);
            } else if (ext == ".html" || ext == ".htm") {
                // Preserve relative images/styles for path-based HTML inputs.
                local = source;
                pdf = temp.path / (source.stem().string() + ".pdf");
            } else fs::copy_file(source, local);
            run_process({settings.value("soffice", "soffice"),
                         "-env:UserInstallation=file://" + (temp.path / "profile").string(),
                         "--headless", "--nologo", "--nodefault", "--norestore", "--convert-to", "pdf",
                         "--outdir", temp.path.string(), local.string()}, timeout);
        }
        // A successful exit is not proof that the converter produced a document.
        if (!fs::is_regular_file(pdf) || fs::file_size(pdf) == 0)
            throw std::runtime_error("conversion produced no PDF for " + ext +
                                     " (check converter, encryption, IRM, corruption and fonts)");
    }
    const auto info = run_process({settings.value("pdfinfo", "pdfinfo"), pdf.string()}, timeout);
    std::smatch match;
    if (!std::regex_search(info, match, std::regex(R"((?:^|\n)Pages:\s+(\d+))")))
        throw std::runtime_error("cannot determine PDF page count");
    const int count = std::stoi(match[1]);
    if (count <= 0 || count > settings.value("max_pages", 1000)) throw std::runtime_error("PDF page count exceeds limit");
    const auto sizes = run_process({settings.value("pdfinfo", "pdfinfo"), "-f", "1", "-l", std::to_string(count), pdf.string()}, timeout);
    const std::regex size_pattern(R"(Page\s+(\d+)\s+size:\s+([0-9.]+)\s+x\s+([0-9.]+)\s+pts)");
    std::map<int, double> longest_edges;
    for (std::sregex_iterator it(sizes.begin(), sizes.end(), size_pattern), end; it != end; ++it)
        longest_edges[std::stoi((*it)[1])] = std::max(std::stod((*it)[2]), std::stod((*it)[3]));
    for (int page = 1; page <= count; ++page) {
        const auto prefix = temp.path / "page";
        if (!longest_edges.count(page) || longest_edges.at(page) <= 0)
            throw std::runtime_error("cannot determine PDF page dimensions");
        const int dpi = settings.value("dpi", 150);
        const int max_edge = int(std::sqrt(double(max_pixels)));
        std::vector<std::string> args{settings.value("pdftoppm", "pdftoppm"), "-f", std::to_string(page), "-l", std::to_string(page),
                                      "-singlefile", "-r", std::to_string(dpi)};
        // -scale-to always rescales, including upscaling: use it only above the cap.
        if (longest_edges.at(page) * dpi / 72.0 > max_edge) {
            args.push_back("-scale-to"); args.push_back(std::to_string(max_edge));
        }
        args.push_back(pdf.string()); args.push_back(prefix.string());
        run_process(args, timeout);
        consume(page, Image::load(prefix.string() + ".ppm", max_pixels));
        fs::remove(prefix.string() + ".ppm");
    }
}
} // namespace omniocr
