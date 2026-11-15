#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "http/router.h"
#include "server/server.h"
#include "server/stats.h"

namespace hs {

class EventLoop;  // one per thread; defined in epoll_server.cpp

// The non-blocking concurrency model: config.threads event loops, each with
// its own SO_REUSEPORT listening socket and its own edge-triggered epoll
// instance. The kernel spreads new connections across the listening sockets,
// and an accepted connection stays on the loop that accepted it until it
// closes, so per-connection state is only ever touched by one thread and needs
// no locks. The only state shared between threads is the stop flag, each
// loop's wake-up eventfd, the Stats counters and the logger.
class EpollServer : public Server {
public:
    EpollServer(const Config& config, const Router& router, Stats& stats);
    ~EpollServer() override;  // stop() and join()

    bool start(std::string& error) override;
    uint16_t port() const override { return port_; }
    void stop() override;
    void join() override;

private:
    const Config& config_;
    const Router& router_;
    Stats& stats_;
    uint16_t port_ = 0;

    // loops_ and threads_ are filled by start() under stop_mu_, so a stop()
    // from another thread sees either no loops (and the loops then see
    // stopping_ when they start) or all of them.
    std::mutex stop_mu_;
    std::atomic<bool> stopping_{false};
    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<std::thread> threads_;
};

}  // namespace hs
