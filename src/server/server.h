#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hs {

struct Config;
class Router;
struct Stats;

// What main() needs from either concurrency model (pool or epoll).
class Server {
public:
    virtual ~Server() = default;
    virtual bool start(std::string& error) = 0;  // bind and spawn threads
    virtual uint16_t port() const = 0;           // the bound port (useful with port 0)
    virtual void stop() = 0;                     // begin graceful shutdown; idempotent, thread-safe
    virtual void join() = 0;                     // returns when all threads have exited
};

// Builds the server selected by config.mode.
std::unique_ptr<Server> make_server(const Config& config, const Router& router, Stats& stats);

}  // namespace hs
