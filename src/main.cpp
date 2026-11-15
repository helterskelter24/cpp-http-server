#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>

#include <cstdio>
#include <memory>
#include <string>

#include "app/routes.h"
#include "app/static_files.h"
#include "config.h"
#include "http/router.h"
#include "logger.h"
#include "net/socket.h"
#include "server/server.h"
#include "server/stats.h"

namespace {

// Blocks until SIGINT/SIGTERM. The signal handler only makes the eventfd
// readable, so all real shutdown work happens here, in ordinary code.
void wait_for_shutdown_signal() {
    hs::ShutdownSignal& signal = hs::ShutdownSignal::instance();
    pollfd p{signal.fd(), POLLIN, 0};
    while (!signal.requested()) {
        int ready = poll(&p, 1, -1);
        if (ready < 0 && errno != EINTR) {
            hs::log_msg(hs::LogLevel::Error, "poll on shutdown signal: %s", strerror(errno));
            return;  // shut down rather than run on with no way to stop cleanly
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    hs::ConfigResult parsed = hs::parse_args(argc, argv);
    if (!parsed.ok) {
        std::fprintf(stderr, "error: %s\n%s", parsed.error.c_str(), hs::usage().c_str());
        return 2;
    }
    if (parsed.help) {
        std::fputs(hs::usage().c_str(), stdout);
        return 0;
    }
    const hs::Config& config = parsed.config;
    hs::log_set_level(config.log_level);

    struct stat st {};
    if (stat(config.root.c_str(), &st) != 0) {
        hs::log_msg(hs::LogLevel::Error, "root directory %s: %s", config.root.c_str(),
                    strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        hs::log_msg(hs::LogLevel::Error, "root directory %s: not a directory",
                    config.root.c_str());
        return 1;
    }

    // Installed before any thread starts, so the eventfd exists before anyone
    // polls it and SIGPIPE is already ignored when the first send happens.
    hs::ShutdownSignal::instance().install();

    hs::Stats stats;
    hs::Router router;
    hs::register_routes(router, hs::AppContext{&config, &stats});
    hs::register_static(router, std::make_shared<const hs::StaticFiles>(config.root));

    std::unique_ptr<hs::Server> server = hs::make_server(config, router, stats);
    if (!server) {  // every Mode has a server; this only guards against a new one being missed
        hs::log_msg(hs::LogLevel::Error, "no server implementation for the selected mode");
        return 1;
    }
    std::string error;
    if (!server->start(error)) {
        hs::log_msg(hs::LogLevel::Error, "cannot start: %s", error.c_str());
        return 1;
    }
    hs::log_msg(hs::LogLevel::Info, "listening on %s:%u mode=%s threads=%u", config.bind.c_str(),
                static_cast<unsigned>(server->port()),
                config.mode == hs::Mode::Pool ? "pool" : "epoll", config.threads);

    wait_for_shutdown_signal();
    hs::log_msg(hs::LogLevel::Info, "shutting down");
    server->stop();
    server->join();
    hs::log_msg(hs::LogLevel::Info, "stopped after %llu requests",
                static_cast<unsigned long long>(stats.requests_total.load()));
    return 0;
}
