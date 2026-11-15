#include "logger.h"

#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "test.h"

namespace {

// Points stderr at a pipe for the lifetime of the object so the test can read
// back exactly what the logger wrote. The read end is drained only in take(),
// so tests must keep the output well under the pipe capacity (64 KiB).
class StderrCapture {
public:
    StderrCapture() {
        if (pipe(fds_) != 0) return;
        saved_ = dup(STDERR_FILENO);
        dup2(fds_[1], STDERR_FILENO);
    }
    ~StderrCapture() { restore(); }

    std::string take() {
        restore();
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(fds_[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
        return out;
    }

private:
    void restore() {
        if (saved_ < 0) return;
        dup2(saved_, STDERR_FILENO);
        close(saved_);
        saved_ = -1;
        close(fds_[1]);  // closing the write end makes read() see EOF
    }
    int fds_[2] = {-1, -1};
    int saved_ = -1;
};

// True if `line` (without its newline) starts with "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] "
// and returns the rest in `rest`. Hand-written instead of std::regex, which trips a
// GCC -Wmaybe-uninitialized false positive in the sanitizer builds.
bool split_line(const std::string& line, const std::string& level, std::string& rest) {
    static const char kShape[] = "dddd-dd-dd dd:dd:dd.ddd ";  // d = any digit
    const size_t n = sizeof kShape - 1;
    if (line.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (kShape[i] == 'd' ? !(line[i] >= '0' && line[i] <= '9') : line[i] != kShape[i])
            return false;
    }
    std::string tag = "[" + level + "] ";
    if (line.compare(n, tag.size(), tag) != 0) return false;
    rest = line.substr(n + tag.size());
    return true;
}

}  // namespace

TEST(logger_line_format) {
    hs::log_set_level(hs::LogLevel::Debug);
    StderrCapture cap;
    hs::log_msg(hs::LogLevel::Info, "hello %d %s", 42, "world");
    std::string out = cap.take();
    CHECK(!out.empty() && out.back() == '\n');
    std::string rest;
    CHECK(split_line(out.substr(0, out.size() - 1), "INFO", rest));
    CHECK_EQ(rest, std::string("hello 42 world"));
    hs::log_set_level(hs::LogLevel::Info);
}

TEST(logger_level_names) {
    hs::log_set_level(hs::LogLevel::Debug);
    StderrCapture cap;
    hs::log_msg(hs::LogLevel::Debug, "a");
    hs::log_msg(hs::LogLevel::Info, "b");
    hs::log_msg(hs::LogLevel::Warn, "c");
    hs::log_msg(hs::LogLevel::Error, "d");
    std::string out = cap.take();
    CHECK(out.find("[DEBUG] a\n") != std::string::npos);
    CHECK(out.find("[INFO] b\n") != std::string::npos);
    CHECK(out.find("[WARN] c\n") != std::string::npos);
    CHECK(out.find("[ERROR] d\n") != std::string::npos);
    hs::log_set_level(hs::LogLevel::Info);
}

TEST(logger_filters_below_level) {
    hs::log_set_level(hs::LogLevel::Warn);
    CHECK(hs::log_level() == hs::LogLevel::Warn);
    StderrCapture cap;
    hs::log_msg(hs::LogLevel::Debug, "dropped-debug");
    hs::log_msg(hs::LogLevel::Info, "dropped-info");
    hs::log_msg(hs::LogLevel::Warn, "kept-warn");
    hs::log_msg(hs::LogLevel::Error, "kept-error");
    std::string out = cap.take();
    CHECK(out.find("dropped") == std::string::npos);
    CHECK(out.find("kept-warn") != std::string::npos);
    CHECK(out.find("kept-error") != std::string::npos);
    hs::log_set_level(hs::LogLevel::Info);
}

TEST(logger_truncates_long_message) {
    hs::log_set_level(hs::LogLevel::Info);
    StderrCapture cap;
    std::string big(10000, 'x');
    hs::log_msg(hs::LogLevel::Info, "%s", big.c_str());
    std::string out = cap.take();
    CHECK(out.size() < 5000);
    CHECK(out.size() > 100);
    CHECK(out.back() == '\n');
    CHECK(out.find("\n") == out.size() - 1);  // still exactly one line
}

TEST(logger_threads_do_not_interleave) {
    hs::log_set_level(hs::LogLevel::Info);
    StderrCapture cap;
    const int kThreads = 4, kPerThread = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i)
                hs::log_msg(hs::LogLevel::Info, "thread=%d seq=%d end", t, i);
        });
    }
    for (auto& th : threads) th.join();
    std::string out = cap.take();
    int lines = 0;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string rest;
        CHECK(split_line(out.substr(pos, nl - pos), "INFO", rest));
        CHECK(rest.rfind("thread=", 0) == 0 && rest.size() > 4 &&
              rest.compare(rest.size() - 4, 4, " end") == 0);
        ++lines;
        pos = nl + 1;
    }
    CHECK_EQ(lines, kThreads * kPerThread);
}

TEST(logger_escapes_control_bytes) {
    hs::log_set_level(hs::LogLevel::Info);
    StderrCapture cap;
    // A decoded request path could carry these; they must not forge a second line.
    hs::log_msg(hs::LogLevel::Info, "path=%s",
                "/a\nFAKE [ERROR] x\r\x1b[31m\x7f"
                "end");
    std::string out = cap.take();
    CHECK(out.find('\n') == out.size() - 1);
    CHECK(out.find('\r') == std::string::npos);
    CHECK(out.find('\x1b') == std::string::npos);
    CHECK(out.find("path=/a\\x0aFAKE [ERROR] x\\x0d\\x1b[31m\\x7fend\n") != std::string::npos);
}

TEST(logger_escaping_respects_truncation) {
    hs::log_set_level(hs::LogLevel::Info);
    StderrCapture cap;
    std::string nasty(5000, '\n');  // every byte expands to four
    hs::log_msg(hs::LogLevel::Info, "%s", nasty.c_str());
    std::string out = cap.take();
    CHECK(out.size() <= 2048);
    CHECK(out.back() == '\n');
    CHECK(out.find('\n') == out.size() - 1);
    // Cut on an escape boundary: the text before the newline ends with a whole \x0a.
    CHECK(out.substr(out.size() - 5) == "\\x0a\n");
}
