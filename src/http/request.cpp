#include "http/request.h"

#include "http/ascii.h"

namespace hs {

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
    items_.emplace_back(to_lower_ascii(name), std::string(value));
}

const std::string* Headers::get(std::string_view name) const {
    std::string wanted = to_lower_ascii(name);
    for (const auto& item : items_) {
        if (item.first == wanted) return &item.second;
    }
    return nullptr;
}

}  // namespace hs
