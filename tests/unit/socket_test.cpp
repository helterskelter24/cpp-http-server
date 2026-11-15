#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "net/socket.h"
#include "test.h"

namespace {

bool readable_now(int fd) {
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

}  // namespace

TEST(socket_listen_ephemeral_port) {
    hs::ListenResult r = hs::listen_tcp("127.0.0.1", 0, 16, false, false);
    CHECK_EQ(r.error, "");
    CHECK(r.fd.valid());
    CHECK(hs::local_port(r.fd.get()) != 0);
    // FD_CLOEXEC is always set; O_NONBLOCK only when asked.
    CHECK(fcntl(r.fd.get(), F_GETFD) & FD_CLOEXEC);
    CHECK(!(fcntl(r.fd.get(), F_GETFL) & O_NONBLOCK));
}

TEST(socket_listen_non_blocking) {
    hs::ListenResult r = hs::listen_tcp("127.0.0.1", 0, 16, true, true);
    CHECK_EQ(r.error, "");
    CHECK(fcntl(r.fd.get(), F_GETFL) & O_NONBLOCK);
}

TEST(socket_listen_port_in_use) {
    hs::ListenResult first = hs::listen_tcp("127.0.0.1", 0, 16, false, false);
    CHECK_EQ(first.error, "");
    uint16_t port = hs::local_port(first.fd.get());
    hs::ListenResult second = hs::listen_tcp("127.0.0.1", port, 16, false, false);
    CHECK(!second.fd.valid());
    CHECK(second.error.find(std::to_string(port)) != std::string::npos);
    CHECK(second.error.find("127.0.0.1") != std::string::npos);
}

TEST(socket_check_port_free_detects_reuse_port_listener) {
    hs::ListenResult first = hs::listen_tcp("127.0.0.1", 0, 16, true, true);
    CHECK_EQ(first.error, "");
    uint16_t port = hs::local_port(first.fd.get());
    // A second SO_REUSEPORT listener shares the port without any error ...
    hs::ListenResult second = hs::listen_tcp("127.0.0.1", port, 16, true, true);
    CHECK_EQ(second.error, "");
    // ... which is why the probe binds without it.
    std::string error = hs::check_port_free("127.0.0.1", port);
    CHECK(error.find("bind 127.0.0.1:" + std::to_string(port)) != std::string::npos);

    first.fd.reset();
    second.fd.reset();
    CHECK_EQ(hs::check_port_free("127.0.0.1", port), "");
}

TEST(socket_listen_bad_address) {
    hs::ListenResult r = hs::listen_tcp("not-an-ip", 0, 16, false, false);
    CHECK(!r.fd.valid());
    CHECK(r.error.find("not-an-ip") != std::string::npos);
}

TEST(socket_fd_closes_on_destroy) {
    int p[2];
    CHECK_EQ(pipe(p), 0);
    {
        hs::Fd reader(p[0]);
        CHECK_EQ(reader.get(), p[0]);
    }
    errno = 0;
    CHECK_EQ(fcntl(p[0], F_GETFD), -1);
    CHECK_EQ(errno, EBADF);
    close(p[1]);
}

TEST(socket_fd_move_and_release) {
    int p[2];
    CHECK_EQ(pipe(p), 0);
    hs::Fd a(p[0]);
    hs::Fd b(std::move(a));
    CHECK(!a.valid());
    CHECK_EQ(b.get(), p[0]);
    int raw = b.release();
    CHECK(!b.valid());
    CHECK(fcntl(raw, F_GETFD) != -1);  // release() did not close it
    hs::Fd c;
    c.reset(raw);
    c.reset(p[1]);  // closes raw
    CHECK_EQ(fcntl(raw, F_GETFD), -1);
}

TEST(socket_set_non_blocking) {
    int p[2];
    CHECK_EQ(pipe(p), 0);
    hs::Fd r(p[0]), w(p[1]);
    CHECK(hs::set_non_blocking(r.get()));
    CHECK(fcntl(r.get(), F_GETFL) & O_NONBLOCK);
}

TEST(shutdown_signal_trigger) {
    hs::ShutdownSignal& s = hs::ShutdownSignal::instance();
    s.install();
    s.reset_for_tests();
    CHECK(!s.requested());
    CHECK(!readable_now(s.fd()));
    s.trigger();
    CHECK(s.requested());
    CHECK(readable_now(s.fd()));
    s.reset_for_tests();
}

TEST(shutdown_signal_sigterm) {
    hs::ShutdownSignal& s = hs::ShutdownSignal::instance();
    s.install();
    s.reset_for_tests();
    CHECK_EQ(raise(SIGTERM), 0);  // would kill the test process without the handler
    CHECK(s.requested());
    CHECK(readable_now(s.fd()));
    // SIGPIPE must be ignored after install().
    struct sigaction current {};
    CHECK_EQ(sigaction(SIGPIPE, nullptr, &current), 0);
    CHECK(current.sa_handler == SIG_IGN);
    s.reset_for_tests();
}
