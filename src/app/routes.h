#pragma once

#include "config.h"
#include "http/router.h"
#include "server/stats.h"

namespace hs {

// What the built-in handlers need to read. Both pointers must outlive the router.
struct AppContext {
    const Config* config;
    Stats* stats;
};

// Registers /health, /api/stats, /api/time, /api/echo and /api/users/:id.
// Static files are registered separately (register_static).
void register_routes(Router& router, const AppContext& ctx);

}  // namespace hs
