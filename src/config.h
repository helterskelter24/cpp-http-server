#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "logger.h"

namespace hs {

enum class Mode { Pool, Epoll };

struct Config {
    Mode mode = Mode::Epoll;
    std::string bind = "0.0.0.0";
    uint16_t port = 8080;  // 0 means the kernel picks a free port
    unsigned threads = default_threads();
    std::string root = "./www";
    int backlog = 1024;
    size_t queue_capacity = 1024;
    std::chrono::milliseconds request_timeout{10000}, keepalive_timeout{5000},
        write_timeout{10000}, shutdown_grace{5000};
    unsigned max_keepalive_requests = 1000;
    size_t max_header_bytes = 8192, max_body_bytes = 1048576;
    LogLevel log_level = LogLevel::Info;
    bool access_log = false;

    // hardware_concurrency() may return 0 when the count is unknown.
    static unsigned default_threads();
};

struct ConfigResult {
    bool ok = false;
    bool help = false;
    Config config;
    std::string error;  // names the offending flag when ok is false
};

// argv includes the program name at index 0, which is skipped.
ConfigResult parse_args(int argc, const char* const* argv);

std::string usage();

}  // namespace hs
