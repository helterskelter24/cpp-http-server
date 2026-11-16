#pragma once

#include <cstdint>
#include <string>

namespace hs {

// Owns one file descriptor and closes it on destruction. Move-only, so two
// owners can never close the same number (which, after fd reuse, could close
// somebody else's file).
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release();            // give up ownership without closing
    void reset(int fd = -1);  // close the current fd (if any) and own `fd`

private:
    int fd_ = -1;
};

struct ListenResult {
    Fd fd;
    std::string error;  // empty on success
};

// IPv4 TCP listening socket on bind:port (port 0 lets the kernel choose).
// SO_REUSEADDR and FD_CLOEXEC are always set; SO_REUSEPORT and O_NONBLOCK only
// when asked. On failure `error` names the step, the address and the reason,
// e.g. "bind 127.0.0.1:8080: Address already in use".
ListenResult listen_tcp(const std::string& bind, uint16_t port, int backlog, bool reuse_port,
                        bool non_blocking);

// Empty if bind:port could be bound by a socket without SO_REUSEPORT right
// now, otherwise an error in the listen_tcp style ("bind 127.0.0.1:8080:
// Address already in use"). Sockets that all set SO_REUSEPORT share a port
// silently (for the same user), so a server that listens that way calls this
// first to make a second instance on the same port fail instead.
std::string check_port_free(const std::string& bind, uint16_t port);

// The port a socket is bound to (useful after binding port 0); 0 on error.
uint16_t local_port(int fd);

// Disables Nagle's algorithm. Best effort: a failure only costs latency.
void set_no_delay(int fd);

// Process-wide shutdown request. install() routes SIGINT and SIGTERM to it and
// ignores SIGPIPE. main() waits in poll() on fd(): once a shutdown is requested
// it becomes readable and stays readable (nobody reads it in normal
// operation), and main() then calls Server::stop(), which wakes the server's
// threads through eventfds of their own.
class ShutdownSignal {
public:
    static ShutdownSignal& instance();

    void install();
    int fd() const;  // eventfd, readable once requested
    bool requested() const;
    void trigger();  // same effect as a signal, from ordinary code

    // Clears the request and drains the eventfd, so tests can trigger it again.
    void reset_for_tests();

private:
    // The state lives in socket.cpp at namespace scope, because the signal
    // handler is a plain function that can only reach globals.
    ShutdownSignal();
};

}  // namespace hs
