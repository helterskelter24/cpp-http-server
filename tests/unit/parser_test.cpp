#include "http/parser.h"

#include <string>

#include "test.h"

using namespace hs;

namespace {

struct Outcome {
    ParseResult result{ParseStatus::NeedMore, 0, 0};
    Request request;
    bool started = false;
};

Outcome parse_all(const std::string& input, ParserLimits limits = {}) {
    HttpParser parser(limits);
    Outcome out;
    out.result = parser.feed(input.data(), input.size());
    out.request = parser.request();
    out.started = parser.started();
    return out;
}

// One byte per feed() call, stopping at the first Complete or Error. Every
// earlier call must be NeedMore. consumed is the number of bytes fed in total.
Outcome parse_bytewise(const std::string& input, ParserLimits limits = {}) {
    HttpParser parser(limits);
    Outcome out;
    size_t fed = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        out.result = parser.feed(input.data() + i, 1);
        ++fed;
        if (out.result.status != ParseStatus::NeedMore) break;
        CHECK_EQ(out.result.consumed, 1);
    }
    if (out.result.status != ParseStatus::NeedMore) out.result.consumed = fed;
    out.request = parser.request();
    out.started = parser.started();
    return out;
}

void check_same(const Outcome& a, const Outcome& b) {
    CHECK_EQ(a.result.status, b.result.status);
    CHECK_EQ(a.result.error_status, b.result.error_status);
    CHECK_EQ(a.started, b.started);
    if (a.result.status == ParseStatus::Complete) {
        CHECK_EQ(a.result.consumed, b.result.consumed);
        CHECK_EQ(a.request.method, b.request.method);
        CHECK_EQ(a.request.target, b.request.target);
        CHECK_EQ(a.request.path, b.request.path);
        CHECK_EQ(a.request.query, b.request.query);
        CHECK_EQ(a.request.version_minor, b.request.version_minor);
        CHECK_EQ(a.request.keep_alive, b.request.keep_alive);
        CHECK_EQ(a.request.body, b.request.body);
        CHECK_EQ(a.request.headers.all(), b.request.headers.all());
    }
}

// Parses in one piece and byte by byte, requires identical results, and returns
// the one-piece outcome so each test also checks its specific expectations.
Outcome parse(const std::string& input, ParserLimits limits = {}) {
    Outcome whole = parse_all(input, limits);
    Outcome bytes = parse_bytewise(input, limits);
    check_same(whole, bytes);
    return whole;
}

void expect_error(const std::string& input, int status, ParserLimits limits = {}) {
    Outcome out = parse(input, limits);
    CHECK_EQ(out.result.status, ParseStatus::Error);
    CHECK_EQ(out.result.error_status, status);
}

ParserLimits header_limit(size_t n) {
    ParserLimits limits;
    limits.max_header_bytes = n;
    return limits;
}

ParserLimits body_limit(size_t n) {
    ParserLimits limits;
    limits.max_body_bytes = n;
    return limits;
}

const std::string kGet = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";

}  // namespace

TEST(parser_simple_get) {
    std::string in = "GET /a/b?x=1 HTTP/1.1\r\nHost: h\r\n\r\n";
    Outcome out = parse(in);
    CHECK_EQ(out.result.status, ParseStatus::Complete);
    CHECK_EQ(out.result.consumed, in.size());
    CHECK_EQ(out.result.error_status, 0);
    CHECK(out.request.method == Method::GET);
    CHECK_EQ(out.request.path, "/a/b");
    CHECK_EQ(out.request.query, "x=1");
    CHECK_EQ(out.request.target, "/a/b?x=1");
    CHECK_EQ(out.request.version_minor, 1);
    CHECK(out.request.keep_alive);
    CHECK(out.request.body.empty());
}

TEST(parser_bytewise_equals_whole) {
    std::string in = "GET /a/b?x=1 HTTP/1.1\r\nHost: h\r\n\r\n";
    HttpParser parser;
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        ParseResult r = parser.feed(in.data() + i, 1);
        CHECK_EQ(r.status, ParseStatus::NeedMore);
        CHECK_EQ(r.consumed, 1);
    }
    ParseResult last = parser.feed(in.data() + in.size() - 1, 1);
    CHECK_EQ(last.status, ParseStatus::Complete);
    CHECK_EQ(last.consumed, 1);
    CHECK_EQ(parser.request().path, "/a/b");
    CHECK_EQ(parser.request().query, "x=1");
    Outcome whole = parse_all(in);
    CHECK_EQ(whole.request.headers.all(), parser.request().headers.all());
}

TEST(parser_partial_input_needs_more) {
    Outcome out = parse("GET / HTTP/1.1\r\nHost: h\r\n");
    CHECK_EQ(out.result.status, ParseStatus::NeedMore);
    CHECK_EQ(out.result.consumed, 25);
    Outcome cr = parse("GET / HTTP/1.1\r\nHost: h\r\n\r");  // CRLFCRLF still incomplete
    CHECK_EQ(cr.result.status, ParseStatus::NeedMore);
}

TEST(parser_post_with_body) {
    std::string in = "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
    Outcome out = parse(in);
    CHECK_EQ(out.result.status, ParseStatus::Complete);
    CHECK_EQ(out.result.consumed, in.size());
    CHECK(out.request.method == Method::POST);
    CHECK_EQ(out.request.body, "hello");
}

TEST(parser_body_split_across_feeds) {
    std::string head = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 6\r\n\r\n";
    HttpParser parser;
    std::string first = head + "abc";
    ParseResult r1 = parser.feed(first.data(), first.size());
    CHECK_EQ(r1.status, ParseStatus::NeedMore);
    CHECK_EQ(r1.consumed, first.size());
    std::string second = "defGET";  // extra bytes belong to the next request
    ParseResult r2 = parser.feed(second.data(), second.size());
    CHECK_EQ(r2.status, ParseStatus::Complete);
    CHECK_EQ(r2.consumed, 3);
    CHECK_EQ(parser.request().body, "abcdef");
}

TEST(parser_pipelined_two) {
    std::string second = "GET /two HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string both = kGet + second;
    HttpParser parser;
    ParseResult r1 = parser.feed(both.data(), both.size());
    CHECK_EQ(r1.status, ParseStatus::Complete);
    CHECK_EQ(r1.consumed, kGet.size());
    CHECK_EQ(parser.request().path, "/");
    parser.reset();
    const char* rest = both.data() + r1.consumed;
    ParseResult r2 = parser.feed(rest, both.size() - r1.consumed);
    CHECK_EQ(r2.status, ParseStatus::Complete);
    CHECK_EQ(r2.consumed, second.size());
    CHECK_EQ(parser.request().path, "/two");
}

TEST(parser_pipelined_body_then_next_request) {
    std::string first = "POST /p HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc";
    std::string both = first + kGet;
    Outcome out = parse(both);
    CHECK_EQ(out.result.status, ParseStatus::Complete);
    CHECK_EQ(out.result.consumed, first.size());
    CHECK_EQ(out.request.body, "abc");
}

TEST(parser_percent_decoding) {
    Outcome ok = parse("GET /a%20b HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_EQ(ok.result.status, ParseStatus::Complete);
    CHECK_EQ(ok.request.path, "/a b");
    CHECK_EQ(ok.request.target, "/a%20b");
    expect_error("GET /a%zz HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a%00 HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a% HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a%2 HTTP/1.1\r\nHost: h\r\n\r\n", 400);
}

TEST(parser_percent_decode_function) {
    std::string out;
    CHECK(percent_decode("a%2Fb%2fc", out));
    CHECK_EQ(out, "a/b/c");
    CHECK(percent_decode("a+b", out));
    CHECK_EQ(out, "a+b");
    CHECK(percent_decode("", out));
    CHECK_EQ(out, "");
    CHECK(percent_decode("%C3%A9", out));
    CHECK_EQ(out, "\xC3\xA9");
    CHECK(!percent_decode("%", out));
    CHECK(!percent_decode("%4", out));
    CHECK(!percent_decode("%g0", out));
    CHECK(!percent_decode("%0g", out));
    CHECK(!percent_decode("%00", out));
}

TEST(parser_query_not_decoded_fragment_dropped) {
    Outcome q = parse("GET /p%41?a=%26b&c=%zz HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_EQ(q.result.status, ParseStatus::Complete);
    CHECK_EQ(q.request.path, "/pA");
    CHECK_EQ(q.request.query, "a=%26b&c=%zz");
    Outcome f = parse("GET /p?a=1#frag?x HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_EQ(f.result.status, ParseStatus::Complete);
    CHECK_EQ(f.request.path, "/p");
    CHECK_EQ(f.request.query, "a=1");
    Outcome g = parse("GET /p#frag HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_EQ(g.request.path, "/p");
    CHECK_EQ(g.request.query, "");
    Outcome e = parse("GET /p? HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_EQ(e.request.path, "/p");
    CHECK_EQ(e.request.query, "");
}

TEST(parser_http10_keepalive) {
    Outcome plain = parse("GET / HTTP/1.0\r\n\r\n");
    CHECK_EQ(plain.result.status, ParseStatus::Complete);
    CHECK_EQ(plain.request.version_minor, 0);
    CHECK(!plain.request.keep_alive);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n").request.keep_alive);
    CHECK(!parse("GET / HTTP/1.0\r\nConnection: close\r\n\r\n").request.keep_alive);
    CHECK(!parse("GET / HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n").request.keep_alive);
    CHECK(!parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n").request.keep_alive);
    CHECK(!parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: foo, Close\r\n\r\n").request.keep_alive);
    CHECK(!parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: foo\r\nConnection: CLOSE\r\n\r\n")
               .request.keep_alive);
    CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive\r\n\r\n").request.keep_alive);
    CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: closed\r\n\r\n").request.keep_alive);
}

TEST(parser_host_rules) {
    expect_error("GET / HTTP/1.1\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);
    Outcome v10 = parse("GET / HTTP/1.0\r\n\r\n");
    CHECK_EQ(v10.result.status, ParseStatus::Complete);
}

TEST(parser_versions) {
    expect_error("GET / HTTP/1.2\r\nHost: h\r\n\r\n", 505);
    expect_error("GET / HTTP/2.0\r\nHost: h\r\n\r\n", 505);
    expect_error("GET / HTTP/0.9\r\nHost: h\r\n\r\n", 505);
    expect_error("GET / HTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET / http/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1 \r\nHost: h\r\n\r\n", 400);
    expect_error("GET / HTTP/1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET / HTTP/\r\nHost: h\r\n\r\n", 400);
    expect_error("GET / HTTP/1.10\r\nHost: h\r\n\r\n", 400);
}

TEST(parser_bad_request_lines) {
    expect_error("GET  / HTTP/1.1\r\nHost: h\r\n\r\n", 400);  // two spaces
    expect_error("GET HTTP/1.1\r\nHost: h\r\n\r\n", 400);     // missing target
    expect_error("GET /\r\nHost: h\r\n\r\n", 400);            // missing version
    expect_error("GET\r\nHost: h\r\n\r\n", 400);
    expect_error("GET abc HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("get / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("Get / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("G3T / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error(" GET / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a b HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a\x01 HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a\x7f HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("GET /a\tb HTTP/1.1\r\nHost: h\r\n\r\n", 400);
}

TEST(parser_methods) {
    struct Case {
        const char* token;
        Method method;
    };
    const Case cases[] = {{"GET", Method::GET},         {"HEAD", Method::HEAD},
                          {"POST", Method::POST},       {"PUT", Method::PUT},
                          {"DELETE", Method::DELETE_},  {"PATCH", Method::PATCH},
                          {"OPTIONS", Method::OPTIONS}, {"BREW", Method::UNKNOWN}};
    for (const Case& c : cases) {
        Outcome out = parse(std::string(c.token) + " / HTTP/1.1\r\nHost: h\r\n\r\n");
        CHECK_EQ(out.result.status, ParseStatus::Complete);
        CHECK(out.request.method == c.method);
    }
    // 16 bytes is the longest accepted method token; 17 is rejected.
    Outcome longest = parse(std::string(16, 'A') + " / HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_EQ(longest.result.status, ParseStatus::Complete);
    CHECK(longest.request.method == Method::UNKNOWN);
    expect_error(std::string(17, 'A') + " / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
}

TEST(parser_bad_headers) {
    expect_error("GET / HTTP/1.1\r\nHost : h\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: h\r\n\tfolded\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: h\x01z\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: h\x7f\r\n\r\n", 400);
    expect_error(std::string("GET / HTTP/1.1\r\nHost: h") + '\0' + "x\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: h\n\r\n", 400);           // bare LF in the value
    expect_error("GET / HTTP/1.1\nHost: h\r\n\r\n", 400);           // bare LF after request line
    expect_error("GET / HTTP/1.1\r\nHost: h\r\n\n", 400);           // bare LF as blank line
    expect_error("GET / HTTP/1.1\r\nHost: h\r\r\n\r\n", 400);       // CR not followed by LF
    expect_error("GET / HTTP/1.1\r\nHost: h\r\n: v\r\n\r\n", 400);  // empty name
    expect_error("GET / HTTP/1.1\r\nHo st: h\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHo(st: h\r\n\r\n", 400);
}

TEST(parser_header_values_and_names) {
    std::string in =
        "GET / HTTP/1.1\r\nHost: h\r\nX-Pad:   \t padded value \t \r\nX-Empty:\r\nX-Blank:   \r\n"
        "X-Tab: a\tb\r\nX-High: caf\xC3\xA9\r\nX-Dup: 1\r\nx-dup: 2\r\nUPPER-Name: v\r\n\r\n";
    Outcome out = parse(in);
    CHECK_EQ(out.result.status, ParseStatus::Complete);
    const Headers& h = out.request.headers;
    CHECK_EQ(h.size(), 9);
    CHECK(h.get("x-pad") != nullptr && *h.get("x-pad") == "padded value");
    CHECK(h.get("x-empty") != nullptr && h.get("x-empty")->empty());
    CHECK(h.get("x-blank") != nullptr && h.get("x-blank")->empty());
    CHECK(h.get("x-tab") != nullptr && *h.get("x-tab") == "a\tb");
    CHECK(h.get("x-high") != nullptr && *h.get("x-high") == "caf\xC3\xA9");
    CHECK(h.get("x-dup") != nullptr && *h.get("x-dup") == "1");  // first match
    CHECK_EQ(h.all()[7].first, "x-dup");
    CHECK_EQ(h.all()[7].second, "2");  // duplicates kept in order
    CHECK(h.get("Upper-Name") != nullptr);
    CHECK_EQ(h.all()[8].first, "upper-name");
}

TEST(parser_content_length_cases) {
    const char* bad[] = {"abc",
                         "-1",
                         "+1",
                         "1 2",
                         "",
                         "1,2",
                         "0x10",
                         "5 5",
                         "123456789012345678901234567890",
                         "18446744073709551616"};
    for (const char* value : bad) {
        expect_error(
            std::string("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: ") + value + "\r\n\r\n",
            400);
    }
    expect_error("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
                 400);
    expect_error("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\nContent-Length: 2x\r\n\r\nab",
                 400);

    Outcome same =
        parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab");
    CHECK_EQ(same.result.status, ParseStatus::Complete);
    CHECK_EQ(same.request.body, "ab");
}

TEST(parser_content_length_edge_values) {
    Outcome zeros = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0005\r\n\r\nhello");
    CHECK_EQ(zeros.result.status, ParseStatus::Complete);
    CHECK_EQ(zeros.request.body, "hello");
    Outcome many = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: " + std::string(40, '0') +
                         "3\r\n\r\nabc");
    CHECK_EQ(many.result.status, ParseStatus::Complete);
    CHECK_EQ(many.request.body, "abc");
    Outcome zero = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    CHECK_EQ(zero.result.status, ParseStatus::Complete);
    CHECK(zero.request.body.empty());
    Outcome padded = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length:  2 \r\n\r\nab");
    CHECK_EQ(padded.result.status, ParseStatus::Complete);
    // Numerically equal repeats are the same length, whatever the spelling.
    Outcome repeat =
        parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 02\r\nContent-Length: 2\r\n\r\nab");
    CHECK_EQ(repeat.result.status, ParseStatus::Complete);
    // No Content-Length means no body, even for POST.
    Outcome none = parse("POST / HTTP/1.1\r\nHost: h\r\n\r\nGET");
    CHECK_EQ(none.result.status, ParseStatus::Complete);
    CHECK(none.request.body.empty());
    CHECK_EQ(none.result.consumed, 28);
}

TEST(parser_transfer_encoding) {
    expect_error("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    expect_error("POST / HTTP/1.1\r\nHost: h\r\ntransfer-encoding: identity\r\n\r\n", 501);
    // With Content-Length as well this is the classic smuggling shape: still refused.
    expect_error(
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\nabcd",
        501);
}

TEST(parser_header_limit) {
    // A 200-byte line is rejected as soon as the limit is crossed, with no CRLFCRLF yet.
    std::string in = "GET / HTTP/1.1\r\nHost: h\r\nX-Big: " + std::string(200, 'a');
    expect_error(in, 431, header_limit(64));
    Outcome whole = parse_all(in, header_limit(64));
    CHECK(whole.result.consumed <= in.size());
    // Even the request line alone can trip it, without any line ending.
    expect_error("GET /" + std::string(100, 'a'), 431, header_limit(64));
}

TEST(parser_header_limit_boundary) {
    // kGet is 27 bytes: accepted at exactly that limit, refused one byte below.
    CHECK_EQ(kGet.size(), 27);
    Outcome at = parse(kGet, header_limit(27));
    CHECK_EQ(at.result.status, ParseStatus::Complete);
    expect_error(kGet, 431, header_limit(26));
    // The body does not count towards the header limit.
    Outcome body =
        parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n" + std::string(100, 'x'),
              header_limit(60));
    CHECK_EQ(body.result.status, ParseStatus::Complete);
}

TEST(parser_body_limit) {
    // Refused from the headers alone: no body byte has been fed.
    expect_error("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\n", 413, body_limit(10));
    Outcome at =
        parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n0123456789", body_limit(10));
    CHECK_EQ(at.result.status, ParseStatus::Complete);
    CHECK_EQ(at.request.body.size(), 10);
    expect_error("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999\r\n\r\n", 400);
    expect_error("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 18446744073709551615\r\n\r\n", 413);
}

TEST(parser_leading_crlf) {
    Outcome out = parse("\r\n\r\n" + kGet);
    CHECK_EQ(out.result.status, ParseStatus::Complete);
    CHECK_EQ(out.request.path, "/");
    HttpParser parser;
    ParseResult r = parser.feed("\r\n", 2);
    CHECK_EQ(r.status, ParseStatus::NeedMore);
    CHECK(!parser.started());
    r = parser.feed("\r", 1);
    CHECK(!parser.started());
    r = parser.feed("\n", 1);
    CHECK(!parser.started());
    r = parser.feed("G", 1);
    CHECK(parser.started());
    // Blank lines before the request do not count against the header limit.
    Outcome limited = parse("\r\n\r\n\r\n" + kGet, header_limit(27));
    CHECK_EQ(limited.result.status, ParseStatus::Complete);
    // But a bare LF there is still not a line ending, and a CR needs its LF.
    expect_error("\nGET / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_error("\rGET / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
}

TEST(parser_started_after_first_byte) {
    HttpParser parser;
    CHECK(!parser.started());
    parser.feed("GE", 2);
    CHECK(parser.started());
    parser.reset();
    CHECK(!parser.started());
}

TEST(parser_error_is_sticky) {
    HttpParser parser;
    std::string bad = "get / HTTP/1.1\r\n\r\n";
    ParseResult r = parser.feed(bad.data(), bad.size());
    CHECK_EQ(r.status, ParseStatus::Error);
    CHECK_EQ(r.error_status, 400);
    CHECK(r.consumed <= bad.size());
    for (int i = 0; i < 2; ++i) {
        r = parser.feed(kGet.data(), kGet.size());
        CHECK_EQ(r.status, ParseStatus::Error);
        CHECK_EQ(r.error_status, 400);
    }
    parser.reset();
    r = parser.feed(kGet.data(), kGet.size());
    CHECK_EQ(r.status, ParseStatus::Complete);
}

TEST(parser_reset_clears_everything) {
    HttpParser parser;
    std::string first =
        "POST /a?q=1 HTTP/1.0\r\nContent-Length: 2\r\nX-A: 1\r\nConnection: keep-alive\r\n\r\nab";
    ParseResult r = parser.feed(first.data(), first.size());
    CHECK_EQ(r.status, ParseStatus::Complete);
    CHECK(parser.request().keep_alive);
    parser.reset();
    r = parser.feed(kGet.data(), kGet.size());
    CHECK_EQ(r.status, ParseStatus::Complete);
    const Request& req = parser.request();
    CHECK(req.method == Method::GET);
    CHECK_EQ(req.path, "/");
    CHECK_EQ(req.query, "");
    CHECK(req.body.empty());
    CHECK_EQ(req.headers.size(), 1);
    CHECK_EQ(req.version_minor, 1);
    CHECK(req.keep_alive);
}

TEST(parser_feed_after_complete_takes_nothing) {
    HttpParser parser;
    ParseResult r = parser.feed(kGet.data(), kGet.size());
    CHECK_EQ(r.status, ParseStatus::Complete);
    ParseResult again = parser.feed("GET /x HTTP/1.1\r\n", 17);
    CHECK_EQ(again.status, ParseStatus::Complete);
    CHECK_EQ(again.consumed, 0);
    CHECK_EQ(parser.request().path, "/");
}

TEST(parser_empty_feed_is_harmless) {
    HttpParser parser;
    ParseResult r = parser.feed(nullptr, 0);
    CHECK_EQ(r.status, ParseStatus::NeedMore);
    CHECK_EQ(r.consumed, 0);
    CHECK(!parser.started());
}

TEST(parser_arbitrary_chunk_boundaries) {
    std::string in = "POST /x?y=1 HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\n123456789";
    for (size_t cut = 1; cut < in.size(); ++cut) {
        HttpParser parser;
        ParseResult a = parser.feed(in.data(), cut);
        CHECK_EQ(a.status, ParseStatus::NeedMore);
        ParseResult b = parser.feed(in.data() + cut, in.size() - cut);
        CHECK_EQ(b.status, ParseStatus::Complete);
        CHECK_EQ(b.consumed, in.size() - cut);
        CHECK_EQ(parser.request().body, "123456789");
        CHECK_EQ(parser.request().query, "y=1");
    }
}
