#include "http/json.h"

#include <cstdio>

namespace hs {

namespace {

// Returns the length of the valid UTF-8 sequence that starts at s[i], or 0 if
// the byte at s[i] does not start one. s[i] must be >= 0x80.
//
// The lead byte gives the sequence length; every following byte must be a
// continuation byte (10xxxxxx). The decoded code point is then checked, because
// well-formed bit patterns can still be invalid UTF-8:
//  - overlong: a code point encoded in more bytes than it needs (C0 AF is '/'),
//    which decoders that accept it turn into a way to smuggle characters past
//    checks;
//  - surrogates U+D800-U+DFFF, which are reserved for UTF-16 and never valid in
//    UTF-8 (strict JSON parsers reject them);
//  - anything above U+10FFFF, the last Unicode code point.
size_t utf8_sequence_length(std::string_view s, size_t i) {
    unsigned char lead = static_cast<unsigned char>(s[i]);
    size_t len;
    uint32_t cp;
    uint32_t min_cp;  // smallest code point that really needs len bytes
    if (lead >= 0xC0 && lead <= 0xDF) {
        len = 2;
        cp = lead & 0x1F;
        min_cp = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        len = 3;
        cp = lead & 0x0F;
        min_cp = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF7) {
        len = 4;
        cp = lead & 0x07;
        min_cp = 0x10000;
    } else {
        return 0;  // a continuation byte on its own, or F8-FF, which UTF-8 never uses
    }
    if (s.size() - i < len) return 0;  // truncated at the end of the input
    for (size_t k = 1; k < len; ++k) {
        unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) return 0;  // truncated: the next character started early
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min_cp) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp > 0x10FFFF) return 0;
    return len;
}

}  // namespace

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) {
            // JSON text must be UTF-8 (RFC 8259), and the input may be any
            // bytes (a request body, for one). Valid sequences are copied as
            // they are; each byte that is not part of one becomes U+FFFD, the
            // replacement character. That loses the original byte value, but
            // the output always parses, and the length of the input is still
            // reported separately where it matters (body_length in echo).
            size_t len = utf8_sequence_length(s, i);
            if (len == 0) {
                out += "\\ufffd";
                ++i;
            } else {
                out.append(s.data() + i, len);
                i += len;
            }
            continue;
        }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", u);
                    out += buf;
                } else {
                    out += c;
                }
        }
        ++i;
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
