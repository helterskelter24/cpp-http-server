#include <exception>
#include <iostream>
#include <string>

#include "test.h"

// Runs every registered test. An optional first argument selects only the
// tests whose name contains that substring.
int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    int matched = 0;
    for (const auto& test : hs_test::registry()) {
        if (test.name.find(filter) == std::string::npos) continue;
        ++matched;
        hs_test::current_failures() = 0;
        // An exception escaping a test counts as that test's failure; the run goes on.
        try {
            test.fn();
        } catch (const std::exception& e) {
            hs_test::report_failure(test.name.c_str(), 0,
                                    std::string("uncaught exception: ") + e.what());
        } catch (...) {
            hs_test::report_failure(test.name.c_str(), 0, "uncaught non-standard exception");
        }
        if (hs_test::current_failures() == 0) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "  in test: " << test.name << "\n";
        }
    }
    // A typo in the filter must not look like a green run.
    if (matched == 0) {
        std::cerr << "no test matches filter '" << filter << "'\n";
        return 1;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
