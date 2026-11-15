#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hs {

// DELETE_ has a trailing underscore because DELETE is a macro on some platforms.
enum class Method { GET, HEAD, POST, PUT, DELETE_, PATCH, OPTIONS, UNKNOWN };

std::string_view method_name(Method m);

// Header names are stored lower-cased so lookups are a plain string compare.
// A vector keeps insertion order and duplicates; header counts are small, so a
// linear scan is cheaper than a map.
class Headers {
public:
    void add(std::string_view name, std::string_view value);
    // First match, case-insensitive; nullptr if absent.
    const std::string* get(std::string_view name) const;
    size_t size() const { return items_.size(); }
    const std::vector<std::pair<std::string, std::string>>& all() const { return items_; }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct Request {
    Method method = Method::UNKNOWN;
    std::string target, path, query;  // path is percent-decoded
    int version_minor = 1;            // 0 or 1
    Headers headers;
    std::string body;
    std::unordered_map<std::string, std::string> params;  // filled by Router
    bool keep_alive = true;                               // computed by the parser
};

}  // namespace hs
