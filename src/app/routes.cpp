#include "app/routes.h"

#include <chrono>
#include <cstdint>
#include <ctime>

#include "http/json.h"

namespace hs {

namespace {

Response health(const Request&) { return Response::json(200, "{\"status\":\"ok\"}"); }

Response echo(const Request& req) {
    JsonWriter w;
    w.begin_object();
    w.key("method").value(method_name(req.method));
    w.key("path").value(req.path);
    w.key("query").value(req.query);
    w.key("header_count").value(static_cast<uint64_t>(req.headers.size()));
    w.key("body_length").value(static_cast<uint64_t>(req.body.size()));
    w.key("body").value(req.body);
    w.end_object();
    return Response::json(200, w.str());
}

Response user(const Request& req) {
    auto it = req.params.find("id");
    // Digits only, and short enough that the echoed name stays small and could
    // fit a 64-bit id. This also keeps the id safe to place in the output.
    if (it == req.params.end() || it->second.empty() || it->second.size() > 18)
        return Response::error(400);
    for (char c : it->second)
        if (c < '0' || c > '9') return Response::error(400);

    JsonWriter w;
    w.begin_object();
    w.key("id").value(it->second);
    w.key("name").value("user-" + it->second);
    w.end_object();
    return Response::json(200, w.str());
}

Response server_time(const Request&) {
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);  // gmtime_r, not gmtime: no shared static buffer
    char iso[32];
    std::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);

    JsonWriter w;
    w.begin_object();
    w.key("unix").value(static_cast<int64_t>(now));
    w.key("iso").value(iso);
    w.end_object();
    return Response::json(200, w.str());
}

Response stats(const Request&, const AppContext& ctx) {
    const Stats& s = *ctx.stats;
    auto load = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s.started);

    // Counters go out as exact 64-bit integers; JsonWriter has no floating-point
    // value on purpose, so nothing here can be rounded.
    JsonWriter w;
    w.begin_object();
    w.key("uptime_seconds").value(static_cast<uint64_t>(uptime.count()));
    w.key("mode").value(ctx.config->mode == Mode::Pool ? "pool" : "epoll");
    w.key("threads").value(static_cast<uint64_t>(ctx.config->threads));
    w.key("active_connections").value(load(s.active_connections));
    w.key("connections_accepted").value(load(s.connections_accepted));
    w.key("requests_total").value(load(s.requests_total));
    w.key("responses").begin_object();
    w.key("2xx").value(load(s.responses_2xx));
    w.key("3xx").value(load(s.responses_3xx));
    w.key("4xx").value(load(s.responses_4xx));
    w.key("5xx").value(load(s.responses_5xx));
    w.end_object();
    w.end_object();
    return Response::json(200, w.str());
}

}  // namespace

void register_routes(Router& router, const AppContext& ctx) {
    router.add(Method::GET, "/health", health);
    router.add(Method::GET, "/api/stats", [ctx](const Request& req) { return stats(req, ctx); });
    router.add(Method::GET, "/api/time", server_time);
    router.add(Method::POST, "/api/echo", echo);
    router.add(Method::GET, "/api/users/:id", user);
}

}  // namespace hs
