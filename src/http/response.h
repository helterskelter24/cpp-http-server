#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hs {

struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    static Response json(int status, std::string body);  // Content-Type: application/json
    static Response text(int status, std::string body);  // text/plain; charset=utf-8
    static Response error(int status);                   // JSON error body
};

// "Unknown" for codes that are not listed.
std::string_view reason_phrase(int status);

// IMF-fixdate in GMT, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
std::string http_date(std::time_t t);

// Full wire form of a response. With head_only the body is left out but
// Content-Length still states its size, as a HEAD response must.
std::string serialize(const Response& response, bool keep_alive, bool head_only);

}  // namespace hs
