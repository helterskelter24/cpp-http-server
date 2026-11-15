#pragma once

// Percentile helper for the load generator, kept in a header of its own so a
// unit test can include it without pulling in the rest of the client.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hs::bench {

// Nearest-rank percentile of `sorted` (ascending, nanoseconds): the sample at
// zero-based index ceil(p / 100 * n) - 1. The result is always one of the
// measured samples, never an interpolated value, which is what "p99 was X"
// should mean. Returns 0 for an empty vector.
//
// The rank is computed as p * n / 100, not p / 100 * n: dividing first
// introduces a rounding error (7 / 100.0 * 100 is 7.000000000000001), and
// ceil() would turn an exact rank 7 into 8.
inline uint64_t percentile_ns(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const double rank = std::ceil(p * static_cast<double>(sorted.size()) / 100.0);
    if (rank < 1.0) return sorted.front();
    const size_t index = static_cast<size_t>(rank) - 1;
    return index >= sorted.size() ? sorted.back() : sorted[index];
}

}  // namespace hs::bench
