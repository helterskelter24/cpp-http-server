#include "http/json.h"

#include <cmath>
#include <cstdio>

namespace hs {

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void JsonWriter::before_item() {
    if (after_key_) {
        after_key_ = false;  // the key already placed the comma
        return;
    }
    if (!has_item_.empty()) {
        if (has_item_.back()) out_ += ',';
        has_item_.back() = true;
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_item();
    out_ += '{';
    has_item_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    out_ += '}';
    has_item_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_item();
    out_ += '[';
    has_item_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    out_ += ']';
    has_item_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    before_item();
    out_ += '"' + json_escape(name) + "\":";
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view s) {
    before_item();
    out_ += '"' + json_escape(s) + '"';
    return *this;
}

// Without this overload a string literal would pick value(bool) (pointer to
// bool is a standard conversion, string_view a user-defined one).
JsonWriter& JsonWriter::value(const char* s) { return value(std::string_view(s)); }

JsonWriter& JsonWriter::value(int64_t n) {
    before_item();
    out_ += std::to_string(n);
    return *this;
}

JsonWriter& JsonWriter::value(uint64_t n) {
    before_item();
    out_ += std::to_string(n);
    return *this;
}

JsonWriter& JsonWriter::value(double d) {
    // JSON has no NaN or Infinity, so those become null.
    if (!std::isfinite(d)) return null();
    before_item();
    // snprintf with %g uses the C locale's decimal point only if setlocale was
    // never called; this server never calls it, so output is always '.'.
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", d);
    out_ += buf;
    return *this;
}

JsonWriter& JsonWriter::value(bool b) {
    before_item();
    out_ += b ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_item();
    out_ += "null";
    return *this;
}

}  // namespace hs
