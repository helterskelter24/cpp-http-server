#include "http/mime.h"

#include <string>

#include "http/ascii.h"

namespace hs {

namespace {

struct MimeEntry {
    std::string_view ext;
    std::string_view type;
};

// Small on purpose: a linear scan over a dozen entries is cheaper and simpler
// than a hash map.
constexpr MimeEntry kTable[] = {
    {"html", "text/html"},        {"css", "text/css"},     {"js", "application/javascript"},
    {"json", "application/json"}, {"txt", "text/plain"},   {"png", "image/png"},
    {"jpg", "image/jpeg"},        {"jpeg", "image/jpeg"},  {"gif", "image/gif"},
    {"svg", "image/svg+xml"},     {"ico", "image/x-icon"}, {"pdf", "application/pdf"},
    {"wasm", "application/wasm"},
};

}  // namespace

std::string_view mime_type(std::string_view path) {
    // Only look inside the last segment so "/v1.2/readme" has no extension.
    size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";

    std::string ext = to_lower_ascii(name.substr(dot + 1));
    for (const MimeEntry& e : kTable)
        if (e.ext == ext) return e.type;
    return "application/octet-stream";
}

}  // namespace hs
