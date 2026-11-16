#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hs {

// Escapes a string for use inside JSON quotes. The result is always valid JSON:
// valid UTF-8 sequences pass through unchanged, and every byte that is not part
// of one (a stray continuation byte, a truncated or overlong sequence, an
// encoded surrogate, anything above U+10FFFF) becomes the escape \ufffd.
std::string json_escape(std::string_view s);

// Builds JSON text in one buffer and inserts the commas itself. It does not
// check that calls are well-formed (e.g. a value without a key inside an object).
class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);
    JsonWriter& value(std::string_view s);
    JsonWriter& value(const char* s);
    JsonWriter& value(int64_t n);
    JsonWriter& value(uint64_t n);
    JsonWriter& value(double d);
    JsonWriter& value(bool b);
    JsonWriter& null();
    std::string str() const { return out_; }

private:
    // Writes the comma that separates this item from the previous sibling.
    void before_item();

    std::string out_;
    // One entry per open container: has it received its first item yet?
    std::vector<bool> has_item_;
    // True right after key(): the value that follows must not get a comma.
    bool after_key_ = false;
};

}  // namespace hs
