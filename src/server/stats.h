#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace hs {

// Counters shared by every connection thread and read by /api/stats.
//
// All operations use relaxed ordering. Each counter is an independent number
// that nothing else synchronizes on: no other data is published through it, so
// the only guarantee needed is that increments are not lost, which atomicity
// alone gives. A reader may see counters from slightly different instants
// (for example requests_total ahead of the class counters); that is fine for
// monitoring output.
struct Stats {
    std::atomic<uint64_t> connections_accepted{0}, active_connections{0}, requests_total{0};
    std::atomic<uint64_t> responses_2xx{0}, responses_3xx{0}, responses_4xx{0}, responses_5xx{0};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    // Counts one finished response. Statuses outside 200..599 (1xx, or a bug
    // producing garbage) are counted as 5xx so they cannot go unnoticed.
    void record(int status) {
        requests_total.fetch_add(1, std::memory_order_relaxed);
        std::atomic<uint64_t>* bucket = &responses_5xx;
        if (status >= 200 && status < 300) bucket = &responses_2xx;
        else if (status >= 300 && status < 400) bucket = &responses_3xx;
        else if (status >= 400 && status < 500) bucket = &responses_4xx;
        bucket->fetch_add(1, std::memory_order_relaxed);
    }
};

}  // namespace hs
