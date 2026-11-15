#include "logger.h"

#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace hs {

namespace {

// Atomic so threads can log while another one changes the level, without a lock
// on the hot path of every (usually dropped) debug call.
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

// Lines longer than this are cut. A fixed stack buffer keeps logging free of
// heap allocation and lets the whole line go out in one write(2).
constexpr size_t kMaxLine = 2048;

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

void log_set_level(LogLevel level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

LogLevel log_level() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

void log_msg(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;

    char buf[kMaxLine];

    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // localtime_r, not localtime: the latter returns a shared static buffer.
    std::tm tm_parts;
    localtime_r(&ts.tv_sec, &tm_parts);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm_parts);

    int n = snprintf(buf, sizeof buf, "%s.%03d [%s] ", stamp, static_cast<int>(ts.tv_nsec / 1000000),
                     level_name(level));
    if (n < 0) return;
    size_t len = static_cast<size_t>(n);

    // Reserve one byte for the trailing newline so a truncated line still ends in one.
    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(buf + len, sizeof buf - len - 1, fmt, args);
    va_end(args);
    if (m < 0) m = 0;
    // vsnprintf returns the length it wanted, not what fit.
    size_t room = sizeof buf - len - 1;
    len += static_cast<size_t>(m) < room ? static_cast<size_t>(m) : room - 1;
    buf[len++] = '\n';

    // One write keeps the line contiguous even when other threads log at once.
    // The result is ignored on purpose: there is nowhere to report a failed log write.
    ssize_t ignored = write(STDERR_FILENO, buf, len);
    (void)ignored;
}

}  // namespace hs
