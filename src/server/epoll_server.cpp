#include "server/epoll_server.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <exception>
#include <iterator>
#include <list>
#include <unordered_map>
#include <utility>

#include "logger.h"
#include "net/socket.h"
#include "server/http_conn.h"

namespace hs {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kReadChunk = 16 * 1024;
constexpr int kMaxEvents = 256;
// epoll_wait never sleeps longer than this, even with no deadline pending.
constexpr auto kMaxWait = std::chrono::milliseconds(1000);
// After accept() fails with EMFILE and the like, the waiting connections stay
// in the backlog, but an edge-triggered listening socket reports nothing new
// until yet another client connects. So accepting is retried on a timer.
constexpr auto kAcceptRetry = std::chrono::milliseconds(100);
// Pause after an unexpected epoll_wait failure, so it cannot become a busy loop.
constexpr auto kErrorBackoff = std::chrono::milliseconds(50);
// An output buffer with more capacity than this is released once it drains,
// so an idle connection does not keep a megabyte from its last big response.
constexpr size_t kKeepOutputCapacity = 64 * 1024;

// epoll_event.data.ptr for the two descriptors that are not connections. Only
// their addresses matter: they are compared, never dereferenced.
char listen_tag;
char wake_tag;

// Connections: readable, peer half-closed, edge-triggered. EPOLLOUT is added
// only while there is output the kernel did not take.
constexpr uint32_t kConnEvents = EPOLLIN | EPOLLRDHUP | EPOLLET;

}  // namespace

// ---- per-connection state ----

enum class Deadline { None, Request, KeepAlive, Write };

struct Conn {
    Conn(Fd socket, const Router& router, const Config& config, Stats& stats, Clock::time_point now)
        : fd(std::move(socket)), http(router, config, stats), idle_since(now) {}

    Fd fd;
    HttpConn http;
    std::string out;  // responses; out[0 .. out_off) has already been sent
    size_t out_off = 0;

    Deadline deadline_kind = Deadline::None;
    Clock::time_point deadline{};
    std::list<Conn*>::iterator timer_it;  // position in the list for deadline_kind

    bool want_write = false;  // EPOLLOUT is in the registered event mask
    bool closing = false;     // closed during this batch; waiting in the graveyard
    bool keep_open = true;    // false once process() (or a 408) decided to close
    bool peer_done = false;   // recv() returned 0: the client sends nothing more

    // Clocks, with the same rules as PoolServer::serve: the request clock
    // starts with a request's first bytes and is never refreshed; the idle
    // clock starts at accept and restarts when all queued output has been
    // sent; the write clock restarts on every send that makes progress.
    bool in_request = false;
    Clock::time_point request_started{};
    Clock::time_point idle_since;
    Clock::time_point last_write{};

    size_t unsent() const { return out.size() - out_off; }
};

// ---- one event loop ----

class EventLoop {
public:
    EventLoop(unsigned id, const Config& config, const Router& router, Stats& stats,
              const std::atomic<bool>& stopping)
        : id_(id), config_(config), router_(router), stats_(stats), stopping_(stopping) {}

    bool open(Fd listen_fd, std::string& error);
    void wake();  // any thread: interrupts epoll_wait
    void run();   // the loop thread's body

private:
    void accept_ready(Clock::time_point now);
    void add_connection(Fd socket, Clock::time_point now);
    void serve(Conn* c, uint32_t events);
    void drive(Conn* c);
    bool flush(Conn* c);
    void handle_input(Conn* c);
    void after_io(Conn* c);
    void update_interest(Conn* c);
    void update_deadline(Conn* c, Clock::time_point now);
    void set_deadline(Conn* c, Deadline kind, Clock::time_point when);
    void expire_deadlines(Clock::time_point now);
    void expire_request(Conn* c);
    void begin_shutdown(Clock::time_point now);
    void close_conn(Conn* c);
    int wait_timeout(Clock::time_point now) const;
    bool idle(const Conn& c) const;
    bool warning_due(Clock::time_point now);

    std::list<Conn*>& timers(Deadline kind) { return timers_[static_cast<int>(kind) - 1]; }

    const unsigned id_;
    const Config& config_;
    const Router& router_;
    Stats& stats_;
    const std::atomic<bool>& stopping_;

    Fd epoll_fd_;
    Fd listen_fd_;
    Fd wake_fd_;  // written by EpollServer::stop() from another thread

    std::unordered_map<int, std::unique_ptr<Conn>> conns_;
    // Connections closed during the current batch of events. They are freed
    // (and their descriptors closed) only after the batch: a later event in
    // the same batch may still carry a pointer to one, and keeping the fd open
    // until then means accept() cannot hand out the same number meanwhile.
    std::vector<std::unique_ptr<Conn>> graveyard_;
    // One list per deadline kind (Request, KeepAlive, Write), each ordered by
    // deadline. Every deadline of one kind is "some event + a fixed duration",
    // so a new or refreshed deadline almost always belongs at the back.
    std::list<Conn*> timers_[3];

    bool draining_ = false;
    Clock::time_point grace_deadline_{};
    bool accept_paused_ = false;
    Clock::time_point accept_retry_at_{};
    Clock::time_point last_warning_{};
    uint64_t accepted_ = 0;
};

bool EventLoop::open(Fd listen_fd, std::string& error) {
    epoll_fd_.reset(epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd_.valid()) {
        error = std::string("epoll_create1: ") + strerror(errno);
        return false;
    }
    wake_fd_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_fd_.valid()) {
        error = std::string("eventfd: ") + strerror(errno);
        return false;
    }
    listen_fd_ = std::move(listen_fd);

    // The listening socket is edge-triggered too: accept_ready() always
    // accepts until EAGAIN, and after EMFILE a level-triggered socket would
    // be reported again at once, forever, while the backlog is not empty.
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = &listen_tag;
    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd_.get(), &ev) != 0) {
        error = std::string("epoll_ctl listening socket: ") + strerror(errno);
        return false;
    }
    // Level-triggered: the loop reads the counter back to zero when woken.
    ev.events = EPOLLIN;
    ev.data.ptr = &wake_tag;
    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, wake_fd_.get(), &ev) != 0) {
        error = std::string("epoll_ctl eventfd: ") + strerror(errno);
        return false;
    }
    return true;
}

void EventLoop::wake() {
    uint64_t one = 1;
    ssize_t ignored = write(wake_fd_.get(), &one, sizeof(one));
    (void)ignored;  // a full counter (EAGAIN) means a wake-up is already pending
}

void EventLoop::run() {
    epoll_event events[kMaxEvents];
    for (;;) {
        Clock::time_point now = Clock::now();
        // The flag, not the eventfd, is what says "stop": the eventfd only
        // interrupts the wait. A stop() that happened before this thread
        // started is therefore seen here too.
        if (!draining_ && stopping_.load()) begin_shutdown(now);
        if (draining_ && (conns_.empty() || now >= grace_deadline_)) break;

        int n = epoll_wait(epoll_fd_.get(), events, kMaxEvents, wait_timeout(now));
        if (n < 0) {
            int err = errno;
            if (err == EINTR) continue;
            if (warning_due(Clock::now()))
                log_msg(LogLevel::Error, "epoll_wait: %s", strerror(err));
            std::this_thread::sleep_for(kErrorBackoff);
            continue;
        }

        for (int i = 0; i < n; ++i) {
            void* tag = events[i].data.ptr;
            if (tag == &wake_tag) {
                uint64_t value;
                ssize_t ignored = read(wake_fd_.get(), &value, sizeof(value));
                (void)ignored;
            } else if (tag == &listen_tag) {
                accept_ready(Clock::now());
            } else {
                Conn* c = static_cast<Conn*>(tag);
                if (!c->closing) serve(c, events[i].events);
            }
        }

        now = Clock::now();
        if (accept_paused_ && now >= accept_retry_at_) accept_ready(now);
        expire_deadlines(now);
        graveyard_.clear();
    }

    // Grace period over, or nothing left: force-close whatever remains.
    std::vector<Conn*> remaining;
    for (auto& entry : conns_) remaining.push_back(entry.second.get());
    for (Conn* c : remaining) close_conn(c);
    graveyard_.clear();
    log_msg(LogLevel::Debug, "event loop %u exiting after accepting %llu connections", id_,
            static_cast<unsigned long long>(accepted_));
}

// ---- accepting ----

void EventLoop::accept_ready(Clock::time_point now) {
    accept_paused_ = false;
    for (;;) {
        // The accepted socket is non-blocking from the start (one syscall
        // instead of accept + fcntl) and close-on-exec.
        int fd = accept4(listen_fd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            add_connection(Fd(fd), now);
            continue;
        }
        int err = errno;
        if (err == EINTR || err == ECONNABORTED) continue;  // try the next one
        if (err == EAGAIN || err == EWOULDBLOCK) return;    // backlog empty
        // EMFILE/ENFILE (out of descriptors), ENOBUFS/ENOMEM: stop for now and
        // retry on a timer instead of looping on a call that keeps failing.
        if (warning_due(now)) log_msg(LogLevel::Warn, "accept: %s (backing off)", strerror(err));
        accept_paused_ = true;
        accept_retry_at_ = now + kAcceptRetry;
        return;
    }
}

void EventLoop::add_connection(Fd socket, Clock::time_point now) {
    set_no_delay(socket.get());
    const int fd = socket.get();
    // Every step that allocates (and so can throw std::bad_alloc) comes before
    // the socket is registered with epoll or counted in the stats, and a
    // failure undoes the steps before it. A failed setup then costs only this
    // connection: its socket is closed, the counters never saw it, and the
    // loop carries on. Without this, an exception escaping here would leave
    // the thread and terminate the process.
    Conn* c = nullptr;
    try {
        auto conn = std::make_unique<Conn>(std::move(socket), router_, config_, stats_, now);
        c = conn.get();
        // The map slot is created empty and filled afterwards, so if creating
        // it throws, `conn` still owns the connection and closes it.
        conns_.emplace(fd, nullptr).first->second = std::move(conn);
        update_deadline(c, now);  // the idle clock starts at accept; allocates a timer-list node
    } catch (const std::exception& e) {
        // Destroying the Conn closes the socket. If the Conn was never made,
        // the Fd still in `socket` closes it when this function returns.
        if (c != nullptr) conns_.erase(fd);
        log_msg(LogLevel::Error, "connection dropped: %s", e.what());
        return;
    }
    epoll_event ev{};
    ev.events = kConnEvents;
    ev.data.ptr = c;
    // Adding a socket that already has data queued reports it at the next
    // epoll_wait, so bytes sent right after connecting are not missed.
    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
        if (warning_due(now)) log_msg(LogLevel::Warn, "epoll_ctl add: %s", strerror(errno));
        timers(c->deadline_kind).erase(c->timer_it);
        conns_.erase(fd);  // destroys the connection and closes its socket
        return;
    }
    stats_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
    stats_.active_connections.fetch_add(1, std::memory_order_relaxed);
    ++accepted_;
}

// ---- connection I/O ----

void EventLoop::serve(Conn* c, uint32_t events) {
    // EPOLLERR means the socket has a pending error (usually a reset): nothing
    // more can be delivered either way. EPOLLHUP and EPOLLRDHUP need no branch
    // of their own: recv() reports them as 0 or an error inside drive().
    if (events & EPOLLERR) {
        close_conn(c);
        return;
    }
    // Handlers run inline; an exception (realistically std::bad_alloc) costs
    // this one connection, not the whole loop.
    try {
        drive(c);
    } catch (const std::exception& e) {
        log_msg(LogLevel::Error, "connection dropped: %s", e.what());
        close_conn(c);
    }
}

// Does all the work an event allows, following HttpConn's read contract:
// read only while nothing is buffered in HttpConn and the output is below the
// high-water mark, process after every read, and process leftover input
// before reading more. Edge-triggered epoll reports new readiness only once,
// so every exit from this loop must be one where a later event is guaranteed:
// recv() said EAGAIN (the next bytes raise a new EPOLLIN edge), or output is
// waiting (EPOLLOUT is registered, and its event brings the loop back here,
// where reading resumes as soon as the output is low enough again).
void EventLoop::drive(Conn* c) {
    char buf[kReadChunk];
    for (;;) {
        if (!flush(c)) {
            close_conn(c);
            return;
        }
        // Compared with out.size(), not unsent(): process() stops at
        // out.size() >= kOutputHighWater, so calling it above that would make
        // no progress. flush() compacts `out` as it drains, so this clears.
        if (c->out.size() >= kOutputHighWater) break;
        if (c->http.has_pending_input()) {  // rule (c): requests left over from a high-water stop
            handle_input(c);
            continue;
        }
        if (!c->keep_open || c->peer_done) {
            if (c->unsent() == 0) {
                close_conn(c);
                return;
            }
            break;  // finish sending on EPOLLOUT, then close
        }
        ssize_t n = recv(c->fd.get(), buf, sizeof(buf), 0);
        if (n > 0) {
            c->http.on_data(buf, static_cast<size_t>(n));
            handle_input(c);  // rule (b)
            continue;
        }
        if (n == 0) {
            // Possibly only a half-close: what was already received is still
            // answered and flushed before the connection closes.
            c->peer_done = true;
            continue;
        }
        int err = errno;
        if (err == EINTR) continue;
        if (err == EAGAIN || err == EWOULDBLOCK) break;
        close_conn(c);  // ECONNRESET or another hard error: the peer is gone
        return;
    }
    after_io(c);
}

// Sends unsent output until it is all sent or the socket buffer is full.
// Returns false when the connection is broken (EPIPE, ECONNRESET, ...).
bool EventLoop::flush(Conn* c) {
    bool progressed = false;
    while (c->unsent() > 0) {
        ssize_t n = send(c->fd.get(), c->out.data() + c->out_off, c->unsent(), MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += static_cast<size_t>(n);
            progressed = true;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;
    }
    if (progressed) {
        Clock::time_point now = Clock::now();
        c->last_write = now;
        if (c->unsent() == 0) c->idle_since = now;  // every queued response is fully sent
    }
    // Same policy as HttpConn's input: erase the sent prefix only once it is
    // at least half the buffer, so each byte is moved O(1) times on average.
    if (c->unsent() == 0) {
        if (c->out.capacity() > kKeepOutputCapacity) {
            std::string().swap(c->out);
        } else {
            c->out.clear();
        }
        c->out_off = 0;
    } else if (c->out_off >= c->out.size() / 2) {
        c->out.erase(0, c->out_off);
        c->out_off = 0;
    }
    return true;
}

void EventLoop::handle_input(Conn* c) {
    unsigned served_before = c->http.requests_served();
    c->keep_open = c->http.process(c->out);
    // A finished request stops its clock; a request that begins in the same
    // bytes gets a fresh one below.
    if (c->http.requests_served() != served_before) c->in_request = false;
    bool in_progress = c->http.mid_request() || c->http.has_pending_input();
    if (!in_progress) {
        c->in_request = false;
    } else if (!c->in_request) {
        c->in_request = true;
        c->request_started = Clock::now();
    }
}

// Bookkeeping after a connection did I/O and is still open.
void EventLoop::after_io(Conn* c) {
    // During shutdown a connection closes as soon as it has nothing left to
    // do; one with keep-alive responses still queued at shutdown gets here
    // once they have been sent.
    if (draining_ && idle(*c)) {
        close_conn(c);
        return;
    }
    update_interest(c);
    if (!c->closing) update_deadline(c, Clock::now());
}

void EventLoop::update_interest(Conn* c) {
    bool want = c->unsent() > 0;
    if (want == c->want_write) return;
    epoll_event ev{};
    ev.events = kConnEvents | (want ? EPOLLOUT : 0u);
    ev.data.ptr = c;
    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, c->fd.get(), &ev) != 0) {
        // Without EPOLLOUT the output would never be sent; give up on it.
        if (warning_due(Clock::now()))
            log_msg(LogLevel::Warn, "epoll_ctl mod: %s", strerror(errno));
        close_conn(c);
        return;
    }
    c->want_write = want;
}

bool EventLoop::idle(const Conn& c) const {
    return !c.http.mid_request() && !c.http.has_pending_input() && c.unsent() == 0;
}

// ---- deadlines ----

void EventLoop::update_deadline(Conn* c, Clock::time_point now) {
    if (c->unsent() > 0) {
        // Output got stuck just now: the write clock starts here. While it
        // stays stuck, only flush() making progress moves last_write.
        if (c->deadline_kind != Deadline::Write) c->last_write = now;
        set_deadline(c, Deadline::Write, c->last_write + config_.write_timeout);
    } else if (c->in_request) {
        set_deadline(c, Deadline::Request, c->request_started + config_.request_timeout);
    } else {
        set_deadline(c, Deadline::KeepAlive, c->idle_since + config_.keepalive_timeout);
    }
}

void EventLoop::set_deadline(Conn* c, Deadline kind, Clock::time_point when) {
    if (c->deadline_kind == kind && c->deadline == when) return;  // unchanged: stays put
    std::list<Conn*>& list = timers(kind);
    // Normally the right place is the back. The walk handles the rare
    // deadline that is earlier than one already queued (a request clock that
    // started while the connection sat in the write list): skipping it would
    // only delay that one expiry, but an unsorted list would also mislead the
    // epoll_wait timeout, which looks at the front alone.
    auto pos = list.end();
    while (pos != list.begin() && (*std::prev(pos))->deadline > when) --pos;
    if (c->deadline_kind == Deadline::None) {
        c->timer_it = list.insert(pos, c);
    } else {
        // splice moves the existing node: no allocation, and timer_it stays valid.
        list.splice(pos, timers(c->deadline_kind), c->timer_it);
    }
    c->deadline_kind = kind;
    c->deadline = when;
}

void EventLoop::expire_deadlines(Clock::time_point now) {
    // Each step removes the front entry from its list (by closing the
    // connection or moving it to the write list), so these loops end.
    std::list<Conn*>& requests = timers(Deadline::Request);
    while (!requests.empty() && requests.front()->deadline <= now) expire_request(requests.front());
    for (Deadline kind : {Deadline::KeepAlive, Deadline::Write}) {
        std::list<Conn*>& list = timers(kind);
        while (!list.empty() && list.front()->deadline <= now) close_conn(list.front());
    }
}

// A request took too long to arrive: answer 408 and close once it is sent.
// The connection is in the request list only while nothing is unsent, so the
// 408 is the whole of its output.
void EventLoop::expire_request(Conn* c) {
    try {
        c->out = c->http.timeout_response();
    } catch (const std::exception&) {
        close_conn(c);
        return;
    }
    c->out_off = 0;
    c->keep_open = false;
    c->in_request = false;
    // Counted when queued, as HttpConn counts every other response.
    stats_.record(408);
    if (!flush(c) || c->unsent() == 0) {
        close_conn(c);
        return;
    }
    after_io(c);  // registers EPOLLOUT and moves it to the write list
}

int EventLoop::wait_timeout(Clock::time_point now) const {
    Clock::time_point wake = now + kMaxWait;
    for (const std::list<Conn*>& list : timers_)
        if (!list.empty() && list.front()->deadline < wake) wake = list.front()->deadline;
    if (draining_ && grace_deadline_ < wake) wake = grace_deadline_;
    if (accept_paused_ && accept_retry_at_ < wake) wake = accept_retry_at_;
    if (wake <= now) return 0;
    // Rounded up: rounding down would wake just before the deadline and spin
    // with zero timeouts until it passes.
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(wake - now).count());
}

// ---- shutdown and closing ----

void EventLoop::begin_shutdown(Clock::time_point now) {
    draining_ = true;
    grace_deadline_ = now + config_.shutdown_grace;
    // No new connections. Connections still in this socket's backlog are
    // reset by the kernel when it closes.
    epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, listen_fd_.get(), nullptr);
    listen_fd_.reset();
    accept_paused_ = false;

    std::vector<Conn*> all;
    for (auto& entry : conns_) all.push_back(entry.second.get());
    for (Conn* c : all) {
        if (idle(*c)) {
            close_conn(c);
        } else {
            c->http.set_draining();  // later responses say Connection: close
        }
    }
    graveyard_.clear();  // not inside an event batch here
}

void EventLoop::close_conn(Conn* c) {
    if (c->closing) return;
    c->closing = true;
    // Explicit, although close() would also remove it: the descriptor stays
    // open until the graveyard is cleared, and it must not report events in
    // the meantime.
    epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, c->fd.get(), nullptr);
    if (c->deadline_kind != Deadline::None) {
        timers(c->deadline_kind).erase(c->timer_it);
        c->deadline_kind = Deadline::None;
    }
    auto it = conns_.find(c->fd.get());
    graveyard_.push_back(std::move(it->second));
    conns_.erase(it);
    stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
}

// Loop warnings repeat for as long as their cause lasts (EMFILE until some
// connection closes), so at most one per second is logged.
bool EventLoop::warning_due(Clock::time_point now) {
    if (now - last_warning_ < std::chrono::seconds(1)) return false;
    last_warning_ = now;
    return true;
}

// ---- EpollServer ----

EpollServer::EpollServer(const Config& config, const Router& router, Stats& stats)
    : config_(config), router_(router), stats_(stats) {}

EpollServer::~EpollServer() {
    stop();
    join();
}

bool EpollServer::start(std::string& error) {
    uint16_t port = config_.port;
    if (port != 0) {
        // Every loop's socket sets SO_REUSEPORT, and Linux lets such sockets
        // share a port when they belong to the same user, so a second server
        // started on this port by mistake would quietly take part of the
        // connections instead of failing. A probe bound without SO_REUSEPORT
        // fails if anything listens there already. There is a small race: a
        // process that binds the port between this probe and the binds below
        // goes unnoticed. That needs two instances started within moments of
        // each other, and the outcome is the same sharing as without the
        // probe, so it is accepted rather than locked against.
        error = check_port_free(config_.bind, port);
        if (!error.empty()) return false;
    }

    std::vector<std::unique_ptr<EventLoop>> loops;
    for (unsigned i = 0; i < config_.threads; ++i) {
        ListenResult listening = listen_tcp(config_.bind, port, config_.backlog,
                                            /*reuse_port=*/true, /*non_blocking=*/true);
        if (!listening.error.empty()) {
            error = listening.error;
            return false;
        }
        // With port 0 the first socket gets a port from the kernel and the
        // others join it.
        if (port == 0) port = local_port(listening.fd.get());
        auto loop = std::make_unique<EventLoop>(i, config_, router_, stats_, stopping_);
        if (!loop->open(std::move(listening.fd), error)) return false;
        loops.push_back(std::move(loop));
    }
    port_ = port;

    // Threads start only once every socket is open, so a failure above
    // leaves nothing running.
    std::lock_guard<std::mutex> lock(stop_mu_);
    loops_ = std::move(loops);
    for (auto& loop : loops_) {
        EventLoop* l = loop.get();
        threads_.emplace_back([l] { l->run(); });
    }
    return true;
}

void EpollServer::stop() {
    std::lock_guard<std::mutex> lock(stop_mu_);
    if (stopping_.load()) return;
    stopping_.store(true);
    for (auto& loop : loops_) loop->wake();
}

void EpollServer::join() {
    for (std::thread& t : threads_)
        if (t.joinable()) t.join();
}

}  // namespace hs
