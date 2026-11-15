// loadgen: a closed-loop HTTP/1.1 load generator for benchmarking the server.
//
// Method, in short (the limits are spelled out in the project README):
//  * Closed loop. Each connection has exactly one request outstanding: send,
//    read the complete response, record the latency, send the next. The offered
//    load therefore slows down when the server does. The measured latency is
//    service time seen by a client that waits its turn; it does not include
//    queueing that an open-loop client with a fixed arrival rate would add, so
//    it understates tail latency under overload (coordinated omission).
//  * Latency of a request is the time from just before its first write() to the
//    moment the recv() that delivers its last response byte returns (the clock is
//    read right after recv, before the response is parsed, so parsing is not in
//    the latency; the response is only known to be complete after parsing). With
//    --no-keepalive the clock starts before connect(), so connect is included.
//    Reconnecting after a "Connection: close" reply on a keep-alive run happens
//    between requests and is not part of any request's latency.
//  * Setup comes first: every thread opens its idle connections and connects its
//    active ones, and only when all threads are done (a refused active connect
//    counts as done; it is retried and counted as an error during the run) does
//    the warm-up clock start, so the connect ramp never eats into the warm-up or
//    the measured window. If an idle connection cannot be opened, or setup does
//    not finish within --timeout, the run is aborted with exit status 2: it
//    would not be the benchmark that was asked for.
//  * Samples whose request started before the warm-up ended are discarded. The
//    measured window is -d seconds long; requests/s is the number of successful
//    responses completed inside it divided by -d. A request still in flight at
//    the end is neither a success nor an error.
//  * The client uses epoll so that a few threads can drive thousands of
//    connections; a thread-per-connection client would distort the measurement
//    with its own scheduling. Level-triggered mode is used because the client is
//    not the system under test and it is the simpler mode to get right.
//
// This file deliberately includes nothing from src/: it is an independent client.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "percentile.h"

namespace hs::bench {
namespace {

using Clock = std::chrono::steady_clock;  // CLOCK_MONOTONIC: unaffected by wall-clock changes
using TimePoint = Clock::time_point;

// ---- command line ----

struct Options {
    std::string host = "127.0.0.1";
    int port = 0;
    std::string path = "/";
    int connections = 1;
    double duration = 10;
    double warmup = 1;
    int threads = 1;
    bool keepalive = true;
    int idle = 0;
    double timeout = 5;
    bool csv = false;
    std::string label;
};

void print_usage(FILE* out) {
    std::fputs(
        "usage: loadgen --port N [--host 127.0.0.1] [--path /] [-c CONNS=1] [-d SECONDS=10]\n"
        "               [--warmup SECONDS=1] [-t THREADS=1] [--no-keepalive] [--idle N]\n"
        "               [--timeout SECONDS=5] [--csv] [--label TEXT]\n"
        "\n"
        "Closed-loop HTTP/1.1 load generator: every connection sends one GET, waits for the\n"
        "full response, records the latency and repeats. Samples from the warm-up are dropped.\n"
        "\n"
        "  --host H          IPv4 address or name of the server (default 127.0.0.1)\n"
        "  --port N          server port (required)\n"
        "  --path P          request target (default /)\n"
        "  -c N              concurrent connections, split over the threads (default 1)\n"
        "  -d S              measured seconds after the warm-up (default 10)\n"
        "  --warmup S        seconds run before measuring (default 1)\n"
        "  -t N              client threads, each with its own epoll loop (default 1)\n"
        "  --no-keepalive    new connection per request; latency includes the connect\n"
        "  --idle N          N extra connections that send nothing; reports idle_closed=\n"
        "  --timeout S       a request with no complete response after S seconds is an error\n"
        "  --csv             print one CSV line instead of the report:\n"
        "                    label,connections,idle,keepalive,requests,errors,duration_s,rps,\n"
        "                    p50_ms,p95_ms,p99_ms,max_ms\n"
        "  --label TEXT      first CSV field (no commas)\n"
        "\n"
        "Exit status: 0 success, 1 no successful request, 2 usage or setup error.\n",
        out);
}

bool parse_int(const char* text, long min, long max, long& out) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v < min || v > max) return false;
    out = v;
    return true;
}

bool parse_double(const char* text, double min, double max, double& out) {
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !(v >= min && v <= max)) return false;
    out = v;
    return true;
}

enum class Parse { Run, Help, Error };

// On Error, `message` says what was wrong with the command line.
Parse parse_args(int argc, char** argv, Options& o, std::string& message) {
    bool have_port = false;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--help" || name == "-h") return Parse::Help;
        if (name == "--no-keepalive") { o.keepalive = false; continue; }
        if (name == "--csv") { o.csv = true; continue; }

        if (i + 1 >= argc) { message = "option " + name + " needs a value"; return Parse::Error; }
        const char* value = argv[++i];
        long n = 0;
        double d = 0;
        bool ok = true;
        if (name == "--host") {
            o.host = value;
        } else if (name == "--port") {
            ok = parse_int(value, 1, 65535, n);
            if (ok) { o.port = static_cast<int>(n); have_port = true; }
        } else if (name == "--path") {
            o.path = value;
            ok = !o.path.empty() && o.path[0] == '/' && o.path.find_first_of(" \r\n") == std::string::npos;
        } else if (name == "-c") {
            ok = parse_int(value, 1, 1000000, n);
            if (ok) o.connections = static_cast<int>(n);
        } else if (name == "-d") {
            ok = parse_double(value, 0.001, 86400, d);
            if (ok) o.duration = d;
        } else if (name == "--warmup") {
            ok = parse_double(value, 0, 86400, d);
            if (ok) o.warmup = d;
        } else if (name == "-t") {
            ok = parse_int(value, 1, 1024, n);
            if (ok) o.threads = static_cast<int>(n);
        } else if (name == "--idle") {
            ok = parse_int(value, 0, 1000000, n);
            if (ok) o.idle = static_cast<int>(n);
        } else if (name == "--timeout") {
            ok = parse_double(value, 0.001, 86400, d);
            if (ok) o.timeout = d;
        } else if (name == "--label") {
            o.label = value;
            // The CSV is split on commas by whatever reads it, so a comma in
            // the label would silently shift every later column.
            ok = o.label.find_first_of(",\r\n") == std::string::npos;
        } else {
            message = "unknown option " + name;
            return Parse::Error;
        }
        if (!ok) { message = "invalid value '" + std::string(value) + "' for " + name; return Parse::Error; }
    }
    if (!have_port) { message = "--port is required"; return Parse::Error; }
    return Parse::Run;
}

// ---- response parsing helpers ----

// Looks up one header in `head` (status line plus header lines, up to and
// including the last CRLF before the blank line). `name` must be lower case.
bool find_header(std::string_view head, std::string_view name, std::string_view& value) {
    size_t pos = head.find("\r\n");  // skip the status line
    while (pos != std::string_view::npos && pos + 2 < head.size()) {
        const size_t start = pos + 2;
        const size_t eol = head.find("\r\n", start);
        const std::string_view line = head.substr(start, eol == std::string_view::npos ? eol : eol - start);
        const size_t colon = line.find(':');
        if (colon == name.size()) {
            bool same = true;
            for (size_t i = 0; i < colon && same; ++i) {
                const char c = line[i];
                same = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == name[i];
            }
            if (same) {
                std::string_view v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
                value = v;
                return true;
            }
        }
        pos = eol;
    }
    return false;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

// ---- per-thread results ----

enum ErrorKind { kConnect, kReset, kTimeout, kStatus, kProtocol, kErrorKinds };
const char* const kErrorNames[kErrorKinds] = {"connect", "reset", "timeout", "status", "protocol"};

struct Result {
    std::vector<uint64_t> latencies_ns;
    uint64_t errors = 0;
    uint64_t errors_by_kind[kErrorKinds] = {};
    uint64_t idle_closed = 0;
    uint64_t idle_failed = 0;
    std::string fatal;  // set when the thread had to give up (epoll failure)
};

// ---- one client thread ----

constexpr auto kScanInterval = std::chrono::milliseconds(10);
// A refused connect is retried after this pause so a dead server does not turn
// the client into a spinning loop.
constexpr auto kConnectRetryDelay = std::chrono::milliseconds(20);
constexpr size_t kMaxHeadBytes = 64 * 1024;
constexpr uint64_t kMaxBodyBytes = 1ull << 30;
// 1 M samples per thread (8 MB of address space, touched lazily) are reserved up
// front: a vector that has to grow in the middle of a run stalls this thread's
// connections while it copies, and that stall shows up as latency. The size is a
// fixed guess rather than derived from -d because the request rate is unknown in
// advance. A thread that records more than this still works, but the report
// warns that the affected samples may include one copy stall.
constexpr size_t kReserveSamples = 1u << 20;

enum class State { Connecting, Ready, Sending, Reading, Backoff, Idle };

struct Conn {
    int fd = -1;
    bool idle = false;       // an --idle connection: never sends
    State state = State::Backoff;
    uint32_t watching = 0;   // epoll events currently registered; 0 = not registered
    size_t sent = 0;         // bytes of the request already written
    std::string in;          // response bytes received so far
    size_t head_len = 0;     // length of status line + headers + blank line; 0 until seen
    uint64_t body_len = 0;
    int status = 0;
    bool server_closes = false;  // response said "Connection: close"
    TimePoint req_start;     // start of the request in flight (or of the connect attempt)
    TimePoint retry_at;      // when a Backoff connection may reconnect
};

// Lets every thread finish its setup before any of them starts measuring, and
// gives them one common start time.
class StartGate {
public:
    explicit StartGate(int parties) : parties_(parties) {}

    // Called by a thread whose setup succeeded. Blocks until all threads have
    // arrived. Returns false if some thread aborted instead.
    bool arrive() {
        std::unique_lock<std::mutex> lock(mu_);
        if (++arrived_ == parties_ && !aborted_) start_ = Clock::now();
        cv_.notify_all();
        cv_.wait(lock, [this] { return arrived_ == parties_ || aborted_; });
        return !aborted_;
    }

    // Called by a thread whose setup failed; releases the waiting threads.
    void abort() {
        std::lock_guard<std::mutex> lock(mu_);
        aborted_ = true;
        cv_.notify_all();
    }

    TimePoint start() const { return start_; }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    const int parties_;
    int arrived_ = 0;
    bool aborted_ = false;
    TimePoint start_;
};

class Worker {
public:
    Worker(const Options& o, const sockaddr_in& addr, std::string request, int connections, int idle)
        : opt_(o), addr_(addr), request_(std::move(request)),
          timeout_(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.timeout))),
          conns_(static_cast<size_t>(connections)), idle_(static_cast<size_t>(idle)) {
        result_.latencies_ns.reserve(kReserveSamples);
        for (Conn& c : idle_) c.idle = true;
    }

    ~Worker() {
        for (Conn& c : conns_) close_fd(c);
        for (Conn& c : idle_) close_fd(c);
        if (ep_ >= 0) ::close(ep_);
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    Result& result() { return result_; }

    // Opens the idle connections and connects the active ones, without sending
    // anything. Throws if an idle connection cannot be opened or if this takes
    // longer than --timeout. An active connect that fails is not fatal here: the
    // connection is retried once the run starts, and counted as an error there.
    void setup() {
        ep_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (ep_ < 0) throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));

        const TimePoint begin = Clock::now();
        for (Conn& c : idle_) open_idle(c);
        for (Conn& c : conns_) start_connect(c, begin);

        std::vector<epoll_event> events(256);
        while (connecting() > 0) {
            if (Clock::now() - begin > timeout_) {
                throw std::runtime_error(std::to_string(connecting()) + " connection(s) not established within " +
                                         std::to_string(opt_.timeout) + " s (--timeout); setup aborted");
            }
            const int n = ::epoll_wait(ep_, events.data(), static_cast<int>(events.size()), 10);
            if (n < 0 && errno != EINTR) throw std::runtime_error(std::string("epoll_wait: ") + std::strerror(errno));
            for (int i = 0; i < n; ++i) {
                on_event(*static_cast<Conn*>(events[static_cast<size_t>(i)].data.ptr), events[static_cast<size_t>(i)].events);
            }
        }
        if (result_.idle_failed > 0) {
            throw std::runtime_error(std::to_string(result_.idle_failed) +
                                     " idle connection(s) could not be opened; the run would not hold the requested --idle");
        }
    }

    // The measured run; call after setup() and after every thread has finished
    // its setup. `start` is the common start time.
    void run(TimePoint start) {
        warm_end_ = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(opt_.warmup));
        end_ = warm_end_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(opt_.duration));
        setting_up_ = false;
        const TimePoint begin = Clock::now();
        for (Conn& c : conns_) {
            if (c.state == State::Ready) begin_request(c);
        }

        std::vector<epoll_event> events(256);
        TimePoint next_scan = begin + kScanInterval;
        for (;;) {
            // While connections wait to reconnect, poll quickly; otherwise the
            // wake-up only exists so timeouts and the end of the run are noticed.
            const int wait_ms = backoff_.empty() ? static_cast<int>(kScanInterval.count()) : 1;
            const int n = ::epoll_wait(ep_, events.data(), static_cast<int>(events.size()), wait_ms);
            if (n < 0 && errno != EINTR) throw std::runtime_error(std::string("epoll_wait: ") + std::strerror(errno));
            for (int i = 0; i < n; ++i) {
                on_event(*static_cast<Conn*>(events[static_cast<size_t>(i)].data.ptr), events[static_cast<size_t>(i)].events);
            }
            const TimePoint now = Clock::now();
            reconnect_due(now);
            if (now >= next_scan) {
                check_timeouts(now);
                next_scan = now + kScanInterval;
            }
            if (now >= end_) break;
        }
    }

private:
    size_t connecting() const {
        size_t n = 0;
        for (const Conn& c : conns_) n += c.state == State::Connecting;
        for (const Conn& c : idle_) n += c.state == State::Connecting;
        return n;
    }

    // An active connection is established: during setup it waits for the run to
    // start, afterwards it sends its first request at once.
    void connected(Conn& c) {
        if (setting_up_) {
            c.state = State::Ready;
            watch(c, EPOLLIN);  // a close before the run starts is noticed
        } else {
            begin_request(c);
        }
    }

    // ---- sockets and epoll ----

    int make_socket() {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return fd;
    }

    // Starts a non-blocking connect. Returns 1 if it already succeeded, 0 if it
    // is in progress, -1 if it failed at once.
    int begin_connect(int fd) {
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr_), sizeof addr_) == 0) return 1;
        return errno == EINPROGRESS ? 0 : -1;
    }

    static int socket_error(int fd) {
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
        return err;
    }

    // Registers or updates the events this connection is watched for.
    void watch(Conn& c, uint32_t events) {
        if (c.watching == events) return;
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = &c;
        const int op = c.watching == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
        if (::epoll_ctl(ep_, op, c.fd, &ev) < 0) throw std::runtime_error(std::string("epoll_ctl: ") + std::strerror(errno));
        c.watching = events;
    }

    // Closing the fd also removes it from the epoll set (nothing dup()s it).
    static void close_fd(Conn& c) {
        if (c.fd >= 0) ::close(c.fd);
        c.fd = -1;
        c.watching = 0;
    }

    // ---- idle connections ----

    void open_idle(Conn& c) {
        c.fd = make_socket();
        const int rc = c.fd < 0 ? -1 : begin_connect(c.fd);
        if (rc < 0) {
            close_fd(c);
            ++result_.idle_failed;
            return;
        }
        if (rc == 1) {
            c.state = State::Idle;
            watch(c, EPOLLIN);
        } else {
            c.state = State::Connecting;
            watch(c, EPOLLOUT);
        }
    }

    void on_idle_event(Conn& c) {
        if (c.state == State::Connecting) {
            if (socket_error(c.fd) != 0) {
                close_fd(c);
                c.state = State::Backoff;  // no longer connecting
                ++result_.idle_failed;
                return;
            }
            c.state = State::Idle;
            watch(c, EPOLLIN);
            return;
        }
        // Anything the server sends is discarded; the only interesting event is
        // the close. Not reopened: the point is to see how the server treats an
        // idle client, and reopening would hide a timeout.
        char buf[512];
        for (;;) {
            const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) continue;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            close_fd(c);
            ++result_.idle_closed;
            return;
        }
    }

    // ---- active connections ----

    void schedule_reconnect(Conn& c, TimePoint at) {
        c.state = State::Backoff;
        c.retry_at = at;
        backoff_.push_back(&c);
    }

    // Counts an error (only if its request began inside the measured window),
    // closes the connection and queues it for a new one. Never reconnects
    // directly, so failures cannot recurse.
    void fail(Conn& c, ErrorKind kind, TimePoint now) {
        if (c.req_start >= warm_end_ && now <= end_) {
            ++result_.errors;
            ++result_.errors_by_kind[kind];
        }
        close_fd(c);
        schedule_reconnect(c, kind == kConnect ? now + kConnectRetryDelay : now);
    }

    void start_connect(Conn& c, TimePoint now) {
        c.in.clear();
        c.sent = 0;
        c.head_len = 0;
        // For --no-keepalive this is the start of the request, so the connect is
        // part of its latency. On keep-alive runs it is replaced just before the
        // first write, and only matters if the connect fails.
        c.req_start = now;
        c.fd = make_socket();
        if (c.fd < 0) {  // out of descriptors or memory
            fail(c, kConnect, now);
            return;
        }
        const int rc = begin_connect(c.fd);
        if (rc < 0) {
            fail(c, kConnect, Clock::now());
        } else if (rc == 1) {
            connected(c);
        } else {
            c.state = State::Connecting;
            watch(c, EPOLLOUT);
        }
    }

    void begin_request(Conn& c) {
        c.state = State::Sending;
        c.sent = 0;
        c.in.clear();
        c.head_len = 0;
        if (opt_.keepalive) c.req_start = Clock::now();  // just before the first write
        try_send(c);
    }

    // Writes as much of the request as the socket takes. Returns false if the
    // connection failed (it is then already queued for reconnect).
    bool try_send(Conn& c) {
        while (c.sent < request_.size()) {
            const ssize_t n = ::send(c.fd, request_.data() + c.sent, request_.size() - c.sent, MSG_NOSIGNAL);
            if (n > 0) {
                c.sent += static_cast<size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                watch(c, EPOLLIN | EPOLLOUT);  // resume when the socket is writable
                return true;
            } else {
                fail(c, kReset, Clock::now());
                return false;
            }
        }
        c.state = State::Reading;
        watch(c, EPOLLIN);
        return true;
    }

    void on_event(Conn& c, uint32_t ev) {
        if (c.idle) {
            on_idle_event(c);
            return;
        }
        switch (c.state) {
        case State::Connecting:
            if (socket_error(c.fd) != 0) {
                fail(c, kConnect, Clock::now());
            } else if (ev & EPOLLOUT) {
                connected(c);
            }
            return;
        case State::Ready:  // the server closed or sent something before we asked
            fail(c, kReset, Clock::now());
            return;
        case State::Sending:
            if ((ev & EPOLLOUT) && !try_send(c)) return;
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) on_readable(c);
            return;
        case State::Reading:
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) on_readable(c);
            return;
        case State::Backoff:
        case State::Idle:
            return;
        }
    }

    void on_readable(Conn& c) {
        char buf[32 * 1024];
        const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        // The clock is read right after recv returns: this is the moment the
        // last byte of the response is in hand.
        const TimePoint now = Clock::now();
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            fail(c, kReset, now);
            return;
        }
        if (n == 0) {  // closed before the response was complete
            fail(c, kReset, now);
            return;
        }
        c.in.append(buf, static_cast<size_t>(n));
        parse_response(c, now);
    }

    void parse_response(Conn& c, TimePoint now) {
        if (c.head_len == 0) {
            const size_t end = c.in.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (c.in.size() > kMaxHeadBytes) fail(c, kProtocol, now);
                return;
            }
            const std::string_view head(c.in.data(), end + 2);
            // "HTTP/1.1 200 ...": three digits after the first space.
            if (head.size() < 12 || head.compare(0, 5, "HTTP/") != 0 || head[8] != ' ' ||
                !(head[9] >= '0' && head[9] <= '9' && head[10] >= '0' && head[10] <= '9' &&
                  head[11] >= '0' && head[11] <= '9')) {
                fail(c, kProtocol, now);
                return;
            }
            c.status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');

            std::string_view length;
            char* parsed_end = nullptr;
            std::string length_text;
            if (!find_header(head, "content-length", length)) {
                fail(c, kProtocol, now);  // the server always sends it
                return;
            }
            length_text.assign(length);
            c.body_len = std::strtoull(length_text.c_str(), &parsed_end, 10);
            if (length_text.empty() || *parsed_end != '\0' || c.body_len > kMaxBodyBytes) {
                fail(c, kProtocol, now);
                return;
            }
            std::string_view connection;
            c.server_closes = find_header(head, "connection", connection) && iequals(connection, "close");
            c.head_len = end + 4;
        }

        const uint64_t total = c.head_len + c.body_len;
        if (c.in.size() > total) {  // closed loop: nothing else may arrive
            fail(c, kProtocol, now);
        } else if (c.in.size() == total) {
            complete(c, now);
        }
    }

    void complete(Conn& c, TimePoint now) {
        if (c.status < 200 || c.status > 299) {
            fail(c, kStatus, now);
            return;
        }
        if (c.req_start >= warm_end_ && now <= end_) {
            result_.latencies_ns.push_back(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - c.req_start).count()));
        }
        if (opt_.keepalive && !c.server_closes) {
            begin_request(c);
        } else {
            // Not an error: --no-keepalive, or the server's keep-alive request
            // limit was reached and it said so.
            close_fd(c);
            schedule_reconnect(c, now);
        }
    }

    void reconnect_due(TimePoint now) {
        if (backoff_.empty()) return;
        // start_connect may queue connections again, so work on a copy.
        due_.swap(backoff_);
        for (Conn* c : due_) {
            if (c->retry_at <= now) {
                start_connect(*c, now);
            } else {
                backoff_.push_back(c);
            }
        }
        due_.clear();
    }

    void check_timeouts(TimePoint now) {
        for (Conn& c : conns_) {
            if (c.state == State::Backoff) continue;
            if (now - c.req_start > timeout_) fail(c, kTimeout, now);
        }
    }

    const Options& opt_;
    const sockaddr_in addr_;
    const std::string request_;
    // Until run() starts, no request can be inside the window, so nothing that
    // happens during setup is counted.
    TimePoint warm_end_ = TimePoint::max();
    TimePoint end_ = TimePoint::max();
    bool setting_up_ = true;
    const Clock::duration timeout_;
    std::vector<Conn> conns_;  // never resized: epoll holds pointers into it
    std::vector<Conn> idle_;
    std::vector<Conn*> backoff_;
    std::vector<Conn*> due_;
    int ep_ = -1;
    Result result_;
};

// ---- setup ----

// Raises the soft descriptor limit to the hard one. Returns the resulting soft limit.
rlim_t raise_fd_limit() {
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) < 0) return 1024;
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < rl.rlim_max) {
        rlimit wanted = rl;
        // An "unlimited" hard limit cannot always be applied as a soft limit.
        wanted.rlim_cur = rl.rlim_max == RLIM_INFINITY ? 1u << 20 : rl.rlim_max;
        if (::setrlimit(RLIMIT_NOFILE, &wanted) == 0) rl = wanted;
    }
    return rl.rlim_cur;
}

double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }

int run(const Options& o) {
    sockaddr_in addr{};
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(o.host.c_str(), nullptr, &hints, &res);
        if (rc != 0 || res == nullptr) {
            std::fprintf(stderr, "loadgen: cannot resolve host '%s': %s\n", o.host.c_str(), ::gai_strerror(rc));
            return 2;
        }
        addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        ::freeaddrinfo(res);
        addr.sin_port = htons(static_cast<uint16_t>(o.port));
    }

    const int threads = std::min(o.threads, o.connections);  // a thread without connections is pointless
    const rlim_t limit = raise_fd_limit();
    // One epoll descriptor per thread plus some for stdio, the resolver and so on.
    const unsigned long long needed = static_cast<unsigned long long>(o.connections) +
                                      static_cast<unsigned long long>(o.idle) +
                                      static_cast<unsigned long long>(threads) + 16;
    if (limit != RLIM_INFINITY && needed > limit) {
        std::fprintf(stderr,
                     "loadgen: %d connections + %d idle need about %llu file descriptors but the limit is %llu "
                     "(raise it with ulimit -n or use fewer connections)\n",
                     o.connections, o.idle, needed, static_cast<unsigned long long>(limit));
        return 2;
    }

    const std::string request = "GET " + o.path + " HTTP/1.1\r\nHost: " + o.host + ":" + std::to_string(o.port) +
                                "\r\nUser-Agent: loadgen\r\nConnection: " + (o.keepalive ? "keep-alive" : "close") +
                                "\r\n\r\n";

    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < threads; ++i) {
        // Spread the remainder over the first threads.
        const int conns = o.connections / threads + (i < o.connections % threads ? 1 : 0);
        const int idle = o.idle / threads + (i < o.idle % threads ? 1 : 0);
        workers.push_back(std::make_unique<Worker>(o, addr, request, conns, idle));
    }
    StartGate gate(threads);
    std::vector<std::thread> pool;
    for (auto& w : workers) {
        Worker* worker = w.get();
        pool.emplace_back([worker, &gate] {
            try {
                worker->setup();
            } catch (const std::exception& e) {
                worker->result().fatal = e.what();
                gate.abort();  // do not leave the other threads waiting
                return;
            }
            if (!gate.arrive()) return;  // another thread's setup failed
            try {
                worker->run(gate.start());
            } catch (const std::exception& e) {
                worker->result().fatal = e.what();
            }
        });
    }
    for (auto& t : pool) t.join();

    // Merge and sort once, after the run, so no sorting happens while measuring.
    std::vector<uint64_t> all;
    uint64_t errors = 0, idle_closed = 0;
    bool grew = false;
    uint64_t by_kind[kErrorKinds] = {};
    for (auto& w : workers) {
        Result& r = w->result();
        if (!r.fatal.empty()) {
            std::fprintf(stderr, "loadgen: %s\n", r.fatal.c_str());
            return 2;
        }
        grew = grew || r.latencies_ns.size() > kReserveSamples;
        all.insert(all.end(), r.latencies_ns.begin(), r.latencies_ns.end());
        errors += r.errors;
        idle_closed += r.idle_closed;
        for (int k = 0; k < kErrorKinds; ++k) by_kind[k] += r.errors_by_kind[k];
    }
    std::sort(all.begin(), all.end());

    const uint64_t requests = all.size();
    const double rps = static_cast<double>(requests) / o.duration;
    const double p50 = ms(percentile_ns(all, 50));
    const double p95 = ms(percentile_ns(all, 95));
    const double p99 = ms(percentile_ns(all, 99));
    const double max = all.empty() ? 0.0 : ms(all.back());

    if (o.csv) {
        std::printf("%s,%d,%d,%d,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", o.label.c_str(), o.connections, o.idle,
                    o.keepalive ? 1 : 0, static_cast<unsigned long long>(requests),
                    static_cast<unsigned long long>(errors), o.duration, rps, p50, p95, p99, max);
        // stdout carries exactly one line; anything else goes to stderr.
        if (idle_closed > 0) std::fprintf(stderr, "loadgen: idle_closed=%llu\n", static_cast<unsigned long long>(idle_closed));
    } else {
        std::printf("target:      %s:%d%s (%s)\n", o.host.c_str(), o.port, o.path.c_str(),
                    o.keepalive ? "keep-alive" : "new connection per request");
        std::printf("connections: %d (threads %d, idle %d)\n", o.connections, threads, o.idle);
        std::printf("requests:    %llu\n", static_cast<unsigned long long>(requests));
        std::printf("errors:      %llu", static_cast<unsigned long long>(errors));
        if (errors > 0) {
            const char* sep = " (";
            for (int k = 0; k < kErrorKinds; ++k) {
                if (by_kind[k] == 0) continue;
                std::printf("%s%s %llu", sep, kErrorNames[k], static_cast<unsigned long long>(by_kind[k]));
                sep = ", ";
            }
            std::printf(")");
        }
        std::printf("\n");
        std::printf("duration:    %.3f s (after %.3f s warm-up)\n", o.duration, o.warmup);
        std::printf("requests/s:  %.3f\n", rps);
        std::printf("p50:         %.3f ms\n", p50);
        std::printf("p95:         %.3f ms\n", p95);
        std::printf("p99:         %.3f ms\n", p99);
        std::printf("max:         %.3f ms\n", max);
        std::printf("idle_closed=%llu\n", static_cast<unsigned long long>(idle_closed));
    }
    if (grew) {
        std::fprintf(stderr, "loadgen: warning: a thread recorded more than %zu samples; its sample buffer grew "
                             "during the run and a few latencies may include the copy\n", kReserveSamples);
    }
    return requests == 0 ? 1 : 0;
}

}  // namespace
}  // namespace hs::bench

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);  // sends also use MSG_NOSIGNAL; this is the second line of defence
    hs::bench::Options options;
    std::string message;
    switch (hs::bench::parse_args(argc, argv, options, message)) {
    case hs::bench::Parse::Help:
        hs::bench::print_usage(stdout);
        return 0;
    case hs::bench::Parse::Error:
        std::fprintf(stderr, "loadgen: %s (see --help)\n", message.c_str());
        return 2;
    case hs::bench::Parse::Run:
        break;
    }
    return hs::bench::run(options);
}
