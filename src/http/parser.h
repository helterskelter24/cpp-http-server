#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "http/request.h"

namespace hs {

// Decodes %XX sequences. Returns false on a malformed escape (missing or
// non-hex digits) and on %00: a NUL byte inside a path would truncate it for
// every C API used later (open, realpath), which is a classic filter bypass.
// '+' is left alone; it means space only in form-encoded query strings.
bool percent_decode(std::string_view in, std::string& out);

enum class ParseStatus { NeedMore, Complete, Error };

struct ParseResult {
    ParseStatus status;
    // Bytes of this feed() call that the parser took. NeedMore: always len.
    // Complete: offset just past the request's last byte, so the bytes after it
    // belong to the next request. Error: unspecified, but never more than len.
    size_t consumed;
    int error_status;  // HTTP status to answer with; 0 unless status == Error
};

struct ParserLimits {
    size_t max_header_bytes = 8192;
    size_t max_body_bytes = 1048576;
};

// Incremental HTTP/1.x request parser. Feed it whatever recv() returned; it never
// assumes a request arrives in one chunk. It stops at the end of one request, so
// the caller handles the request, calls reset(), and feeds the leftover bytes.
class HttpParser {
public:
    explicit HttpParser(ParserLimits limits = {});

    // After Complete (until reset) this returns Complete with consumed == 0; after
    // Error it keeps returning Error. Neither state ever touches the data.
    ParseResult feed(const char* data, size_t len);

    Request& request() { return request_; }  // valid after Complete
    bool started() const { return started_; }  // any request byte consumed since reset
    void reset();

private:
    enum class State { RequestLine, Headers, Body, Done, Error };

    // Outcome of handling one header-section byte.
    enum class Step { Continue, HeadersDone, Fail };

    Step consume_head_byte(char c);
    Step handle_line();
    Step parse_request_line(std::string_view line);
    Step parse_header_line(std::string_view line);
    Step finish_headers();
    Step fail(int status);

    ParserLimits limits_;
    State state_ = State::RequestLine;
    Request request_;
    std::string line_;          // the header-section line being assembled, CR included
    size_t head_bytes_ = 0;     // request line + headers + blank line, for max_header_bytes
    bool started_ = false;
    int error_status_ = 0;

    bool has_content_length_ = false;
    uint64_t content_length_ = 0;
    size_t body_remaining_ = 0;
    int host_count_ = 0;
};

}  // namespace hs
