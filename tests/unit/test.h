#pragma once

// Minimal unit-test harness: TEST registers a function before main runs,
// CHECK and CHECK_EQ record a failure and let the test keep going.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace hs_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

// Function-local static avoids the static initialization order problem:
// TEST registrars in other translation units run before main().
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

// Failures recorded by the test that is currently running.
inline int& current_failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++current_failures();
    std::cerr << file << ":" << line << ": FAILED: " << what << "\n";
}

template <typename A, typename B>
void check_eq(const char* file, int line, const char* a_text, const char* b_text, const A& a,
              const B& b) {
    if (a == b) return;
    std::ostringstream out;
    out << a_text << " == " << b_text << "\n    left:  " << a << "\n    right: " << b;
    report_failure(file, line, out.str());
}

}  // namespace hs_test

#define TEST(name)                                                       \
    static void test_##name();                                           \
    static ::hs_test::Registrar registrar_##name(#name, &test_##name);   \
    static void test_##name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) ::hs_test::report_failure(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b) ::hs_test::check_eq(__FILE__, __LINE__, #a, #b, (a), (b))
