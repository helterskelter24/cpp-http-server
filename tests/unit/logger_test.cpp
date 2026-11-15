#include <unistd.h>

#include <cstdio>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"
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

}  // namespace

TEST(logger_line_format) {
    hs::log_set_level(hs::LogLevel::Debug);
    StderrCapture cap;
    hs::log_msg(hs::LogLevel::Info, "hello %d %s", 42, "world");
    std::string out = cap.take();
    std::regex re(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[INFO\] hello 42 world\n)");
    CHECK(std::regex_match(out, re));
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
    std::regex re(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[INFO\] thread=\d seq=\d+ end)");
    int lines = 0;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) break;
        CHECK(std::regex_match(out.substr(pos, nl - pos), re));
        ++lines;
        pos = nl + 1;
    }
    CHECK_EQ(lines, kThreads * kPerThread);
}
