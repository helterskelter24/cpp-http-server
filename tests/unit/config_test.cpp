#include "config.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "test.h"

namespace {

// parse_args takes argv including the program name, so the helper adds it.
hs::ConfigResult parse(std::vector<std::string> args) {
    std::vector<const char*> argv{"httpserver"};
    for (const auto& a : args) argv.push_back(a.c_str());
    return hs::parse_args(static_cast<int>(argv.size()), argv.data());
}

bool error_names(const hs::ConfigResult& r, const std::string& flag) {
    return !r.ok && r.error.find(flag) != std::string::npos;
}

}  // namespace

TEST(config_defaults) {
    auto r = parse({});
    CHECK(r.ok);
    CHECK(!r.help);
    const hs::Config& c = r.config;
    unsigned hw = std::thread::hardware_concurrency();
    CHECK(c.mode == hs::Mode::Epoll);
    CHECK_EQ(c.bind, std::string("0.0.0.0"));
    CHECK_EQ(c.port, 8080);
    CHECK_EQ(c.threads, hw == 0 ? 1u : hw);
    CHECK_EQ(c.root, std::string("./www"));
    CHECK_EQ(c.backlog, 1024);
    CHECK_EQ(c.queue_capacity, size_t{1024});
    CHECK_EQ(c.request_timeout.count(), 10000);
    CHECK_EQ(c.keepalive_timeout.count(), 5000);
    CHECK_EQ(c.write_timeout.count(), 10000);
    CHECK_EQ(c.shutdown_grace.count(), 5000);
    CHECK_EQ(c.max_keepalive_requests, 1000u);
    CHECK_EQ(c.max_header_bytes, size_t{8192});
    CHECK_EQ(c.max_body_bytes, size_t{1048576});
    CHECK(c.log_level == hs::LogLevel::Info);
    CHECK(!c.access_log);
}

TEST(config_parses_all_flags_separate_value) {
    auto r = parse({"--mode",
                    "pool",
                    "--bind",
                    "127.0.0.1",
                    "--port",
                    "9090",
                    "--threads",
                    "3",
                    "--root",
                    "/srv/www",
                    "--backlog",
                    "128",
                    "--queue-capacity",
                    "16",
                    "--request-timeout",
                    "2",
                    "--keepalive-timeout",
                    "3",
                    "--write-timeout",
                    "4",
                    "--shutdown-grace",
                    "6",
                    "--max-keepalive-requests",
                    "7",
                    "--max-header-bytes",
                    "512",
                    "--max-body-bytes",
                    "0",
                    "--log-level",
                    "debug",
                    "--access-log"});
    CHECK(r.ok);
    const hs::Config& c = r.config;
    CHECK(c.mode == hs::Mode::Pool);
    CHECK_EQ(c.bind, std::string("127.0.0.1"));
    CHECK_EQ(c.port, 9090);
    CHECK_EQ(c.threads, 3u);
    CHECK_EQ(c.root, std::string("/srv/www"));
    CHECK_EQ(c.backlog, 128);
    CHECK_EQ(c.queue_capacity, size_t{16});
    CHECK_EQ(c.request_timeout.count(), 2000);
    CHECK_EQ(c.keepalive_timeout.count(), 3000);
    CHECK_EQ(c.write_timeout.count(), 4000);
    CHECK_EQ(c.shutdown_grace.count(), 6000);
    CHECK_EQ(c.max_keepalive_requests, 7u);
    CHECK_EQ(c.max_header_bytes, size_t{512});
    CHECK_EQ(c.max_body_bytes, size_t{0});
    CHECK(c.log_level == hs::LogLevel::Debug);
    CHECK(c.access_log);
}

TEST(config_parses_all_flags_equals_form) {
    auto r = parse({"--mode=epoll", "--bind=::1", "--port=0", "--threads=5", "--root=/x",
                    "--backlog=64", "--queue-capacity=8", "--request-timeout=1.5",
                    "--keepalive-timeout=2", "--write-timeout=3", "--shutdown-grace=0.25",
                    "--max-keepalive-requests=9", "--max-header-bytes=64", "--max-body-bytes=100",
                    "--log-level=error", "--access-log"});
    CHECK(r.ok);
    const hs::Config& c = r.config;
    CHECK(c.mode == hs::Mode::Epoll);
    CHECK_EQ(c.bind, std::string("::1"));
    CHECK_EQ(c.port, 0);
    CHECK_EQ(c.threads, 5u);
    CHECK_EQ(c.root, std::string("/x"));
    CHECK_EQ(c.backlog, 64);
    CHECK_EQ(c.queue_capacity, size_t{8});
    CHECK_EQ(c.request_timeout.count(), 1500);
    CHECK_EQ(c.shutdown_grace.count(), 250);
    CHECK_EQ(c.max_keepalive_requests, 9u);
    CHECK_EQ(c.max_header_bytes, size_t{64});
    CHECK_EQ(c.max_body_bytes, size_t{100});
    CHECK(c.log_level == hs::LogLevel::Error);
    CHECK(c.access_log);
}

TEST(config_fractional_seconds) {
    auto r = parse({"--request-timeout", "0.5"});
    CHECK(r.ok);
    CHECK(r.config.request_timeout == std::chrono::milliseconds(500));
    auto z = parse({"--keepalive-timeout", "0"});
    CHECK(z.ok);
    CHECK_EQ(z.config.keepalive_timeout.count(), 0);
}

TEST(config_rejects_unknown_flag) {
    auto r = parse({"--frobnicate", "1"});
    CHECK(error_names(r, "--frobnicate"));
    auto p = parse({"stray"});
    CHECK(!p.ok);
}

TEST(config_rejects_missing_value) {
    auto r = parse({"--port"});
    CHECK(error_names(r, "--port"));
    auto e = parse({"--root="});  // empty value is also missing for strings
    CHECK(error_names(e, "--root"));
    auto a = parse({"--access-log=yes"});  // flag takes no value
    CHECK(error_names(a, "--access-log"));
}

TEST(config_rejects_bad_numbers) {
    CHECK(error_names(parse({"--port", "70000"}), "--port"));
    CHECK(error_names(parse({"--port", "abc"}), "--port"));
    CHECK(error_names(parse({"--port", "80x"}), "--port"));
    CHECK(error_names(parse({"--port", ""}), "--port"));
    CHECK(error_names(parse({"--port", "-1"}), "--port"));
    CHECK(error_names(parse({"--port", "99999999999999999999"}), "--port"));
    CHECK(error_names(parse({"--threads", "0"}), "--threads"));
    CHECK(error_names(parse({"--threads", "1025"}), "--threads"));
    CHECK(error_names(parse({"--request-timeout", "-1"}), "--request-timeout"));
    CHECK(error_names(parse({"--request-timeout", "1x"}), "--request-timeout"));
    CHECK(error_names(parse({"--write-timeout", "86401"}), "--write-timeout"));
    CHECK(error_names(parse({"--keepalive-timeout", "nan"}), "--keepalive-timeout"));
    CHECK(error_names(parse({"--shutdown-grace", "inf"}), "--shutdown-grace"));
    CHECK(error_names(parse({"--backlog", "0"}), "--backlog"));
    CHECK(error_names(parse({"--backlog", "65536"}), "--backlog"));
    CHECK(error_names(parse({"--queue-capacity", "0"}), "--queue-capacity"));
    CHECK(error_names(parse({"--max-keepalive-requests", "0"}), "--max-keepalive-requests"));
    CHECK(error_names(parse({"--max-header-bytes", "63"}), "--max-header-bytes"));
    CHECK(error_names(parse({"--max-body-bytes", "-5"}), "--max-body-bytes"));
}

TEST(config_accepts_boundaries) {
    CHECK(parse({"--port", "65535"}).ok);
    CHECK(parse({"--port", "0"}).ok);
    CHECK(parse({"--threads", "1024"}).ok);
    CHECK(parse({"--request-timeout", "86400"}).ok);
    CHECK(parse({"--backlog", "65535"}).ok);
    CHECK(parse({"--max-header-bytes", "64"}).ok);
}

TEST(config_rejects_bad_mode_and_level) {
    CHECK(error_names(parse({"--mode", "fork"}), "--mode"));
    CHECK(error_names(parse({"--log-level", "loud"}), "--log-level"));
}

TEST(config_help_flag) {
    auto r = parse({"--help"});
    CHECK(r.ok);
    CHECK(r.help);
}

TEST(config_usage_lists_every_flag) {
    std::string u = hs::usage();
    for (const char* flag :
         {"--mode", "--bind", "--port", "--threads", "--root", "--backlog", "--queue-capacity",
          "--request-timeout", "--keepalive-timeout", "--write-timeout", "--shutdown-grace",
          "--max-keepalive-requests", "--max-header-bytes", "--max-body-bytes", "--log-level",
          "--access-log", "--help"}) {
        CHECK(u.find(flag) != std::string::npos);
    }
    CHECK(u.find("8080") != std::string::npos);
    CHECK(u.find("8192") != std::string::npos);
}

TEST(config_rejects_huge_timeout_without_throwing) {
    // Used to escape parse_args as std::out_of_range from stod.
    std::string huge(400, '9');
    CHECK(error_names(parse({"--request-timeout", huge}), "--request-timeout"));
    CHECK(error_names(parse({"--request-timeout=" + huge + ".5"}), "--request-timeout"));
    CHECK(error_names(parse({"--write-timeout", "1" + std::string(400, '0')}), "--write-timeout"));
    CHECK(error_names(parse({"--port", huge}), "--port"));
    CHECK(error_names(parse({"--max-body-bytes", huge}), "--max-body-bytes"));
}

TEST(config_tiny_timeout_underflows_to_zero) {
    // Valid non-negative number of seconds; rounds to 0 ms instead of crashing.
    auto r = parse({"--request-timeout", "0." + std::string(400, '0') + "1"});
    CHECK(r.ok);
    CHECK_EQ(r.config.request_timeout.count(), 0);
}
