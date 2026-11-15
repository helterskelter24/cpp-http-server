#include "http/parser.h"

#include <limits>

namespace hs {

namespace {

char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) return false;
    }
    return true;
}

// -1 if c is not a hex digit.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Control characters and DEL. Tab is a control character too; callers that
// allow it check for it first.
bool is_control(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f;
}

// RFC 9110 "tchar": the characters allowed in a header name.
bool is_token_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~': return true;
        default: return false;
    }
}

std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Digits only; false on empty input, any other character, or overflow of uint64_t.
// Leading zeros are fine and do not count towards overflow.
bool parse_decimal(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

Method method_from_token(std::string_view token) {
    if (token == "GET") return Method::GET;
    if (token == "HEAD") return Method::HEAD;
    if (token == "POST") return Method::POST;
    if (token == "PUT") return Method::PUT;
    if (token == "DELETE") return Method::DELETE_;
    if (token == "PATCH") return Method::PATCH;
    if (token == "OPTIONS") return Method::OPTIONS;
    return Method::UNKNOWN;
}

// Longest method token accepted. Real methods are far shorter; the cap only
// stops a client from making us treat a huge token as a "method".
constexpr size_t kMaxMethodLength = 16;

// Scans the comma-separated tokens of every Connection header.
void scan_connection_tokens(const Headers& headers, bool& has_close, bool& has_keep_alive) {
    has_close = false;
    has_keep_alive = false;
    for (const auto& item : headers.all()) {
        if (item.first != "connection") continue;
        std::string_view rest = item.second;
        while (true) {
            size_t comma = rest.find(',');
            std::string_view token = trim_ows(rest.substr(0, comma));
            if (iequals(token, "close")) has_close = true;
            if (iequals(token, "keep-alive")) has_keep_alive = true;
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
}

}  // namespace

bool percent_decode(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (in.size() - i < 3) return false;  // '%' must be followed by two more bytes
        int hi = hex_value(in[i + 1]);
        int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        int byte = hi * 16 + lo;
        if (byte == 0) return false;
        out.push_back(static_cast<char>(byte));
        i += 2;
    }
    return true;
}

HttpParser::HttpParser(ParserLimits limits) : limits_(limits) {}

void HttpParser::reset() {
    state_ = State::RequestLine;
    request_ = Request{};
    line_.clear();
    head_bytes_ = 0;
    started_ = false;
    error_status_ = 0;
    has_content_length_ = false;
    content_length_ = 0;
    body_remaining_ = 0;
    host_count_ = 0;
}

HttpParser::Step HttpParser::fail(int status) {
    state_ = State::Error;
    error_status_ = status;
    return Step::Fail;
}

ParseResult HttpParser::feed(const char* data, size_t len) {
    if (state_ == State::Error) return {ParseStatus::Error, 0, error_status_};
    if (state_ == State::Done) return {ParseStatus::Complete, 0, 0};

    size_t i = 0;
    while (i < len) {
        if (state_ == State::Body) {
            // Bulk copy: bodies can be large, so no per-byte work here.
            size_t take = len - i;
            if (take > body_remaining_) take = body_remaining_;
            request_.body.append(data + i, take);
            i += take;
            body_remaining_ -= take;
            if (body_remaining_ == 0) {
                state_ = State::Done;
                return {ParseStatus::Complete, i, 0};
            }
            continue;
        }

        Step step = consume_head_byte(data[i]);
        ++i;
        if (step == Step::Fail) return {ParseStatus::Error, i, error_status_};
        if (step == Step::HeadersDone) {
            if (body_remaining_ == 0) {
                state_ = State::Done;
                return {ParseStatus::Complete, i, 0};
            }
            state_ = State::Body;
        }
    }
    return {ParseStatus::NeedMore, len, 0};
}

HttpParser::Step HttpParser::consume_head_byte(char c) {
    // Empty lines before the request line are tolerated (some clients send an
    // extra CRLF after a POST body). They must not count as "started", or an
    // idle keep-alive connection would look like a half-sent request.
    bool leading_blank = state_ == State::RequestLine &&
                         ((c == '\r' && line_.empty()) || (c == '\n' && line_ == "\r"));
    if (!leading_blank) started_ = true;

    if (++head_bytes_ > limits_.max_header_bytes) return fail(431);

    // Strict CRLF: a CR must be followed by LF and an LF must follow a CR.
    // Accepting a bare LF is how parsers end up disagreeing with proxies about
    // where a header ends.
    bool pending_cr = !line_.empty() && line_.back() == '\r';
    if (pending_cr && c != '\n') return fail(400);
    if (c != '\n') {
        line_.push_back(c);
        return Step::Continue;
    }
    if (!pending_cr) return fail(400);

    line_.pop_back();  // drop the CR
    Step step = handle_line();
    line_.clear();
    return step;
}

HttpParser::Step HttpParser::handle_line() {
    if (state_ == State::RequestLine) {
        if (line_.empty()) {
            head_bytes_ = 0;  // skipped blank line: not part of the request
            return Step::Continue;
        }
        Step step = parse_request_line(line_);
        if (step == Step::Continue) state_ = State::Headers;
        return step;
    }
    if (line_.empty()) return finish_headers();
    return parse_header_line(line_);
}

HttpParser::Step HttpParser::parse_request_line(std::string_view line) {
    // Exactly METHOD SP target SP version: a second space or a missing part is
    // an error rather than something to be lenient about.
    size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return fail(400);
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return fail(400);

    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);

    if (method.empty() || method.size() > kMaxMethodLength) return fail(400);
    for (char c : method) {
        if (c < 'A' || c > 'Z') return fail(400);
    }

    // Origin-form only: "*" and absolute URLs are not supported.
    if (target.empty() || target[0] != '/') return fail(400);
    for (char c : target) {
        if (is_control(c)) return fail(400);
    }

    if (version.substr(0, 5) != "HTTP/") return fail(400);
    std::string_view number = version.substr(5);
    bool shaped = number.size() == 3 && number[0] >= '0' && number[0] <= '9' && number[1] == '.' &&
                  number[2] >= '0' && number[2] <= '9';
    if (!shaped) return fail(400);
    if (number == "1.1") {
        request_.version_minor = 1;
    } else if (number == "1.0") {
        request_.version_minor = 0;
    } else {
        return fail(505);
    }

    // The fragment never reaches a server in practice; if a client sends one
    // anyway it is dropped so it cannot end up in the path or query.
    size_t hash = target.find('#');
    if (hash != std::string_view::npos) target = target.substr(0, hash);

    std::string_view raw_path = target;
    std::string_view query;
    size_t question = target.find('?');
    if (question != std::string_view::npos) {
        raw_path = target.substr(0, question);
        query = target.substr(question + 1);
    }
    // Only the path is decoded. The query keeps its %XX so a later handler can
    // still tell an encoded '&' from a separator.
    std::string decoded;
    if (!percent_decode(raw_path, decoded)) return fail(400);

    request_.method = method_from_token(method);
    request_.target = std::string(target);
    request_.path = std::move(decoded);
    request_.query = std::string(query);
    return Step::Continue;
}

HttpParser::Step HttpParser::parse_header_line(std::string_view line) {
    // A line starting with SP/HT would be an obsolete folded continuation. Its
    // meaning differs between implementations, so it is rejected.
    if (line[0] == ' ' || line[0] == '\t') return fail(400);

    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return fail(400);
    std::string_view name = line.substr(0, colon);
    for (char c : name) {
        if (!is_token_char(c)) return fail(400);  // also rejects "Host : h"
    }

    std::string_view value = line.substr(colon + 1);
    for (char c : value) {
        if (c != '\t' && is_control(c)) return fail(400);
    }
    value = trim_ows(value);

    if (iequals(name, "transfer-encoding")) return fail(501);

    if (iequals(name, "content-length")) {
        uint64_t parsed = 0;
        if (!parse_decimal(value, parsed)) return fail(400);
        // Two different lengths mean two parsers could split the stream differently.
        if (has_content_length_ && parsed != content_length_) return fail(400);
        has_content_length_ = true;
        content_length_ = parsed;
    }
    if (iequals(name, "host")) ++host_count_;

    request_.headers.add(name, value);
    return Step::Continue;
}

HttpParser::Step HttpParser::finish_headers() {
    if (host_count_ > 1) return fail(400);
    if (request_.version_minor == 1 && host_count_ == 0) return fail(400);

    // Rejected here, before a single body byte is read or buffered.
    if (has_content_length_ && content_length_ > limits_.max_body_bytes) return fail(413);
    body_remaining_ = static_cast<size_t>(content_length_);

    bool has_close = false;
    bool has_keep_alive = false;
    scan_connection_tokens(request_.headers, has_close, has_keep_alive);
    if (request_.version_minor == 1) {
        request_.keep_alive = !has_close;
    } else {
        request_.keep_alive = has_keep_alive && !has_close;
    }
    return Step::HeadersDone;
}

}  // namespace hs
