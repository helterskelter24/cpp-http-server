#include <string>

#include "http/request.h"
#include "http/response.h"
#include "test.h"

using namespace hs;

namespace {
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}
}  // namespace

TEST(headers_case_insensitive) {
    Headers h;
    h.add("Content-Type", "text/plain");
    const std::string* v = h.get("content-type");
    CHECK(v != nullptr);
    if (v) CHECK_EQ(*v, "text/plain");
    const std::string* upper = h.get("CONTENT-TYPE");
    CHECK(upper != nullptr);
    if (upper) CHECK_EQ(*upper, "text/plain");
    CHECK(h.get("x") == nullptr);
    CHECK_EQ(h.size(), 1);
}

TEST(serialize_basic) {
    std::string out = serialize(Response::text(200, "hi"), true, false);
    CHECK(out.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(contains(out, "Content-Length: 2\r\n"));
    CHECK(contains(out, "Connection: keep-alive\r\n"));
    CHECK(contains(out, "Server: cpp-http-server\r\n"));
    CHECK(ends_with(out, "\r\n\r\nhi"));
}

TEST(serialize_head_only) {
    std::string out = serialize(Response::text(200, "hi"), true, true);
    CHECK(ends_with(out, "\r\n\r\n"));
    CHECK(contains(out, "Content-Length: 2"));
}

TEST(serialize_close) {
    std::string out = serialize(Response::text(200, "hi"), false, false);
    CHECK(contains(out, "Connection: close\r\n"));
}

TEST(error_body) {
    Response r = Response::error(404);
    CHECK_EQ(r.status, 404);
    CHECK_EQ(r.body, R"({"error":{"status":404,"message":"Not Found"}})");
}

TEST(http_date_format) {
    CHECK_EQ(http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
}

TEST(reason_phrases) {
    CHECK_EQ(reason_phrase(200), "OK");
    CHECK_EQ(reason_phrase(505), "HTTP Version Not Supported");
    CHECK_EQ(reason_phrase(299), "Unknown");
}
