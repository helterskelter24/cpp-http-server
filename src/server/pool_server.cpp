#include "server/pool_server.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <exception>
#include <utility>

#include "http/response.h"
#include "logger.h"
#include "server/http_conn.h"

namespace hs {

namespace {

// Blocking calls wake up at least this often, so a worker notices shutdown and
// expired deadlines without anything having to interrupt it.
constexpr long kTickMicros = 100 * 1000;
constexpr size_t kReadChunk = 16 * 1024;
// How long the acceptor pauses after an accept failure such as EMFILE. The
// pending connection stays in the backlog and poll() would report it again at
// once, so without a pause the acceptor would spin at full CPU.
constexpr auto kAcceptBackoff = std::chrono::milliseconds(50);

void set_tick_timeouts(int fd) {
    timeval tick{0, kTickMicros};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof(tick));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tick, sizeof(tick));
}

}  // namespace

PoolServer::PoolServer(const Config& config, const Router& router, Stats& stats)
    : config_(config), router_(router), stats_(stats) {}

PoolServer::~PoolServer() {
    stop();
    join();
}

bool PoolServer::start(std::string& error) {
    // Non-blocking listen socket: if a client resets between poll() reporting
    // it and accept4(), a blocking accept would hang until the next client.
    ListenResult listening =
        listen_tcp(config_.bind, config_.port, config_.backlog, /*reuse_port=*/false,
                   /*non_blocking=*/true);
    if (!listening.error.empty()) {
        error = listening.error;
        return false;
    }
    wake_fd_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_fd_.valid()) {
        error = std::string("eventfd: ") + strerror(errno);
        return false;
    }
    listen_fd_ = std::move(listening.fd);
    port_ = local_port(listen_fd_.get());
    pool_ = std::make_unique<ThreadPool>(config_.threads, config_.queue_capacity);
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void PoolServer::stop() {
    std::lock_guard<std::mutex> lock(stop_mu_);
    if (stopping_.load()) return;
    grace_deadline_ = Clock::now() + config_.shutdown_grace;
    stopping_.store(true);
    uint64_t one = 1;
    // Fails harmlessly (bad fd) if start() never ran.
    ssize_t ignored = write(wake_fd_.get(), &one, sizeof(one));
    (void)ignored;
}

void PoolServer::join() {
    if (acceptor_.joinable()) acceptor_.join();
    // Lets connections finish (each worker watches stopping_ and the grace
    // deadline) and runs the tasks still queued, which close their sockets.
    if (pool_) pool_->shutdown();
}

bool PoolServer::past_grace(Clock::time_point now) const {
    return stopping_.load() && now >= grace_deadline_;
}

// ---- acceptor thread ----

void PoolServer::accept_loop() {
    pollfd fds[2] = {{listen_fd_.get(), POLLIN, 0}, {wake_fd_.get(), POLLIN, 0}};
    while (!stopping_.load()) {
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            int err = errno;
            if (err == EINTR) continue;
            // Should never happen with two valid descriptors; if it does, it
            // will most likely keep happening, so log rarely and back off
            // rather than flood the log from a tight loop.
            if (warning_due())
                log_msg(LogLevel::Error, "poll on listening socket: %s", strerror(err));
            std::this_thread::sleep_for(kAcceptBackoff);
            continue;
        }
        if (fds[1].revents != 0) break;  // stop() was called
        if (fds[0].revents & POLLIN) {
            accept_ready();
        } else if (fds[0].revents != 0) {
            // POLLERR, POLLHUP or POLLNVAL without POLLIN: accept() would not
            // clear the condition, so poll() would report it again at once and
            // the acceptor would spin. Back off the same way as for accept errors.
            if (warning_due())
                log_msg(LogLevel::Error, "listening socket reported revents=0x%x (backing off)",
                        static_cast<unsigned>(fds[0].revents));
            std::this_thread::sleep_for(kAcceptBackoff);
        }
    }
    // Closing the listening socket makes the kernel refuse new connections and
    // reset any still waiting in the backlog.
    listen_fd_.reset();
}

void PoolServer::accept_ready() {
    while (!stopping_.load()) {
        // Accepted sockets stay blocking (accept4 does not copy O_NONBLOCK from
        // the listening socket): workers rely on SO_RCVTIMEO ticks instead.
        int fd = accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) {
            hand_off(Fd(fd));
            continue;
        }
        int err = errno;
        if (err == EINTR || err == ECONNABORTED) continue;  // try the next one
        if (err == EAGAIN || err == EWOULDBLOCK) return;    // backlog empty
        // EMFILE/ENFILE (out of descriptors), ENOBUFS/ENOMEM, or anything else:
        // back off rather than retry at once, which would just fail again.
        if (warning_due()) log_msg(LogLevel::Warn, "accept: %s (backing off)", strerror(err));
        std::this_thread::sleep_for(kAcceptBackoff);
        return;
    }
}

// Acceptor-thread warnings repeat for as long as their cause lasts (EMFILE
// until some connection closes), so at most one per second is logged.
bool PoolServer::warning_due() {
    Clock::time_point now = Clock::now();
    if (now - last_acceptor_warning_ < std::chrono::seconds(1)) return false;
    last_acceptor_warning_ = now;
    return true;
}

void PoolServer::hand_off(Fd conn) {
    stats_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
    stats_.active_connections.fetch_add(1, std::memory_order_relaxed);
    set_no_delay(conn.get());

    // The task owns the descriptor from here on; std::function needs a
    // copyable callable, so it carries the plain number. If the pool refuses
    // the task, the task is destroyed unrun and ownership comes back here.
    int fd = conn.release();
    Clock::time_point accepted = Clock::now();
    if (pool_->try_submit([this, fd, accepted] { run_connection(fd, accepted); })) return;

    Fd rejected(fd);
    reject_busy(rejected.get());
    rejected.reset();
    stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
}

void PoolServer::reject_busy(int fd) {
    // Best effort and never blocking: the acceptor must not stall on one slow
    // client while the server is already overloaded.
    std::string reply = serialize(Response::error(503), /*keep_alive=*/false, /*head_only=*/false);
    ssize_t sent = send(fd, reply.data(), reply.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(reply.size())) stats_.record(503);
}

// ---- worker threads ----

void PoolServer::run_connection(int raw_fd, Clock::time_point accepted) {
    Fd fd(raw_fd);
    // A ThreadPool task must not throw. The only realistic exception here is
    // std::bad_alloc; it costs this one connection, not the whole process.
    try {
        serve(fd.get(), accepted);
    } catch (const std::exception& e) {
        log_msg(LogLevel::Error, "connection dropped: %s", e.what());
    } catch (...) {
        log_msg(LogLevel::Error, "connection dropped: unknown exception");
    }
    fd.reset();
    stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
}

// Deadlines (the epoll server applies the same rules):
// - Keep-alive: the idle clock starts at accept and restarts whenever a
//   response has been fully sent. Bytes that do not start a request (blank
//   lines before a request) do not restart it.
// - Request: a request is in progress while the parser has started one or
//   unparsed input is buffered. Its clock starts when the bytes that began it
//   arrived and is not refreshed by later bytes, so a client trickling one
//   header byte at a time still gets its 408.
void PoolServer::serve(int fd, Clock::time_point accepted) {
    // Queued when shutdown began: close without serving.
    if (stopping_.load()) return;
    set_tick_timeouts(fd);

    HttpConn conn(router_, config_, stats_);
    std::string out;
    bool keep_open = true;   // false once process() has decided to close
    bool peer_done = false;  // recv() returned 0: the client sends nothing more
    bool draining = false;
    bool request_in_progress = false;
    Clock::time_point idle_since = accepted;
    Clock::time_point request_started{};
    char buf[kReadChunk];

    auto handle_input = [&](Clock::time_point arrived) {
        if (!draining && stopping_.load()) {
            conn.set_draining();  // responses from now on say Connection: close
            draining = true;
        }
        unsigned served_before = conn.requests_served();
        keep_open = conn.process(out);
        // A finished request stops its clock; a request that begins in the
        // same bytes gets a fresh one below.
        if (conn.requests_served() != served_before) request_in_progress = false;
        bool in_progress = conn.mid_request() || conn.has_pending_input();
        if (!in_progress) {
            request_in_progress = false;
        } else if (!request_in_progress) {
            request_in_progress = true;
            request_started = arrived;
        }
    };

    for (;;) {
        // The whole output is flushed before anything else, so `out` is always
        // empty (below kOutputHighWater) when the code below reads or processes.
        if (!out.empty()) {
            if (!send_all(fd, out)) return;
            out.clear();
            idle_since = Clock::now();
        }
        if (!keep_open) return;
        // Read contract rule (c): requests left over after a high-water stop
        // are handled before reading anything new.
        if (conn.has_pending_input()) {
            handle_input(Clock::now());
            continue;
        }
        if (peer_done) return;  // nothing buffered and nothing more will come

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            conn.on_data(buf, static_cast<size_t>(n));
            handle_input(Clock::now());
        } else if (n == 0) {
            // Possibly only a half-close: whatever was already processed is
            // still flushed by the next pass before the connection closes.
            peer_done = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return;  // ECONNRESET or another hard error: the peer is gone
        }
        // EAGAIN/EWOULDBLOCK is the SO_RCVTIMEO tick.

        // A finished response is sent before any deadline is judged: the
        // request it answers arrived in time, however late the reply.
        if (!out.empty()) continue;

        // Judged only after a read attempt, so bytes a client sent while the
        // connection waited in the pool queue are seen before the idle clock
        // (which started at accept) can close it. Judged after every read, not
        // only after a timed-out one: a client sending one byte per tick would
        // otherwise never let a check run.
        Clock::time_point now = Clock::now();
        if (stopping_.load()) {
            if (!request_in_progress) return;  // idle at shutdown: close now
            if (!draining) {
                conn.set_draining();
                draining = true;
            }
            if (past_grace(now)) return;
        }
        if (request_in_progress) {
            if (now - request_started >= config_.request_timeout) {
                if (send_all(fd, conn.timeout_response())) stats_.record(408);
                return;
            }
        } else if (now - idle_since >= config_.keepalive_timeout) {
            return;
        }
    }
}

// Sends all of `data`. False when the peer is gone (EPIPE, ECONNRESET), when no
// byte could be sent for write_timeout, or when the shutdown grace period ends.
bool PoolServer::send_all(int fd, const std::string& data) {
    size_t sent = 0;
    Clock::time_point last_progress = Clock::now();
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            last_progress = Clock::now();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // SO_SNDTIMEO tick: the client is not reading.
            Clock::time_point now = Clock::now();
            if (now - last_progress >= config_.write_timeout || past_grace(now)) return false;
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace hs
