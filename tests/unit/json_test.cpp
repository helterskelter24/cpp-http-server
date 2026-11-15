#include "http/json.h"

#include <cmath>
#include <string>

#include "test.h"

using namespace hs;

TEST(json_escape_cases) {
    CHECK_EQ(json_escape("\""), "\\\"");
    CHECK_EQ(json_escape("\\"), "\\\\");
    CHECK_EQ(json_escape("\n"), "\\n");
    CHECK_EQ(json_escape("\t"), "\\t");
    CHECK_EQ(json_escape(std::string(1, '\x01')), "\\u0001");
    CHECK_EQ(json_escape("caf\xc3\xa9"), "caf\xc3\xa9");
}

TEST(json_writer_nested) {
    JsonWriter w;
    w.begin_object();
    w.key("a").value("x");
    w.key("b").value(static_cast<int64_t>(-3));
    w.key("c").value(true);
    w.key("d").null();
    w.key("e")
        .begin_array()
        .value(static_cast<int64_t>(1))
        .value(static_cast<int64_t>(2))
        .end_array();
    w.end_object();
    CHECK_EQ(w.str(), R"({"a":"x","b":-3,"c":true,"d":null,"e":[1,2]})");
}

TEST(json_writer_numbers) {
    JsonWriter w;
    w.begin_array();
    w.value(1.5).value(static_cast<uint64_t>(18446744073709551615ULL));
    w.value(std::nan("")).value(INFINITY);
    w.end_array();
    CHECK_EQ(w.str(), "[1.5,18446744073709551615,null,null]");
}

TEST(json_writer_empty_containers_and_escaped_key) {
    JsonWriter w;
    w.begin_object()
        .key("k\"")
        .begin_array()
        .end_array()
        .key("o")
        .begin_object()
        .end_object()
        .end_object();
    CHECK_EQ(w.str(), R"({"k\"":[],"o":{}})");
}
