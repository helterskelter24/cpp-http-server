#include "http/response.h"

#include <cstdio>
#include <ctime>

#include "http/json.h"

namespace hs {

Response Response::json(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "application/json");
    r.body = std::move(body);
    return r;
}

Response Response::text(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    r.body = std::move(body);
    return r;
}

Response Response::error(int status) {
    JsonWriter w;
    w.begin_object().key("error").begin_object();
    w.key("status").value(static_cast<int64_t>(status));
    w.key("message").value(reason_phrase(status));
    w.end_object().end_object();
    return json(status, w.str());
}

std::string_view reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string http_date(std::time_t t) {
    // Fixed English tables instead of strftime's %a/%b, which follow the locale;
    // HTTP dates must always be English.
    static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::tm tm{};
    gmtime_r(&t, &tm);  // the thread-safe variant of gmtime
    char num[32];
    std::string out = days[tm.tm_wday];
    out += ", ";
    std::snprintf(num, sizeof num, "%02d ", tm.tm_mday);
    out += num;
    out += months[tm.tm_mon];
    std::snprintf(num, sizeof num, " %04d %02d:%02d:%02d GMT", tm.tm_year + 1900, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    out += num;
    return out;
}

std::string serialize(const Response& response, bool keep_alive, bool head_only) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " ";
    out += reason_phrase(response.status);
    out += "\r\n";
    for (const auto& h : response.headers) {
        out += h.first + ": " + h.second + "\r\n";
    }
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Date: " + http_date(std::time(nullptr)) + "\r\n";
    out += "Server: cpp-http-server\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    out += "\r\n";
    if (!head_only) out += response.body;
    return out;
}

}  // namespace hs
