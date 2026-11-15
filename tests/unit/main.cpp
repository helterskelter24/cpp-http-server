#include <iostream>
#include <string>

#include "test.h"

// Runs every registered test. An optional first argument selects only the
// tests whose name contains that substring.
int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    for (const auto& test : hs_test::registry()) {
        if (test.name.find(filter) == std::string::npos) continue;
        hs_test::current_failures() = 0;
        test.fn();
        if (hs_test::current_failures() == 0) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "  in test: " << test.name << "\n";
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
