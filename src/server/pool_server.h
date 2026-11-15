#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "http/router.h"
#include "net/socket.h"
#include "server/server.h"
#include "server/stats.h"
#include "server/thread_pool.h"

namespace hs {

// The blocking concurrency model: one acceptor thread hands each accepted
// socket to a ThreadPool, and a worker owns that connection with plain blocking
// recv/send until it closes. Simple to follow, but every open connection (even
// an idle keep-alive one) occupies a worker for as long as it stays open.
class PoolServer : public Server {
public:
    PoolServer(const Config& config, const Router& router, Stats& stats);
    ~PoolServer() override;  // stop() and join()

    bool start(std::string& error) override;
    uint16_t port() const override { return port_; }
    void stop() override;
    void join() override;

private:
    using Clock = std::chrono::steady_clock;

    void accept_loop();
    void accept_ready();  // accept until the backlog is empty
    void hand_off(Fd conn);
    void reject_busy(int fd);
    void warn_accept_failure(int err);

    // Runs on a worker: serves one connection, then closes it.
    void run_connection(int fd, Clock::time_point accepted);
    void serve(int fd, Clock::time_point accepted);
    bool send_all(int fd, const std::string& data);
    bool past_grace(Clock::time_point now) const;

    const Config& config_;
    const Router& router_;
    Stats& stats_;

    Fd listen_fd_;  // used only by the acceptor thread once it has started
    Fd wake_fd_;    // eventfd: stop() writes it to wake the acceptor's poll
    uint16_t port_ = 0;
    std::unique_ptr<ThreadPool> pool_;
    std::thread acceptor_;
    Clock::time_point last_accept_warning_{};  // acceptor thread only

    // grace_deadline_ is written once, before stopping_ becomes true, and read
    // only after a thread has seen stopping_ == true. The atomic store/load pair
    // orders the two, so the plain time_point needs no lock of its own.
    std::mutex stop_mu_;
    std::atomic<bool> stopping_{false};
    Clock::time_point grace_deadline_{};
};

}  // namespace hs
