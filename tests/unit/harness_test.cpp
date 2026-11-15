#include <vector>

#include "test.h"

namespace {
enum class Color { Red, Green };
}

TEST(check_eq_mixed_sign_and_size) {
    std::vector<int> v = {1, 2};
    CHECK_EQ(v.size(), 2);  // size_t vs int literal
    CHECK_EQ(2, v.size());
    CHECK_EQ(static_cast<unsigned>(3), 3L);
}

TEST(check_eq_enum_class) {
    Color c = Color::Green;
    CHECK_EQ(c, Color::Green);
}
