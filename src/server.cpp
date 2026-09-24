#include "omniocr/core.hpp"
#include "box_pool.hpp"
#include <microhttpd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <queue>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <cctype>
#include <iterator>

namespace omniocr {
namespace {
struct Options {
    fs::path config, data_dir, allowed_root;
    std::string host = "127.0.0.1", api_key;
    unsigned short port = 8080;
    size_t max_upload_bytes = 64 * 1024 * 1024, max_jobs = 1000;
    size_t max_active_jobs = 64, max_inflight_upload_bytes = 256 * 1024 * 1024;
    size_t max_queued_page_bytes = 256 * 1024 * 1024;
    int page_workers = 4, box_workers = 1, document_workers = 2, max_queued_pages = 2;
    unsigned http_connections = 128;
};
bool below(const fs::path& root, const fs::path& path) {
    auto a = root.begin(), b = path.begin();
    for (; a != root.end() && b != path.end(); ++a, ++b)
        if (*a != *b) return false;
    return a == root.end();
}
std::string new_id() {
    // Opaque job identifiers, not an authorization mechanism.
    std::random_device random;
    std::ostringstream stream;
    for (int i = 0; i < 4; ++i)
        stream << std::hex << std::setfill('0') << std::setw(8) << uint32_t(random());
    return stream.str();
}
int number(const Json& item, const char* key, int fallback, int lower, int upper) {
    if (!item.contains(key)) return fallback;
    const auto& value = item.at(key);
    if (!value.is_number_integer() && !value.is_number_unsigned())
        throw std::runtime_error(std::string(key) + " must be an integer");
    if (value.is_number_unsigned()) {
        const uint64_t n = value.get<uint64_t>();
        if (n > uint64_t(upper)) throw std::runtime_error(std::string(key) + " out of range");
        return int(n);
    }
    const int64_t n = value.get<int64_t>();
    if (n < lower || n > upper) throw std::runtime_error(std::string(key) + " out of range");
    return int(n);
}
std::string extension(const std::string& input) {
    std::string ext = input;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (!supports_input_extension(ext))
        throw std::runtime_error("unsupported upload extension");
    return ext;
}
MHD_Result respond(MHD_Connection* connection, unsigned status, const std::string& body,
                   const char* type = "application/json; charset=utf-8",
                   const char* retry_after = nullptr) {
    auto* response = MHD_create_response_from_buffer(body.size(), const_cast<char*>(body.data()),
                                                      MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    if (retry_after) MHD_add_response_header(response, "Retry-After", retry_after);
    const auto result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}
// File-backed responses avoid reading and then copying the entire output for
// each downloader. MHD owns the fd after successful response construction.
MHD_Result respond_file(MHD_Connection* connection, const fs::path& path,
                        uint64_t limit, const char* type) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("cannot open result");
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        uint64_t(info.st_size) > limit) {
        ::close(fd);
        throw std::runtime_error("result is missing or exceeds response limit");
    }
    // Open nonblocking to reject special files without hanging, then provide
    // the blocking regular descriptor required by libmicrohttpd.
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        ::close(fd);
        throw std::runtime_error("cannot prepare result file");
    }
    auto* response = MHD_create_response_from_fd64(uint64_t(info.st_size), fd);
    if (!response) { ::close(fd); return MHD_NO; }
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    const auto result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}
MHD_Result failure(MHD_Connection* conn, unsigned code, const std::string& error) {
    return respond(conn, code, Json{{"error", error}}.dump());
}
struct CapacityError : std::runtime_error {
    unsigned status;
    CapacityError(unsigned code, const char* message) : std::runtime_error(message), status(code) {}
};
MHD_Result capacity_failure(MHD_Connection* conn, const CapacityError& error) {
    return respond(conn, error.status, Json{{"error", error.what()}}.dump(),
                   "application/json; charset=utf-8",
                   error.status == MHD_HTTP_TOO_MANY_REQUESTS ? "1" : nullptr);
}
volatile std::sig_atomic_t stopping = 0;
extern "C" void stop_server(int) { stopping = 1; }
} // namespace

class ServerScheduler {
    struct Job {
        std::string id;
        fs::path input, output;
        std::string state = "queued", error;
        int priority = 0, pages_completed = 0, pages_queued = 0;
        uint64_t sequence = 0;
        size_t upload_bytes = 0;
        bool reader_done = false;
        size_t outstanding = 0;
        std::map<int, Page> pages;
        std::string source;
    };
    struct JobPriority {
        bool operator()(const std::shared_ptr<Job>& a, const std::shared_ptr<Job>& b) const {
            if (a->priority != b->priority) return a->priority < b->priority;
            return a->sequence > b->sequence;
        }
    };
    struct Pending {
        std::shared_ptr<Job> job;
        int number;
        uint64_t sequence;
        Image image;
    };
    Pipeline pipeline_;
    Json document_settings_;
    int schema_version_ = 0;
    Options options_;
    BoxTaskPool box_pool_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::set<std::string> reservations_;
    size_t active_jobs_ = 0, inflight_upload_bytes_ = 0;
    uint64_t admitted_ = 0, succeeded_ = 0, failed_ = 0, rejected_ = 0;
    // Completed jobs remain queryable, but never participate in scheduling.
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, JobPriority> waiting_;
    std::deque<Pending> pages_;
    size_t queued_page_bytes_ = 0, reserved_page_bytes_ = 0, reserved_pages_ = 0;
    std::vector<std::thread> threads_;
    uint64_t sequence_ = 0;
    bool shutdown_ = false;

    void fail_locked(const std::shared_ptr<Job>& job, const std::string& message) {
        if (job->state == "succeeded" || job->state == "failed") return;
        job->state = "failed";
        ++failed_;
        --active_jobs_;
        inflight_upload_bytes_ -= job->upload_bytes;
        job->error = message;
        job->pages.clear();
        pages_.erase(std::remove_if(pages_.begin(), pages_.end(),
            [&](const Pending& page) {
                if (page.job != job) return false;
                queued_page_bytes_ -= page.image.rgb.size();
                return true;
            }), pages_.end());
        changed_.notify_all();
    }
    bool finalize_locked(const std::shared_ptr<Job>& job, Document& output) {
        if (!job->reader_done || job->outstanding || job->state == "failed" ||
            job->state == "finalizing" || job->state == "succeeded") return false;
        if (job->pages.empty()) {
            fail_locked(job, "document produced no pages");
            return false;
        }
        job->state = "finalizing";
        output.source = job->source;
        for (auto& entry : job->pages) output.pages.push_back(std::move(entry.second));
        job->pages.clear();
        return true;
    }
    void finalize(const std::shared_ptr<Job>& job, const Document& doc) {
        try {
            write_outputs(doc, job->output, "both", schema_version_);
            std::lock_guard<std::mutex> guard(mutex_);
            if (job->state == "finalizing") {
                job->state = "succeeded";
                ++succeeded_;
                --active_jobs_;
                inflight_upload_bytes_ -= job->upload_bytes;
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> guard(mutex_);
            fail_locked(job, e.what());
        }
        changed_.notify_all();
    }
    void reader() {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] { return shutdown_ || !waiting_.empty(); });
                if (shutdown_) return;
                job = waiting_.top();
                waiting_.pop();
                job->state = "reading";
            }
            try {
                read_document(job->input, document_settings_, [&](int number, const Image& image) {
                    const size_t bytes = image.rgb.size();
                    if (bytes > options_.max_queued_page_bytes)
                        throw std::runtime_error("rendered page exceeds queued page byte limit");
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait(lock, [&] {
                        return shutdown_ || job->state == "failed" ||
                               (pages_.size() + reserved_pages_ < size_t(options_.max_queued_pages) &&
                                bytes <= options_.max_queued_page_bytes -
                                         queued_page_bytes_ - reserved_page_bytes_);
                    });
                    if (shutdown_ || job->state == "failed")
                        throw std::runtime_error("document processing cancelled");
                    // Reserve capacity, then copy the image without holding the
                    // global scheduler lock. Reader-owned images are not movable.
                    ++reserved_pages_;
                    reserved_page_bytes_ += bytes;
                    lock.unlock();
                    Image owned;
                    try { owned = image; }
                    catch (...) {
                        lock.lock();
                        --reserved_pages_;
                        reserved_page_bytes_ -= bytes;
                        lock.unlock();
                        changed_.notify_all();
                        throw;
                    }
                    lock.lock();
                    --reserved_pages_;
                    reserved_page_bytes_ -= bytes;
                    if (shutdown_ || job->state == "failed") {
                        lock.unlock();
                        changed_.notify_all();
                        throw std::runtime_error("document processing cancelled");
                    }
                    try { pages_.push_back({job, number, sequence_++, std::move(owned)}); }
                    catch (...) {
                        lock.unlock();
                        changed_.notify_all();
                        throw;
                    }
                    queued_page_bytes_ += bytes;
                    ++job->outstanding;
                    ++job->pages_queued;
                    job->state = "processing";
                    lock.unlock();
                    changed_.notify_all();
                });
                Document done;
                bool ready;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    job->reader_done = true;
                    ready = finalize_locked(job, done);
                }
                if (ready) finalize(job, done);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex_);
                fail_locked(job, e.what());
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                fail_locked(job, "unknown document reader error");
            }
        }
    }
    void page_worker() {
        while (true) {
            Pending work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] { return shutdown_ || !pages_.empty(); });
                if (shutdown_) return;
                auto chosen = std::max_element(pages_.begin(), pages_.end(),
                    [](const Pending& a, const Pending& b) {
                        if (a.job->priority != b.job->priority)
                            return a.job->priority < b.job->priority;
                        return a.sequence > b.sequence;
                    });
                work = std::move(*chosen);
                pages_.erase(chosen);
                queued_page_bytes_ -= work.image.rgb.size();
            }
            changed_.notify_all();
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (work.job->state == "failed") continue;
            }
            try {
                // BOX work uses a separate fixed pool so one page can expose
                // several regions concurrently without page_workers * box_workers
                // OS threads. The model pool still bounds backend calls.
                Page page = options_.box_workers > 1 ?
                    pipeline_.process_page(work.number, work.image, work.job->output,
                        options_.box_workers, [this](std::function<void()> task) {
                            return box_pool_.submit(std::move(task));
                        }) :
                    pipeline_.process_page(work.number, work.image, work.job->output, 1);
                Document done;
                bool ready;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (work.job->state == "failed") continue;
                    if (!work.job->pages.emplace(work.number, std::move(page)).second)
                        throw std::runtime_error("duplicate PDF page number");
                    ++work.job->pages_completed;
                    --work.job->outstanding;
                    ready = finalize_locked(work.job, done);
                }
                if (ready) finalize(work.job, done);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> guard(mutex_);
                fail_locked(work.job, e.what());
            } catch (...) {
                std::lock_guard<std::mutex> guard(mutex_);
                fail_locked(work.job, "unknown page inference error");
            }
        }
    }
public:
    explicit ServerScheduler(Json config, Options options)
        : pipeline_(config), document_settings_(config.value("document", Json::object())),
          schema_version_(config.value("output",Json::object()).value("schema_version",0)),
          options_(std::move(options)),
          box_pool_(options_.box_workers > 1 ? options_.box_workers : 0) {
        fs::create_directories(options_.data_dir / "jobs");
        fs::permissions(options_.data_dir / "jobs", fs::perms::owner_all,
                        fs::perm_options::replace);
        try {
            for (int i = 0; i < options_.page_workers; ++i)
                threads_.emplace_back([this] { page_worker(); });
            for (int i = 0; i < options_.document_workers; ++i)
                threads_.emplace_back([this] { reader(); });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shutdown_ = true;
            }
            changed_.notify_all();
            for (auto& thread : threads_) thread.join();
            throw;
        }
    }
    ~ServerScheduler() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        changed_.notify_all();
        for (auto& worker : threads_) worker.join();
    }
    fs::path staging(const std::string& id) {
        const auto dir = options_.data_dir / "jobs" / id;
        if (!fs::create_directory(dir)) throw std::runtime_error("job storage collision");
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        return dir;
    }
    std::string reserve() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (jobs_.size() + reservations_.size() >= options_.max_jobs) {
            ++rejected_;
            throw CapacityError(MHD_HTTP_SERVICE_UNAVAILABLE, "cumulative job limit reached");
        }
        if (active_jobs_ + reservations_.size() >= options_.max_active_jobs) {
            ++rejected_;
            throw CapacityError(MHD_HTTP_TOO_MANY_REQUESTS, "active job limit reached");
        }
        for (int tries = 0; tries < 10; ++tries) {
            std::string id = new_id();
            if (!jobs_.count(id) && !reservations_.count(id) &&
                !fs::exists(options_.data_dir / "jobs" / id)) {
                reservations_.insert(id);
                return id;
            }
        }
        throw std::runtime_error("cannot allocate a unique job ID");
    }
    void release_reservation(const std::string& id, size_t upload_bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (reservations_.erase(id)) inflight_upload_bytes_ -= upload_bytes;
    }
    bool charge_upload(size_t bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (bytes > options_.max_inflight_upload_bytes - inflight_upload_bytes_) {
            ++rejected_;
            return false;
        }
        inflight_upload_bytes_ += bytes;
        return true;
    }
    void submit(const std::string& id, const fs::path& input, int priority, size_t upload_bytes = 0) {
        auto job = std::make_shared<Job>();
        job->id = id; job->input = input;
        job->output = options_.data_dir / "jobs" / id / "output";
        job->priority = priority;
        job->upload_bytes = upload_bytes;
        job->source = fs::absolute(input).string();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!reservations_.count(id)) throw std::runtime_error("job reservation missing");
            if (!jobs_.emplace(id, job).second) throw std::runtime_error("duplicate job ID");
            job->sequence = sequence_++;
            try { waiting_.push(job); }
            catch (...) { jobs_.erase(id); throw; }
            reservations_.erase(id);
            ++active_jobs_;
            ++admitted_;
        }
        changed_.notify_all();
    }
    fs::path approved_path(const std::string& path) const {
        if (options_.allowed_root.empty()) throw std::runtime_error("server-side paths are disabled");
        const fs::path value = fs::canonical(path);
        if (!below(options_.allowed_root, value) || !fs::is_regular_file(value))
            throw std::runtime_error("path is not a regular file inside allowed input root");
        return value;
    }
    Json status(const std::string& id) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto item = jobs_.find(id);
        if (item == jobs_.end()) throw std::out_of_range("job not found");
        const auto& j = *item->second;
        return {{"id", j.id}, {"status", j.state}, {"priority", j.priority},
                {"pages_queued", j.pages_queued}, {"pages_completed", j.pages_completed},
                {"error", j.error}, {"result_url", "/v1/jobs/" + id + "/result"},
                {"markdown_url", "/v1/jobs/" + id + "/result?format=markdown"}};
    }
    Json metrics() {
        std::lock_guard<std::mutex> guard(mutex_);
        return {{"active_jobs", active_jobs_}, {"reserved_jobs", reservations_.size()},
                {"queued_documents", waiting_.size()}, {"queued_pages", pages_.size()},
                {"queued_page_bytes", queued_page_bytes_},
                {"reserved_page_bytes", reserved_page_bytes_},
                {"inflight_upload_bytes", inflight_upload_bytes_},
                {"admitted_total", admitted_}, {"succeeded_total", succeeded_},
                {"failed_total", failed_}, {"rejected_total", rejected_}};
    }
    fs::path result_file(const std::string& id, const std::string& format) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) throw std::out_of_range("job not found");
        if (it->second->state != "succeeded") throw std::logic_error("result is not ready");
        if (format == "json") return it->second->output / "result.json";
        if (format == "markdown") return it->second->output / "result.md";
        throw std::runtime_error("format must be json or markdown");
    }
    fs::path asset_file(const std::string& id, const std::string& name) {
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name == "." || name == ".." ||
            name.size() < 5 || name.substr(name.size() - 4) != ".png")
            throw std::runtime_error("invalid asset name");
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) throw std::out_of_range("job not found");
        if (it->second->state != "succeeded") throw std::logic_error("result is not ready");
        return it->second->output / "assets" / name;
    }
    const Options& options() const { return options_; }
};

namespace {
struct Request {
    bool upload = false;
    bool submitted = false;
    bool rejected = false;
    unsigned error_status = 400;
    std::string error;
    std::string body, id;
    fs::path spool, staged_folder;
    std::ofstream file;
    size_t bytes = 0;
    int priority = 0;
};
struct Http {
    ServerScheduler& scheduler;
    static MHD_Result handler(void* context, MHD_Connection* conn, const char* url,
                              const char* method, const char*, const char* data,
                              size_t* size, void** cls) {
        return static_cast<Http*>(context)->handle(conn, url, method, data, size, cls);
    }
    static void completed(void* context, MHD_Connection*, void** cls, MHD_RequestTerminationCode) {
        auto* request = static_cast<Request*>(*cls);
        if (request && !request->submitted && !request->id.empty()) {
            request->file.close();
            std::error_code ignored;
            if (!request->staged_folder.empty()) fs::remove_all(request->staged_folder, ignored);
            static_cast<Http*>(context)->scheduler.release_reservation(
                request->id, request->upload ? request->bytes : 0);
        }
        delete request;
        *cls = nullptr;
    }
    MHD_Result handle(MHD_Connection* conn, const std::string& url, const std::string& method,
                      const char* data, size_t* size, void** cls) {
        auto* request = static_cast<Request*>(*cls);
        if (!request) {
            auto entry = std::make_unique<Request>();
            const std::string auth = scheduler.options().api_key;
            if (!auth.empty()) {
                const char* header = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization");
                if (!header || ("Bearer " + auth) != header)
                    return failure(conn, MHD_HTTP_UNAUTHORIZED, "unauthorized");
            }
            if (method == "POST" && (url == "/v1/jobs" || url == "/v1/jobs/upload")) {
                const char* header = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Content-Length");
                if (header) {
                    try {
                        const uint64_t len = std::stoull(header);
                        if (len > (url == "/v1/jobs" ? 65536ULL : scheduler.options().max_upload_bytes))
                            return failure(conn, MHD_HTTP_CONTENT_TOO_LARGE, "request body too large");
                    } catch (const std::exception&) {
                        return failure(conn, MHD_HTTP_BAD_REQUEST, "invalid Content-Length");
                    }
                }
                entry->upload = url == "/v1/jobs/upload";
                try {
                    if (entry->upload) {
                        const char* ext = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "extension");
                        if (!ext) return failure(conn, MHD_HTTP_BAD_REQUEST, "extension query is required");
                        const std::string suffix = extension(ext);
                        const char* pr = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "priority");
                        if (pr) {
                            const Json value = Json::parse(pr);
                            entry->priority = number({{"priority", value}}, "priority", 0, -1000000, 1000000);
                        }
                        entry->id = scheduler.reserve();
                        entry->staged_folder = scheduler.staging(entry->id);
                        entry->spool = entry->staged_folder / ("source" + suffix);
                        entry->file.open(entry->spool, std::ios::binary | std::ios::trunc);
                        if (!entry->file) throw std::runtime_error("cannot create uploaded file");
                    } else entry->id = scheduler.reserve();
                } catch (const CapacityError& e) {
                    if (!entry->id.empty()) scheduler.release_reservation(entry->id, 0);
                    return capacity_failure(conn, e);
                } catch (const std::exception& e) {
                    if (!entry->id.empty()) {
                        entry->file.close();
                        std::error_code ignored;
                        if (!entry->staged_folder.empty()) fs::remove_all(entry->staged_folder, ignored);
                        scheduler.release_reservation(entry->id, 0);
                    }
                    return failure(conn, MHD_HTTP_BAD_REQUEST, e.what());
                }
            }
            *cls = entry.release();
            return MHD_YES;
        }
        if (method == "POST" && (url == "/v1/jobs" || url == "/v1/jobs/upload")) {
            if (*size) {
                if (request->bytes > (request->upload ? scheduler.options().max_upload_bytes : 65536)
                        - std::min(*size, request->upload ? scheduler.options().max_upload_bytes : size_t(65536)) ||
                    *size > (request->upload ? scheduler.options().max_upload_bytes : size_t(65536))) {
                    request->rejected = true;
                    request->error_status = MHD_HTTP_CONTENT_TOO_LARGE;
                    request->error = "request body too large";
                } else if (!request->rejected) {
                    if (request->upload) {
                        if (!scheduler.charge_upload(*size)) {
                            request->rejected = true;
                            request->error_status = MHD_HTTP_TOO_MANY_REQUESTS;
                            request->error = "inflight upload byte limit reached";
                        } else {
                            request->bytes += *size;
                            request->file.write(data, std::streamsize(*size));
                        }
                        if (!request->rejected && !request->file) {
                            request->rejected = true;
                            request->error_status = MHD_HTTP_INTERNAL_SERVER_ERROR;
                            request->error = "cannot write uploaded file";
                        }
                    } else { request->body.append(data, *size); request->bytes += *size; }
                }
                *size = 0;
                return MHD_YES;
            }
            if (request->rejected) {
                if (request->error_status == MHD_HTTP_TOO_MANY_REQUESTS)
                    return capacity_failure(conn, CapacityError(request->error_status, request->error.c_str()));
                return failure(conn, request->error_status, request->error);
            }
            try {
                if (request->upload) {
                    request->file.close();
                    if (!request->file) throw std::runtime_error("uploaded file flush failed");
                    if (!request->bytes) throw std::runtime_error("empty upload");
                    scheduler.submit(request->id, request->spool, request->priority, request->bytes);
                    request->submitted = true;
                    return respond(conn, MHD_HTTP_ACCEPTED,
                        Json{{"id", request->id}, {"status", "queued"},
                             {"status_url", "/v1/jobs/" + request->id}}.dump());
                }
                const Json item = Json::parse(request->body);
                if (!item.is_object()) throw std::runtime_error("job body must be an object");
                const std::string input = item.at("path").get<std::string>();
                const int priority = number(item, "priority", 0, -1000000, 1000000);
                const fs::path source = scheduler.approved_path(input);
                request->staged_folder = scheduler.staging(request->id);
                scheduler.submit(request->id, source, priority);
                request->submitted = true;
                return respond(conn, MHD_HTTP_ACCEPTED,
                    Json{{"id", request->id}, {"status", "queued"},
                         {"status_url", "/v1/jobs/" + request->id}}.dump());
            } catch (const CapacityError& e) {
                return capacity_failure(conn, e);
            } catch (const std::exception& e) {
                return failure(conn, MHD_HTTP_BAD_REQUEST, e.what());
            }
        }
        if (method == "GET" && url == "/healthz")
            return respond(conn, MHD_HTTP_OK, R"({"status":"ok"})");
        if (method == "GET" && url == "/v1/metrics")
            return respond(conn, MHD_HTTP_OK, scheduler.metrics().dump());
        if (method != "GET") return failure(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "unsupported method");
        const std::string prefix = "/v1/jobs/";
        if (url.compare(0, prefix.size(), prefix) != 0)
            return failure(conn, MHD_HTTP_NOT_FOUND, "not found");
        const std::string tail = url.substr(prefix.size());
        const size_t slash = tail.find('/');
        const std::string id = tail.substr(0, slash);
        if (id.empty() || id.size() > 64 ||
            !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); }))
            return failure(conn, MHD_HTTP_BAD_REQUEST, "invalid job ID");
        try {
            if (slash == std::string::npos)
                return respond(conn, MHD_HTTP_OK, scheduler.status(id).dump());
            const std::string subpath = tail.substr(slash);
            if (subpath == "/result") {
                const char* format = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "format");
                const std::string kind = format ? format : "json";
                const auto file = scheduler.result_file(id, kind);
                return respond_file(conn, file, 64 * 1024 * 1024,
                    kind == "json" ? "application/json; charset=utf-8" : "text/markdown; charset=utf-8");
            }
            const std::string assets = "/assets/";
            if (subpath.compare(0, assets.size(), assets) == 0) {
                const auto file = scheduler.asset_file(id, subpath.substr(assets.size()));
                return respond_file(conn, file, 128 * 1024 * 1024, "image/png");
            }
            return failure(conn, MHD_HTTP_NOT_FOUND, "not found");
        } catch (const std::out_of_range&) {
            return failure(conn, MHD_HTTP_NOT_FOUND, "job not found");
        } catch (const std::logic_error&) {
            return failure(conn, MHD_HTTP_CONFLICT, "result is not ready");
        } catch (const std::exception& e) {
            return failure(conn, MHD_HTTP_BAD_REQUEST, e.what());
        }
    }
};
} // namespace
} // namespace omniocr

int main(int argc, char** argv) {
    using namespace omniocr;
    try {
        Options options;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "omniocr-server --config CONFIG --data-dir DIR [--allowed-input-root DIR]"
                         " [--host 127.0.0.1] [--port 8080] [--page-workers N] [--box-workers N]"
                         " [--document-workers N] [--max-queued-pages N]"
                         " [--max-upload-bytes N] [--max-jobs N] [--max-active-jobs N]"
                         " [--max-inflight-upload-bytes N] [--max-queued-page-bytes N]"
                         " [--http-connections N]"
                         " [--api-key-env NAME]\n";
            return 0;
        }
        std::map<std::string, std::string> args;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key != "--config" && key != "--data-dir" && key != "--allowed-input-root" &&
                key != "--host" && key != "--port" && key != "--page-workers" &&
                key != "--box-workers" &&
                key != "--document-workers" && key != "--max-queued-pages" &&
                key != "--max-upload-bytes" && key != "--max-jobs" &&
                key != "--max-active-jobs" && key != "--max-inflight-upload-bytes" &&
                key != "--max-queued-page-bytes" &&
                key != "--http-connections" &&
                key != "--api-key-env")
                throw std::runtime_error("unknown option: " + key);
            if (++i >= argc || args.count(key)) throw std::runtime_error("duplicate or missing option: " + key);
            args[key] = argv[i];
        }
        if (!args.count("--config") || !args.count("--data-dir"))
            throw std::runtime_error("--config and --data-dir are required");
        auto parse = [&](const char* key, int fallback, int low, int high) {
            if (!args.count(key)) return fallback;
            const auto& value = args.at(key);
            size_t consumed = 0;
            const long long n = std::stoll(value, &consumed);
            if (consumed != value.size() || n < low || n > high)
                throw std::runtime_error(std::string(key) + " out of range");
            return int(n);
        };
        options.config = fs::canonical(args.at("--config"));
        options.data_dir = fs::absolute(args.at("--data-dir")).lexically_normal();
        if (args.count("--allowed-input-root"))
            options.allowed_root = fs::canonical(args.at("--allowed-input-root"));
        options.host = args.count("--host") ? args.at("--host") : options.host;
        if (options.host != "127.0.0.1" && options.host != "0.0.0.0")
            throw std::runtime_error("--host supports 127.0.0.1 or 0.0.0.0");
        options.port = static_cast<unsigned short>(parse("--port", 8080, 1, 65535));
        options.page_workers = parse("--page-workers", 4, 1, 128);
        options.box_workers = parse("--box-workers", 1, 1, 128);
        options.document_workers = parse("--document-workers", 2, 1, 32);
        options.max_queued_pages = parse("--max-queued-pages", 2, 1, 256);
        options.max_jobs = size_t(parse("--max-jobs", 1000, 1, 100000));
        options.max_active_jobs = size_t(parse("--max-active-jobs", 64, 1, 100000));
        options.http_connections = unsigned(parse("--http-connections", 128, 16, 1024));
        options.max_upload_bytes = size_t(parse("--max-upload-bytes", 64*1024*1024, 1, 512*1024*1024));
        auto parse_bytes = [&](const char* key, size_t fallback) {
            if (!args.count(key)) return fallback;
            size_t consumed = 0;
            const auto& value = args.at(key);
            const auto n = std::stoull(value, &consumed);
            if (consumed != value.size() || n == 0 || n > (1ULL << 40) ||
                n > std::numeric_limits<size_t>::max())
                throw std::runtime_error(std::string(key) + " out of range");
            return size_t(n);
        };
        options.max_inflight_upload_bytes = parse_bytes(
            "--max-inflight-upload-bytes", options.max_inflight_upload_bytes);
        options.max_queued_page_bytes = parse_bytes(
            "--max-queued-page-bytes", options.max_queued_page_bytes);
        if (args.count("--api-key-env")) {
            const char* value = std::getenv(args.at("--api-key-env").c_str());
            if (!value || !*value) throw std::runtime_error("API key environment variable is missing or empty");
            options.api_key = value;
        }
        if (options.host == "0.0.0.0" && options.api_key.empty())
            throw std::runtime_error("non-loopback binding requires --api-key-env");
        fs::create_directories(options.data_dir);
        const auto config = load_config(options.config);
        ServerScheduler scheduler(config, options);
        Http http{scheduler};
        const auto flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION;
        sockaddr_in bind_address{};
        bind_address.sin_family = AF_INET;
        bind_address.sin_port = htons(options.port);
        if (inet_pton(AF_INET, options.host.c_str(), &bind_address.sin_addr) != 1)
            throw std::runtime_error("invalid IPv4 bind address");
        struct MHD_Daemon* daemon = MHD_start_daemon(flags, options.port, nullptr, nullptr,
            &Http::handler, &http, MHD_OPTION_SOCK_ADDR, &bind_address,
            MHD_OPTION_CONNECTION_TIMEOUT, 30u,
            MHD_OPTION_CONNECTION_LIMIT, options.http_connections,
            MHD_OPTION_NOTIFY_COMPLETED, &Http::completed, &http, MHD_OPTION_END);
        if (!daemon) throw std::runtime_error("could not start HTTP server");
        std::signal(SIGINT, stop_server);
        std::signal(SIGTERM, stop_server);
        std::cout << "OmniOCR REST server listening on " << options.host << ':' << options.port << '\n';
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        MHD_stop_daemon(daemon);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "omniocr-server: " << e.what() << '\n';
        return 1;
    }
}
