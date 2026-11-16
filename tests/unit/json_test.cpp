#include "http/json.h"

#include <cctype>
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

TEST(json_escape_valid_utf8_unchanged) {
    CHECK_EQ(json_escape("\xc3\xa9"), "\xc3\xa9");                  // U+00E9, 2 bytes
    CHECK_EQ(json_escape("\xe2\x82\xac"), "\xe2\x82\xac");          // U+20AC, 3 bytes
    CHECK_EQ(json_escape("\xf0\x9f\x98\x80"), "\xf0\x9f\x98\x80");  // U+1F600, 4 bytes
    // The edges of each range: U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF,
    // and the code points just either side of the surrogates.
    CHECK_EQ(json_escape("\xc2\x80\xdf\xbf"), "\xc2\x80\xdf\xbf");
    CHECK_EQ(json_escape("\xe0\xa0\x80\xef\xbf\xbf"), "\xe0\xa0\x80\xef\xbf\xbf");
    CHECK_EQ(json_escape("\xed\x9f\xbf\xee\x80\x80"), "\xed\x9f\xbf\xee\x80\x80");
    CHECK_EQ(json_escape("\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"), "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf");
}

namespace {

// A hand-written check that s is a valid JSON string body (the text between the
// quotes) made only of ASCII: no raw control characters or quotes, and every
// backslash starts one of the escapes JSON allows. The inputs it is used on
// contain only invalid UTF-8 and ASCII, so the escaped output must be ASCII.
bool is_valid_json_string_body(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == '"') return false;
        if (c >= 0x80) return false;  // the cases below expect ASCII-only output
        if (c == '\\') {
            if (i + 1 >= s.size()) return false;
            char e = s[++i];
            if (e == 'u') {
                if (i + 4 >= s.size()) return false;
                for (int k = 1; k <= 4; ++k)
                    if (!std::isxdigit(static_cast<unsigned char>(s[i + k]))) return false;
                i += 4;
            } else if (std::string("\"\\/bfnrt").find(e) == std::string::npos) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST(json_escape_invalid_utf8_replaced) {
    const std::string r = "\\ufffd";
    CHECK_EQ(json_escape("a\x80"
                         "b"),
             "a" + r + "b");                          // lone continuation byte
    CHECK_EQ(json_escape("a\xe2\x82"), "a" + r + r);  // truncated at end of input
    CHECK_EQ(json_escape("\xe2\x82"
                         "a"),
             r + r + "a");                                     // truncated by the next character
    CHECK_EQ(json_escape("\xc0\xaf"), r + r);                  // overlong '/'
    CHECK_EQ(json_escape("\xe0\x80\xaf"), r + r + r);          // overlong '/', 3 bytes
    CHECK_EQ(json_escape("\xed\xa0\x80"), r + r + r);          // surrogate U+D800
    CHECK_EQ(json_escape("\xf4\x90\x80\x80"), r + r + r + r);  // U+110000, above the range
    CHECK_EQ(json_escape("\xff\xfe"), r + r);                  // bytes UTF-8 never uses
    // A valid character right after an invalid byte is kept.
    CHECK_EQ(json_escape("\xff\xc3\xa9"), r + "\xc3\xa9");

    const std::string inputs[] = {
        "a\x80"
        "b",
        "a\xe2\x82",
        "\xc0\xaf",
        "\xed\xa0\x80",
        "\xff\xfe",
        std::string("\xff\xfe\x00\x01\"\\", 6)};
    for (const std::string& in : inputs) CHECK(is_valid_json_string_body(json_escape(in)));
    // The checker itself rejects what the old code produced for these bytes.
    CHECK(!is_valid_json_string_body("\xff\xfe"));
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
    w.value(static_cast<uint64_t>(18446744073709551615ULL));
    w.value(static_cast<int64_t>(-9223372036854775807LL - 1));
    w.end_array();
    CHECK_EQ(w.str(), "[18446744073709551615,-9223372036854775808]");
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
