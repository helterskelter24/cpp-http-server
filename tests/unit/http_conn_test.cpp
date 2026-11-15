#include <fcntl.h>
#include <unistd.h>

#include <string>

#include "config.h"
#include "http/router.h"
#include "server/http_conn.h"
#include "server/stats.h"
#include "test.h"

namespace {

constexpr size_t kBigBody = 64 * 1024;

// A router with the two routes the tests need, plus /big for backpressure.
struct Fixture {
    hs::Config config;
    hs::Stats stats;
    hs::Router router;
    Fixture() {
        router.add(hs::Method::GET, "/health",
                   [](const hs::Request&) { return hs::Response::json(200, "{\"status\":\"ok\"}"); });
        router.add(hs::Method::POST, "/api/echo",
                   [](const hs::Request& req) { return hs::Response::text(200, req.body); });
        router.add(hs::Method::GET, "/big", [](const hs::Request&) {
            return hs::Response::text(200, std::string(kBigBody, 'x'));
        });
    }
};

void feed(hs::HttpConn& conn, const std::string& s) { conn.on_data(s.data(), s.size()); }

size_t count(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

const std::string kGetHealth = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";

}  // namespace

TEST(http_conn_single_request) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    feed(conn, kGetHealth);
    std::string out;
    CHECK(conn.process(out));
    CHECK(starts_with(out, "HTTP/1.1 200 OK\r\n"));
    CHECK_EQ(count(out, "HTTP/1.1 "), 1u);
    CHECK(contains(out, "Connection: keep-alive\r\n"));
    CHECK(contains(out, "{\"status\":\"ok\"}"));
    CHECK_EQ(conn.requests_served(), 1u);
    CHECK(!conn.has_pending_input());
    CHECK(!conn.mid_request());
    CHECK_EQ(f.stats.requests_total.load(), 1u);
    CHECK_EQ(f.stats.responses_2xx.load(), 1u);
}

TEST(http_conn_split_across_reads) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    const std::string req = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    std::string out;
    feed(conn, req.substr(0, 10));
    CHECK(conn.process(out));
    CHECK(out.empty());
    CHECK(conn.mid_request());
    feed(conn, req.substr(10, 40));
    CHECK(conn.process(out));
    CHECK(out.empty());
    CHECK(conn.mid_request());
    feed(conn, req.substr(50));
    CHECK(conn.process(out));
    CHECK(starts_with(out, "HTTP/1.1 200 OK\r\n"));
    CHECK(contains(out, "\r\n\r\nhello"));
    CHECK(!conn.mid_request());
}

TEST(http_conn_pipelined_in_order) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    feed(conn, kGetHealth + "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string out;
    CHECK(conn.process(out));
    size_t first = out.find("HTTP/1.1 200 OK");
    size_t second = out.find("HTTP/1.1 404 Not Found");
    CHECK_EQ(first, 0u);
    CHECK(second != std::string::npos);
    CHECK(second > first);
    CHECK_EQ(count(out, "HTTP/1.1 "), 2u);
    CHECK_EQ(conn.requests_served(), 2u);
    CHECK(!conn.has_pending_input());
}

TEST(http_conn_connection_close_request) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    feed(conn, "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" + kGetHealth);
    std::string out;
    CHECK(!conn.process(out));
    CHECK_EQ(count(out, "HTTP/1.1 "), 1u);
    CHECK(contains(out, "Connection: close\r\n"));
    CHECK(!conn.has_pending_input());  // leftover after a closing response is discarded
    // The connection is finished: more input is ignored.
    feed(conn, kGetHealth);
    std::string more;
    CHECK(!conn.process(more));
    CHECK(more.empty());
    CHECK_EQ(conn.requests_served(), 1u);
}

TEST(http_conn_parse_error_closes) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    feed(conn, "garbage garbage\r\n\r\n");
    std::string out;
    CHECK(!conn.process(out));
    CHECK(starts_with(out, "HTTP/1.1 400 Bad Request\r\n"));
    CHECK(contains(out, "Connection: close\r\n"));
    CHECK_EQ(f.stats.responses_4xx.load(), 1u);
}

TEST(http_conn_max_keepalive_requests) {
    Fixture f;
    f.config.max_keepalive_requests = 2;
    hs::HttpConn conn(f.router, f.config, f.stats);
    std::string out;
    feed(conn, kGetHealth);
    CHECK(conn.process(out));
    CHECK(contains(out, "Connection: keep-alive\r\n"));
    out.clear();
    feed(conn, kGetHealth);
    CHECK(!conn.process(out));
    CHECK(contains(out, "Connection: close\r\n"));
    CHECK_EQ(conn.requests_served(), 2u);
}

TEST(http_conn_draining_closes) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    conn.set_draining();
    feed(conn, kGetHealth);
    std::string out;
    CHECK(!conn.process(out));
    CHECK(starts_with(out, "HTTP/1.1 200 OK\r\n"));
    CHECK(contains(out, "Connection: close\r\n"));
}

TEST(http_conn_head_no_body) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    feed(conn, "HEAD /health HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string out;
    CHECK(conn.process(out));
    CHECK(starts_with(out, "HTTP/1.1 200 OK\r\n"));
    CHECK(contains(out, "Content-Length: 15\r\n"));
    CHECK(!contains(out, "status"));
    CHECK_EQ(out.substr(out.size() - 4), "\r\n\r\n");
}

TEST(http_conn_idle_not_mid_request) {
    Fixture f;
    hs::HttpConn fresh(f.router, f.config, f.stats);
    CHECK(!fresh.mid_request());
    CHECK(!fresh.has_pending_input());

    hs::HttpConn blank(f.router, f.config, f.stats);
    feed(blank, "\r\n");
    std::string out;
    CHECK(blank.process(out));
    CHECK(out.empty());
    CHECK(!blank.mid_request());

    // After a full keep-alive request the connection is idle again.
    feed(blank, kGetHealth);
    CHECK(blank.process(out));
    CHECK(!blank.mid_request());
}

TEST(http_conn_output_backpressure) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    std::string input;
    for (int i = 0; i < 100; ++i) input += "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    feed(conn, input);

    std::string out;
    CHECK(conn.process(out));
    CHECK(out.size() >= hs::kOutputHighWater);
    CHECK(out.size() < hs::kOutputHighWater + 70000);
    CHECK(conn.has_pending_input());

    size_t responses = count(out, "HTTP/1.1 200 OK");
    for (int round = 0; round < 200 && conn.has_pending_input(); ++round) {
        out.clear();
        CHECK(conn.process(out));
        responses += count(out, "HTTP/1.1 200 OK");
    }
    CHECK_EQ(responses, 100u);
    CHECK_EQ(conn.requests_served(), 100u);
    CHECK(!conn.has_pending_input());
}

TEST(http_conn_timeout_response_is_408_close) {
    Fixture f;
    hs::HttpConn conn(f.router, f.config, f.stats);
    std::string r = conn.timeout_response();
    CHECK(starts_with(r, "HTTP/1.1 408 Request Timeout\r\n"));
    CHECK(contains(r, "Connection: close\r\n"));
}

TEST(http_conn_access_log_line) {
    Fixture f;
    f.config.access_log = true;
    hs::HttpConn conn(f.router, f.config, f.stats);
    feed(conn, kGetHealth + "HEAD /health HTTP/1.1\r\nHost: x\r\n\r\n");

    // Capture stderr in a pipe while the requests are handled.
    int p[2];
    CHECK_EQ(pipe(p), 0);
    int saved = dup(STDERR_FILENO);
    dup2(p[1], STDERR_FILENO);
    close(p[1]);
    hs::LogLevel old_level = hs::log_level();
    hs::log_set_level(hs::LogLevel::Info);
    std::string out;
    conn.process(out);
    hs::log_set_level(old_level);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char buf[4096];
    ssize_t n = read(p[0], buf, sizeof(buf));
    close(p[0]);
    std::string logged(buf, n > 0 ? static_cast<size_t>(n) : 0);

    CHECK(contains(logged, "[INFO] GET /health 200 15\n"));
    CHECK(contains(logged, "[INFO] HEAD /health 200 0\n"));
}
