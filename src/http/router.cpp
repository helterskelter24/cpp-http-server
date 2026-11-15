#include "http/router.h"

#include <exception>
#include <stdexcept>

#include "logger.h"

namespace hs {

namespace {

// Splits on '/' and drops empty pieces, so "/a/", "/a" and "//a" are the same path.
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        if (slash > pos) out.push_back(path.substr(pos, slash - pos));
        pos = slash + 1;
    }
    return out;
}

bool is_param(const std::string& segment) { return !segment.empty() && segment[0] == ':'; }

// Does the pattern fit the path? Same length, and every literal segment equal.
bool matches(const std::vector<std::string>& pattern, const std::vector<std::string>& path) {
    if (pattern.size() != path.size()) return false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (!is_param(pattern[i]) && pattern[i] != path[i]) return false;
    }
    return true;
}

// True if pattern a is more specific than b: at the first segment where one has a
// literal and the other a parameter, the literal wins. Two patterns that both match
// one path always have the same length, so comparing position by position is enough.
bool more_specific(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        bool a_param = is_param(a[i]);
        if (a_param != is_param(b[i])) return !a_param;
    }
    return false;
}

// Allow lists methods in one fixed order whatever the registration order was.
constexpr Method kAllowOrder[] = {Method::GET,     Method::HEAD,  Method::POST,   Method::PUT,
                                  Method::DELETE_, Method::PATCH, Method::OPTIONS};

}  // namespace

void Router::add(Method method, std::string pattern, Handler handler) {
    std::vector<std::string> segments = split_path(pattern);
    for (const auto& s : segments) {
        // A bare ":" would be a parameter nobody can read; catch the typo at startup.
        if (s == ":") throw std::invalid_argument("empty parameter name in pattern: " + pattern);
    }
    routes_.push_back({method, std::move(segments), std::move(handler)});
}

void Router::set_fallback(Handler handler) { fallback_ = std::move(handler); }

Response Router::dispatch(Request& req) const {
    if (req.method == Method::UNKNOWN) return Response::error(501);

    std::vector<std::string> path = split_path(req.path);
    // HEAD is answered by the GET handler; the server drops the body when serializing.
    Method wanted = req.method == Method::HEAD ? Method::GET : req.method;

    const Route* best = nullptr;
    bool path_known = false;
    bool allowed[8] = {};  // indexed by Method; which methods exist for this path
    for (const Route& route : routes_) {
        if (!matches(route.segments, path)) continue;
        path_known = true;
        allowed[static_cast<int>(route.method)] = true;
        if (route.method != wanted) continue;
        // On a tie the first registered route wins, since only a strictly more specific one
        // replaces it.
        if (best == nullptr || more_specific(route.segments, best->segments)) best = &route;
    }

    const Handler* handler = nullptr;
    if (best != nullptr) {
        handler = &best->handler;
        req.params.clear();
        for (size_t i = 0; i < path.size(); ++i) {
            if (is_param(best->segments[i])) req.params[best->segments[i].substr(1)] = path[i];
        }
    } else if (path_known) {
        std::string list;
        for (Method m : kAllowOrder) {
            // HEAD is implied by GET and never registered itself.
            bool listed = m == Method::HEAD ? allowed[static_cast<int>(Method::GET)]
                                            : allowed[static_cast<int>(m)];
            if (!listed) continue;
            if (!list.empty()) list += ", ";
            list += method_name(m);
        }
        Response res = Response::error(405);
        res.headers.emplace_back("Allow", list);
        return res;
    } else if (fallback_ && wanted == Method::GET) {
        handler = &fallback_;
    } else {
        return Response::error(404);
    }

    // A buggy handler must not take the worker thread down. The exception text goes
    // to the log only; the client gets a generic 500.
    try {
        return (*handler)(req);
    } catch (const std::exception& e) {
        log_msg(LogLevel::Error, "handler for %s %s threw: %s",
                std::string(method_name(req.method)).c_str(), req.path.c_str(), e.what());
    } catch (...) {
        log_msg(LogLevel::Error, "handler for %s %s threw a non-standard exception",
                std::string(method_name(req.method)).c_str(), req.path.c_str());
    }
    return Response::error(500);
}

}  // namespace hs
