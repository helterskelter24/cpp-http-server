#include "http/request.h"

namespace hs {

namespace {
// Not std::tolower: that depends on the C locale and on int/unsigned char conversions.
char to_lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = to_lower_ascii(c);
    return out;
}
}  // namespace

std::string_view method_name(Method m) {
    switch (m) {
        case Method::GET: return "GET";
        case Method::HEAD: return "HEAD";
        case Method::POST: return "POST";
        case Method::PUT: return "PUT";
        case Method::DELETE_: return "DELETE";
        case Method::PATCH: return "PATCH";
        case Method::OPTIONS: return "OPTIONS";
        case Method::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

void Headers::add(std::string_view name, std::string_view value) {
    items_.emplace_back(lower(name), std::string(value));
}

const std::string* Headers::get(std::string_view name) const {
    std::string wanted = lower(name);
    for (const auto& item : items_) {
        if (item.first == wanted) return &item.second;
    }
    return nullptr;
}

}  // namespace hs
