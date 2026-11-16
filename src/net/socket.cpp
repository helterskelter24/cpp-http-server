#include "net/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace hs {

int Fd::release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
}

void Fd::reset(int fd) {
    // On Linux close() releases the descriptor even when it reports EINTR, so
    // it is never retried: a retry could close a number another thread has
    // just been given.
    if (fd_ >= 0 && fd_ != fd) ::close(fd_);
    fd_ = fd;
}

namespace {

ListenResult listen_error(const char* step, const std::string& bind, uint16_t port) {
    ListenResult r;
    r.error = std::string(step) + " " + bind + ":" + std::to_string(port) + ": " + strerror(errno);
    return r;
}

bool set_int_option(int fd, int level, int name, int value) {
    return setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

// A TCP socket bound to bind:port, not yet listening.
ListenResult bind_tcp(const std::string& bind, uint16_t port, bool reuse_port, bool non_blocking) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1) {
        ListenResult r;
        r.error = "invalid bind address '" + bind + "': expected an IPv4 address such as 0.0.0.0";
        return r;
    }

    // Setting the flags in socket() itself avoids a window in which another
    // thread's fork+exec could inherit the descriptor.
    int type = SOCK_STREAM | SOCK_CLOEXEC | (non_blocking ? SOCK_NONBLOCK : 0);
    Fd fd(::socket(AF_INET, type, 0));
    if (!fd.valid()) return listen_error("socket", bind, port);

    // SO_REUSEADDR lets a restarted server bind while old connections from the
    // previous run are still in TIME_WAIT.
    if (!set_int_option(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1))
        return listen_error("setsockopt SO_REUSEADDR", bind, port);
    if (reuse_port && !set_int_option(fd.get(), SOL_SOCKET, SO_REUSEPORT, 1))
        return listen_error("setsockopt SO_REUSEPORT", bind, port);

    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        return listen_error("bind", bind, port);

    ListenResult r;
    r.fd = std::move(fd);
    return r;
}

}  // namespace

ListenResult listen_tcp(const std::string& bind, uint16_t port, int backlog, bool reuse_port,
                        bool non_blocking) {
    ListenResult r = bind_tcp(bind, port, reuse_port, non_blocking);
    if (!r.error.empty()) return r;
    if (::listen(r.fd.get(), backlog) != 0) return listen_error("listen", bind, port);
    return r;
}

std::string check_port_free(const std::string& bind, uint16_t port) {
    // Bound without SO_REUSEPORT, so any socket already listening on the port
    // makes this fail, whatever options that socket has. The probe never
    // listens and is closed on return.
    return bind_tcp(bind, port, /*reuse_port=*/false, /*non_blocking=*/false).error;
}

uint16_t local_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    if (addr.sin_family != AF_INET) return 0;
    return ntohs(addr.sin_port);
}

void set_no_delay(int fd) { set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, 1); }

// ---- ShutdownSignal ----

namespace {

// A signal handler may only touch lock-free atomics (or volatile
// sig_atomic_t). std::atomic<bool> is used because other threads read the flag
// too, and the static_assert makes sure it really is lock-free here.
static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "the shutdown flag must be lock-free");
std::atomic<bool> g_requested{false};
int g_event_fd = -1;

// Only async-signal-safe work: an atomic store and write(2). errno is saved
// because the handler can interrupt code that is about to read it.
void request_shutdown() {
    int saved_errno = errno;
    g_requested.store(true);
    uint64_t one = 1;
    // A full counter (EAGAIN) is fine: the fd is already readable.
    ssize_t ignored = write(g_event_fd, &one, sizeof(one));
    (void)ignored;
    errno = saved_errno;
}

void on_shutdown_signal(int) { request_shutdown(); }

}  // namespace

ShutdownSignal::ShutdownSignal() {
    // Created once, before any handler is installed, so the handler never sees -1.
    g_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_event_fd < 0) throw std::system_error(errno, std::generic_category(), "eventfd");
}

ShutdownSignal& ShutdownSignal::instance() {
    static ShutdownSignal signal;
    return signal;
}

void ShutdownSignal::install() {
    struct sigaction sa {};
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: threads learn about shutdown through fd(), not through
    // interrupted calls, so the flag buys nothing. Whichever thread takes the
    // signal may see EINTR from a blocking call, and every caller already
    // treats EINTR as "check state and retry".
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // A peer that disconnects mid-write must produce EPIPE, not kill the process.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, nullptr);
}

int ShutdownSignal::fd() const { return g_event_fd; }

bool ShutdownSignal::requested() const { return g_requested.load(); }

void ShutdownSignal::trigger() { request_shutdown(); }

void ShutdownSignal::reset_for_tests() {
    g_requested.store(false);
    uint64_t value;
    // Reading an eventfd returns and zeroes its counter; EAGAIN means it was already 0.
    ssize_t ignored = read(g_event_fd, &value, sizeof(value));
    (void)ignored;
}

}  // namespace hs
