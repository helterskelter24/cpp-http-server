#include "app/routes.h"

#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "http/router.h"
#include "server/stats.h"
#include "test.h"

namespace {

hs::Request make(hs::Method m, const std::string& path) {
    hs::Request req;
    req.method = m;
    req.path = path;
    req.target = path;
    return req;
}

const std::string* header(const hs::Response& r, const std::string& name) {
    for (const auto& h : r.headers)
        if (h.first == name) return &h.second;
    return nullptr;
}

struct App {
    hs::Config config;
    hs::Stats stats;
    hs::Router router;
    App() {
        config.threads = 3;
        hs::AppContext ctx{&config, &stats};
        hs::register_routes(router, ctx);
    }
    hs::Response call(hs::Request req) { return router.dispatch(req); }
};

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

}  // namespace

TEST(routes_health) {
    App app;
    hs::Response r = app.call(make(hs::Method::GET, "/health"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "{\"status\":\"ok\"}");
    const std::string* ct = header(r, "Content-Type");
    CHECK(ct != nullptr);
    if (ct) CHECK_EQ(*ct, "application/json");
}

TEST(routes_echo) {
    App app;
    hs::Request req = make(hs::Method::POST, "/api/echo");
    req.query = "a=1";
    req.headers.add("Host", "x");
    req.headers.add("X-Y", "z");
    req.body = "say \"hi\"\nbye";
    hs::Response r = app.call(req);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body,
             "{\"method\":\"POST\",\"path\":\"/api/echo\",\"query\":\"a=1\","
             "\"header_count\":2,\"body_length\":12,"
             "\"body\":\"say \\\"hi\\\"\\nbye\"}");
}

TEST(routes_echo_get_not_allowed) {
    App app;
    CHECK_EQ(app.call(make(hs::Method::GET, "/api/echo")).status, 405);
}

TEST(routes_users_param) {
    App app;
    hs::Response r = app.call(make(hs::Method::GET, "/api/users/42"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "{\"id\":\"42\",\"name\":\"user-42\"}");
}

TEST(routes_users_bad_id_400) {
    App app;
    CHECK_EQ(app.call(make(hs::Method::GET, "/api/users/abc")).status, 400);
    CHECK_EQ(app.call(make(hs::Method::GET, "/api/users/4x")).status, 400);
    CHECK_EQ(app.call(make(hs::Method::GET, "/api/users/-1")).status, 400);
    // Very long digit strings are rejected so the echoed name stays small.
    CHECK_EQ(app.call(make(hs::Method::GET, "/api/users/" + std::string(40, '1'))).status, 400);
}

TEST(routes_time_shape) {
    App app;
    hs::Response r = app.call(make(hs::Method::GET, "/api/time"));
    CHECK_EQ(r.status, 200);
    CHECK(r.body.rfind("{\"unix\":", 0) == 0);
    size_t iso = r.body.find("\"iso\":\"");
    CHECK(iso != std::string::npos);
    if (iso != std::string::npos) {
        std::string stamp = r.body.substr(iso + 7, 20);
        // YYYY-MM-DDTHH:MM:SSZ
        CHECK_EQ(stamp.size(), 20u);
        CHECK_EQ(stamp[4], '-');
        CHECK_EQ(stamp[10], 'T');
        CHECK_EQ(stamp[13], ':');
        CHECK_EQ(stamp[19], 'Z');
    }
}

TEST(stats_record_classes) {
    hs::Stats s;
    s.record(200);
    s.record(404);
    s.record(302);
    s.record(503);
    s.record(99);  // out of range counts as 5xx
    s.record(700);
    CHECK_EQ(s.requests_total.load(), 6u);
    CHECK_EQ(s.responses_2xx.load(), 1u);
    CHECK_EQ(s.responses_3xx.load(), 1u);
    CHECK_EQ(s.responses_4xx.load(), 1u);
    CHECK_EQ(s.responses_5xx.load(), 3u);
}

TEST(stats_shape) {
    App app;
    app.stats.record(200);
    app.stats.record(404);
    app.stats.connections_accepted = 7;
    app.stats.active_connections = 2;
    hs::Response r = app.call(make(hs::Method::GET, "/api/stats"));
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, "\"uptime_seconds\":"));
    CHECK(contains(r.body, "\"mode\":\"epoll\""));
    CHECK(contains(r.body, "\"threads\":3"));
    CHECK(contains(r.body, "\"active_connections\":2"));
    CHECK(contains(r.body, "\"connections_accepted\":7"));
    CHECK(contains(r.body, "\"requests_total\":2"));
    CHECK(contains(r.body, "\"responses\":{\"2xx\":1,\"3xx\":0,\"4xx\":1,\"5xx\":0}"));
}

TEST(stats_mode_pool) {
    App app;
    app.config.mode = hs::Mode::Pool;
    hs::Response r = app.call(make(hs::Method::GET, "/api/stats"));
    CHECK(contains(r.body, "\"mode\":\"pool\""));
}

TEST(stats_concurrent_record) {
    hs::Stats s;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&s] {
            for (int i = 0; i < 10000; ++i) s.record(200);
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(s.requests_total.load(), 40000u);
    CHECK_EQ(s.responses_2xx.load(), 40000u);
}
