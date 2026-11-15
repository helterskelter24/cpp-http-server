#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "config.h"
#include "http/parser.h"
#include "http/router.h"
#include "server/stats.h"

namespace hs {

// Once this many response bytes are waiting to be sent, process() stops
// handling pipelined requests until the caller has written some of them out.
constexpr size_t kOutputHighWater = 1u << 20;  // 1 MiB

// The HTTP side of one connection, with no socket in it: the server pushes in
// whatever bytes it read and writes out whatever process() produced. Both
// servers (blocking pool and epoll) use it, so pipelining order, keep-alive
// decisions and output backpressure behave the same in both and are tested
// without any networking. One instance belongs to one thread at a time.
class HttpConn {
public:
    HttpConn(const Router& router, const Config& config, Stats& stats);

    // Appends bytes read from the socket to the pending input. Ignored once the
    // connection has decided to close.
    void on_data(const char* data, size_t len);

    // Handles complete requests from the pending input in arrival order,
    // appending each serialized response to `out`. Stops when the input runs
    // out, when out.size() reaches kOutputHighWater, or after a response that
    // closes the connection. Returns false when the connection must be closed
    // once `out` has been flushed; after that, every call returns false and
    // does nothing.
    bool process(std::string& out);

    bool has_pending_input() const;  // unparsed bytes are buffered
    bool mid_request() const;        // a request has started but is not complete

    // Shutdown has begun: every later response says Connection: close.
    void set_draining() { draining_ = true; }

    // A 408 with Connection: close, for a request that took too long to arrive.
    // It does not touch Stats; the caller that sends it records the 408.
    std::string timeout_response() const;

    unsigned requests_served() const { return served_; }

private:
    // Appends one response, counts it and writes the access-log line.
    // `method` and `path` are only used for the log.
    void emit(const Response& resp, std::string_view method, const std::string& path,
              bool keep_alive, bool head_only, std::string& out);
    void finish();   // the connection is done: drop leftover input
    void compact();  // reclaim the consumed prefix of input_

    const Router& router_;
    const Config& config_;
    Stats& stats_;
    HttpParser parser_;

    // Unparsed input is input_[input_pos_ ..]. Advancing an offset instead of
    // erasing after every request keeps pipelined input linear rather than
    // quadratic; compact() erases the consumed prefix once it is at least half
    // the buffer, so each byte is moved O(1) times on average.
    //
    // This buffer needs no size limit of its own, provided the server keeps two
    // rules: call process() after every on_data(), and stop reading from the
    // socket while its unsent output is at or above kOutputHighWater. process()
    // returns early only for high output (or to close), so whenever the server
    // is allowed to read again the parser has taken every buffered byte; a
    // partial request lives in the parser, which caps it at max_header_bytes +
    // max_body_bytes. The buffer therefore never holds more than one read's
    // worth of bytes.
    std::string input_;
    size_t input_pos_ = 0;

    unsigned served_ = 0;
    bool draining_ = false;
    bool closed_ = false;
};

}  // namespace hs
