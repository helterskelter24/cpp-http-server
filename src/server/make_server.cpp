#include "server/pool_server.h"
#include "server/server.h"

namespace hs {

std::unique_ptr<Server> make_server(const Config& config, const Router& router, Stats& stats) {
    switch (config.mode) {
        case Mode::Pool:
            return std::make_unique<PoolServer>(config, router, stats);
        case Mode::Epoll:
            return nullptr;  // not built yet
    }
    return nullptr;
}

}  // namespace hs
