#include "config.h"

#include <cmath>
#include <cstdlib>
#include <thread>

namespace hs {

unsigned Config::default_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

namespace {

// Digits only: no sign, no whitespace, no trailing junk. strtoul would accept
// "-1" (wrapping to a huge value) and " 5", so the check is done by hand.
bool parse_uint(const std::string& s, unsigned long long max, unsigned long long& out) {
    if (s.empty()) return false;
    unsigned long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        unsigned digit = static_cast<unsigned>(c - '0');
        // Checked before multiplying so the arithmetic itself can never overflow.
        if (v > (max - digit) / 10) return false;
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

// Seconds as "digits" or "digits.digits", converted to milliseconds. Hand-checked
// for the same reasons as parse_uint, and because strtod accepts "nan", "inf" and hex.
bool parse_seconds(const std::string& s, std::chrono::milliseconds& out) {
    constexpr double kMaxSeconds = 86400;
    bool seen_digit = false, seen_dot = false;
    for (char c : s) {
        if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else if (c >= '0' && c <= '9') {
            seen_digit = true;
        } else {
            return false;
        }
    }
    if (!seen_digit) return false;
    // strtod rather than stod: stod throws on out-of-range input, and a
    // 400-digit value must produce a normal "invalid value" error, not an exception.
    // Overflow returns HUGE_VAL (above the limit, so rejected below). Underflow
    // (e.g. "0.<400 zeros>1") returns a value near 0 with ERANGE; that is accepted
    // and rounds to 0 ms, since it is a valid non-negative number of seconds.
    char* end = nullptr;
    double seconds = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    if (!(seconds <= kMaxSeconds)) return false;
    out = std::chrono::milliseconds(std::llround(seconds * 1000));
    return true;
}

}  // namespace

ConfigResult parse_args(int argc, const char* const* argv) {
    ConfigResult result;
    Config& c = result.config;

    auto fail = [&](const std::string& flag, const std::string& why) {
        result.ok = false;
        result.error = flag + ": " + why;
        return result;
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) return fail(arg, "unexpected argument");

        // Accept both "--name value" and "--name=value".
        std::string name = arg, value;
        bool has_value = false;
        size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            has_value = true;
        }

        if (name == "--help") {
            if (has_value) return fail(name, "takes no value");
            result.help = true;
            result.ok = true;
            return result;
        }
        if (name == "--access-log") {
            if (has_value) return fail(name, "takes no value");
            c.access_log = true;
            continue;
        }

        static const char* const kValueFlags[] = {
            "--mode", "--bind", "--port", "--threads", "--root", "--backlog", "--queue-capacity",
            "--request-timeout", "--keepalive-timeout", "--write-timeout", "--shutdown-grace",
            "--max-keepalive-requests", "--max-header-bytes", "--max-body-bytes", "--log-level"};
        bool known = false;
        for (const char* f : kValueFlags) known = known || name == f;
        if (!known) return fail(name, "unknown flag");

        if (!has_value) {
            if (i + 1 >= argc) return fail(name, "missing value");
            value = argv[++i];
        }
        if (value.empty()) return fail(name, "missing value");

        unsigned long long n = 0;
        std::chrono::milliseconds ms{0};
        if (name == "--mode") {
            if (value == "pool") c.mode = Mode::Pool;
            else if (value == "epoll") c.mode = Mode::Epoll;
            else return fail(name, "expected pool or epoll");
        } else if (name == "--bind") {
            c.bind = value;
        } else if (name == "--root") {
            c.root = value;
        } else if (name == "--log-level") {
            if (value == "debug") c.log_level = LogLevel::Debug;
            else if (value == "info") c.log_level = LogLevel::Info;
            else if (value == "warn") c.log_level = LogLevel::Warn;
            else if (value == "error") c.log_level = LogLevel::Error;
            else return fail(name, "expected debug, info, warn or error");
        } else if (name == "--request-timeout" || name == "--keepalive-timeout" ||
                   name == "--write-timeout" || name == "--shutdown-grace") {
            if (!parse_seconds(value, ms)) return fail(name, "expected seconds between 0 and 86400");
            if (name == "--request-timeout") c.request_timeout = ms;
            else if (name == "--keepalive-timeout") c.keepalive_timeout = ms;
            else if (name == "--write-timeout") c.write_timeout = ms;
            else c.shutdown_grace = ms;
        } else if (name == "--port") {
            if (!parse_uint(value, 65535, n)) return fail(name, "expected 0-65535");
            c.port = static_cast<uint16_t>(n);
        } else if (name == "--threads") {
            if (!parse_uint(value, 1024, n) || n < 1) return fail(name, "expected 1-1024");
            c.threads = static_cast<unsigned>(n);
        } else if (name == "--backlog") {
            if (!parse_uint(value, 65535, n) || n < 1) return fail(name, "expected 1-65535");
            c.backlog = static_cast<int>(n);
        } else if (name == "--queue-capacity") {
            if (!parse_uint(value, 1ULL << 32, n) || n < 1) return fail(name, "expected at least 1");
            c.queue_capacity = static_cast<size_t>(n);
        } else if (name == "--max-keepalive-requests") {
            if (!parse_uint(value, 1ULL << 31, n) || n < 1) return fail(name, "expected at least 1");
            c.max_keepalive_requests = static_cast<unsigned>(n);
        } else if (name == "--max-header-bytes") {
            if (!parse_uint(value, 1ULL << 32, n) || n < 64) return fail(name, "expected at least 64");
            c.max_header_bytes = static_cast<size_t>(n);
        } else if (name == "--max-body-bytes") {
            if (!parse_uint(value, 1ULL << 40, n)) return fail(name, "expected a non-negative number");
            c.max_body_bytes = static_cast<size_t>(n);
        }
    }
    result.ok = true;
    return result;
}

std::string usage() {
    return
        "Usage: httpserver [options]\n"
        "  --mode pool|epoll          concurrency model (default: epoll)\n"
        "  --bind ADDR                address to listen on (default: 0.0.0.0)\n"
        "  --port N                   TCP port, 0 picks a free one (default: 8080)\n"
        "  --threads N                worker threads, 1-1024 (default: CPU count)\n"
        "  --root DIR                 static file root (default: ./www)\n"
        "  --backlog N                listen backlog, 1-65535 (default: 1024)\n"
        "  --queue-capacity N         pool mode connection queue size (default: 1024)\n"
        "  --request-timeout SEC      time to receive a full request (default: 10)\n"
        "  --keepalive-timeout SEC    idle time between requests (default: 5)\n"
        "  --write-timeout SEC        time to send a response (default: 10)\n"
        "  --shutdown-grace SEC       time to finish in-flight requests on exit (default: 5)\n"
        "  --max-keepalive-requests N requests per connection (default: 1000)\n"
        "  --max-header-bytes N       request head size limit, at least 64 (default: 8192)\n"
        "  --max-body-bytes N         request body size limit (default: 1048576)\n"
        "  --log-level LEVEL          debug|info|warn|error (default: info)\n"
        "  --access-log               log one line per request (default: off)\n"
        "  --help                     show this text\n"
        "Timeouts are in seconds and may be fractional, e.g. 0.5; maximum 86400.\n";
}

}  // namespace hs
