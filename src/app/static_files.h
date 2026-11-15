#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "http/request.h"
#include "http/response.h"
#include "http/router.h"

namespace hs {

// Files larger than this are refused with a 500; they are read whole into memory.
constexpr size_t kMaxStaticFileBytes = 64u * 1024 * 1024;

// Serves files below a root directory. Immutable after construction, so many
// threads may call serve() at once.
class StaticFiles {
public:
    // The root is resolved with realpath once, so later containment checks
    // compare against a canonical path.
    explicit StaticFiles(std::string root);

    // False if the root could not be resolved; every request then gets 404.
    bool root_ok() const { return root_ok_; }

    // 200 with the file, or 403 (unreadable), 404 (missing, outside the root,
    // not a regular file) or 500 (too large, read error).
    Response serve(const Request& req) const;

private:
    bool inside_root(const std::string& resolved) const;
    // realpath plus the containment check; empty string means "not servable".
    std::string resolve_inside_root(const std::string& candidate) const;

    std::string root_;        // canonical, no trailing slash (except "/")
    std::string root_prefix_; // root_ + "/" (just "/" for the filesystem root)
    bool root_ok_ = false;
};

// Makes the files the router's fallback for GET/HEAD requests no route claims.
void register_static(Router& router, std::shared_ptr<const StaticFiles> files);

}  // namespace hs
