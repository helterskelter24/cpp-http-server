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
//
// Read contract. A server must follow all three rules:
//   (a) read from the socket only while wants_input() is true and its unsent
//       output is below kOutputHighWater;
//   (b) call process() after every on_data();
//   (c) whenever unsent output drops below kOutputHighWater and
//       has_pending_input() is true, call process() again instead of reading.
// Under this contract pending_input_bytes() never exceeds the size of one read
// (the largest `len` passed to on_data()). Reads happen only when nothing is
// buffered, so a read leaves at most `len` bytes; process() can only shrink
// that. A request split across reads does not stay here: the parser takes every
// byte it is given while it needs more, and holds the partial request itself,
// capped by max_header_bytes plus max_body_bytes. So the per-connection input
// memory is at most one read plus those parser limits.
//
// Rule (a) is what stops a pipelining client from growing the buffer: after a
// high-water stop the unanswered requests stay buffered, and reading more on
// top of them as soon as output drains would let input pile up without limit.
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
    size_t pending_input_bytes() const { return input_.size() - input_pos_; }

    // True when the server may read more from the socket (rule (a) above):
    // the connection is still open and every buffered byte has been handled.
    bool wants_input() const { return !closed_ && !has_pending_input(); }
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
    // No size limit of its own is needed: the read contract above bounds it
    // to one read.
    std::string input_;
    size_t input_pos_ = 0;

    unsigned served_ = 0;
    bool draining_ = false;
    bool closed_ = false;
};

}  // namespace hs
