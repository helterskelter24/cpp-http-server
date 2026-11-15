#pragma once

// Minimal unit-test harness: TEST registers a function before main runs,
// CHECK and CHECK_EQ record a failure and let the test keep going.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
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

// Printing for failure messages. print_value(out, v, 0) prefers the first
// overload (taken only if `out << v` compiles); otherwise the `long` overload
// is used, since 0 converts to long less well than to int. Enums print as their
// underlying number and anything else as a placeholder, so CHECK_EQ works for
// every type that has operator==.
template <typename T>
auto print_value(std::ostream& out, const T& value, int) -> decltype(out << value, void()) {
    out << value;
}

template <typename T>
void print_value(std::ostream& out, const T& value, long) {
    if constexpr (std::is_enum_v<T>) {
        out << static_cast<std::underlying_type_t<T>>(value);
    } else {
        (void)value;
        out << "<unprintable>";
    }
}

// Equality used by CHECK_EQ. Comparing a signed with an unsigned integer using
// == would convert the signed one silently (and trip -Wsign-compare), so that
// mixed case is handled explicitly: a negative number never equals an unsigned one.
template <typename A, typename B>
bool values_equal(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B> &&
                  std::is_signed_v<A> != std::is_signed_v<B>) {
        if constexpr (std::is_signed_v<A>) {
            return a >= 0 && static_cast<unsigned long long>(a) == static_cast<unsigned long long>(b);
        } else {
            return b >= 0 && static_cast<unsigned long long>(a) == static_cast<unsigned long long>(b);
        }
    } else {
        return a == b;
    }
}

template <typename A, typename B>
void check_eq(const char* file, int line, const char* a_text, const char* b_text, const A& a,
              const B& b) {
    if (values_equal(a, b)) return;
    std::ostringstream out;
    out << a_text << " == " << b_text << "\n    left:  ";
    print_value(out, a, 0);
    out << "\n    right: ";
    print_value(out, b, 0);
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
