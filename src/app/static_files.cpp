#include "app/static_files.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string_view>

#include "http/mime.h"
#include "logger.h"

namespace hs {

namespace {

bool is_text_type(std::string_view type) {
    return type.substr(0, 5) == "text/" || type == "application/javascript" ||
           type == "application/json" || type == "image/svg+xml";
}

// Reads exactly `size` bytes from fd (or until EOF if the file shrank).
// Returns false on a read error.
bool read_all(int fd, size_t size, std::string& out) {
    out.resize(size);
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, &out[done], size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) break;  // file got shorter after fstat
        done += static_cast<size_t>(n);
    }
    out.resize(done);
    return true;
}

}  // namespace

StaticFiles::StaticFiles(std::string root) {
    char buf[PATH_MAX];
    if (realpath(root.c_str(), buf) == nullptr) {
        log_msg(LogLevel::Error, "static root '%s' cannot be resolved: %s", root.c_str(),
                strerror(errno));
        return;
    }
    root_ = buf;
    root_prefix_ = root_ == "/" ? "/" : root_ + "/";
    root_ok_ = true;
}

// Compares whole path components: "/srv/www2" must not count as inside
// "/srv/www", which a plain string-prefix test would allow.
bool StaticFiles::inside_root(const std::string& resolved) const {
    return resolved == root_ || resolved.compare(0, root_prefix_.size(), root_prefix_) == 0;
}

std::string StaticFiles::resolve_inside_root(const std::string& candidate) const {
    char buf[PATH_MAX];
    // realpath follows symlinks and removes "." / ".." / repeated slashes, so the
    // result is the file's real location. This check, not any filtering of the
    // request text, is what keeps requests inside the root. Every failure
    // (missing, loop, too long, not searchable) is reported as "not found" so a
    // client cannot probe which paths exist outside the root.
    if (realpath(candidate.c_str(), buf) == nullptr) return "";
    std::string resolved = buf;
    if (!inside_root(resolved)) return "";
    return resolved;
}

Response StaticFiles::serve(const Request& req) const {
    if (!root_ok_) return Response::error(404);
    // A NUL would silently truncate the path inside realpath/open.
    if (req.path.find('\0') != std::string::npos) return Response::error(404);

    std::string resolved = resolve_inside_root(root_ + "/" + req.path);
    if (resolved.empty()) return Response::error(404);

    // Open first, then judge the file through the descriptor: fstat on the fd
    // cannot be fooled by the path being swapped after the check.
    // O_NONBLOCK so opening a FIFO cannot hang a worker thread; O_NOFOLLOW since
    // the resolved path should contain no symlinks any more.
    constexpr int kOpenFlags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW;
    int fd = open(resolved.c_str(), kOpenFlags);
    if (fd < 0) return Response::error(errno == EACCES ? 403 : 404);

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return Response::error(404);
    }

    if (S_ISDIR(st.st_mode)) {
        close(fd);
        // index.html may itself be a symlink, so it goes through the same
        // resolve-and-contain step.
        resolved = resolve_inside_root(resolved + "/index.html");
        if (resolved.empty()) return Response::error(404);
        fd = open(resolved.c_str(), kOpenFlags);
        if (fd < 0) return Response::error(errno == EACCES ? 403 : 404);
        if (fstat(fd, &st) != 0) {
            close(fd);
            return Response::error(404);
        }
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return Response::error(404);
    }
    if (st.st_size < 0 || static_cast<unsigned long long>(st.st_size) > kMaxStaticFileBytes) {
        close(fd);
        log_msg(LogLevel::Error, "refusing to serve '%s': %lld bytes exceeds the %zu byte limit",
                resolved.c_str(), static_cast<long long>(st.st_size), kMaxStaticFileBytes);
        return Response::error(500);
    }

    Response r;
    r.status = 200;
    bool ok = read_all(fd, static_cast<size_t>(st.st_size), r.body);
    int saved_errno = errno;
    close(fd);
    if (!ok) {
        log_msg(LogLevel::Error, "read of '%s' failed: %s", resolved.c_str(), strerror(saved_errno));
        return Response::error(500);
    }

    std::string type(mime_type(resolved));
    if (is_text_type(type)) type += "; charset=utf-8";
    r.headers.emplace_back("Content-Type", std::move(type));
    return r;
}

void register_static(Router& router, std::shared_ptr<const StaticFiles> files) {
    // The shared_ptr keeps the object alive for as long as the router holds the handler.
    router.set_fallback([files](const Request& req) { return files->serve(req); });
}

}  // namespace hs
