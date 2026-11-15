#include "percentile.h"

#include <cstdint>
#include <numeric>
#include <vector>

#include "test.h"

namespace {

std::vector<uint64_t> one_to(uint64_t n) {
    std::vector<uint64_t> v(n);
    std::iota(v.begin(), v.end(), uint64_t{1});
    return v;
}

}  // namespace

TEST(percentile_nearest_rank) {
    const auto v = one_to(100);
    CHECK_EQ(hs::bench::percentile_ns(v, 50), 50u);
    CHECK_EQ(hs::bench::percentile_ns(v, 95), 95u);
    CHECK_EQ(hs::bench::percentile_ns(v, 99), 99u);
    CHECK_EQ(hs::bench::percentile_ns(v, 100), 100u);
}

TEST(percentile_single_sample_is_itself) {
    const std::vector<uint64_t> v{42};
    CHECK_EQ(hs::bench::percentile_ns(v, 50), 42u);
    CHECK_EQ(hs::bench::percentile_ns(v, 95), 42u);
    CHECK_EQ(hs::bench::percentile_ns(v, 99), 42u);
}

TEST(percentile_rounds_rank_up) {
    // 7 samples: p50 -> ceil(3.5) = rank 4, p99 -> ceil(6.93) = rank 7.
    const auto v = one_to(7);
    CHECK_EQ(hs::bench::percentile_ns(v, 50), 4u);
    CHECK_EQ(hs::bench::percentile_ns(v, 99), 7u);
    // Tiny p still returns the smallest sample, never index -1.
    CHECK_EQ(hs::bench::percentile_ns(v, 0.001), 1u);
}

TEST(percentile_exact_ranks_are_not_pushed_up_by_rounding) {
    // 7 / 100.0 * 100 is 7.000000000000001 in floating point; computing the
    // rank that way would round it up to 8. p * n / 100 is exact here.
    const auto v = one_to(100);
    CHECK_EQ(hs::bench::percentile_ns(v, 7), 7u);
    CHECK_EQ(hs::bench::percentile_ns(v, 14), 14u);
    const auto w = one_to(50);
    CHECK_EQ(hs::bench::percentile_ns(w, 50), 25u);
}

TEST(percentile_empty_is_zero) {
    const std::vector<uint64_t> v;
    CHECK_EQ(hs::bench::percentile_ns(v, 50), 0u);
}
