#include "server/http_conn.h"

#include <algorithm>

#include "http/response.h"
#include "logger.h"

namespace hs {

HttpConn::HttpConn(const Router& router, const Config& config, Stats& stats)
    : router_(router),
      config_(config),
      stats_(stats),
      parser_(ParserLimits{config.max_header_bytes, config.max_body_bytes}) {}

void HttpConn::on_data(const char* data, size_t len) {
    if (closed_) return;
    input_.append(data, len);
}

bool HttpConn::process(std::string& out) {
    if (closed_) return false;

    while (input_pos_ < input_.size() && out.size() < kOutputHighWater) {
        size_t available = input_.size() - input_pos_;
        ParseResult r = parser_.feed(input_.data() + input_pos_, available);
        input_pos_ += std::min(r.consumed, available);

        if (r.status == ParseStatus::NeedMore) break;  // every byte is now inside the parser

        if (r.status == ParseStatus::Error) {
            // After a framing error the rest of the stream cannot be trusted
            // to start at a request boundary, so the connection always closes.
            emit(Response::error(r.error_status), "-", "-", false, false, out);
            finish();
            return false;
        }

        Request& req = parser_.request();
        Response resp = router_.dispatch(req);
        ++served_;
        bool keep_alive = req.keep_alive && !draining_ && served_ < config_.max_keepalive_requests;
        emit(resp, method_name(req.method), req.path, keep_alive, req.method == Method::HEAD, out);
        parser_.reset();
        if (!keep_alive) {
            finish();
            return false;
        }
    }

    compact();
    return true;
}

bool HttpConn::has_pending_input() const { return input_pos_ < input_.size(); }

// The parser is reset right after each complete request, so "started" alone
// means a request is partway in. Leading blank lines do not set it.
bool HttpConn::mid_request() const { return !closed_ && parser_.started(); }

std::string HttpConn::timeout_response() const {
    return serialize(Response::error(408), false, false);
}

void HttpConn::emit(const Response& resp, std::string_view method, const std::string& path,
                    bool keep_alive, bool head_only, std::string& out) {
    out += serialize(resp, keep_alive, head_only);
    stats_.record(resp.status);
    if (config_.access_log) {
        // Body bytes actually sent, so a HEAD response logs 0.
        size_t bytes = head_only ? 0 : resp.body.size();
        log_msg(LogLevel::Info, "%.*s %s %d %zu", static_cast<int>(method.size()), method.data(),
                path.c_str(), resp.status, bytes);
    }
}

void HttpConn::finish() {
    closed_ = true;
    // Whatever followed the closing request is never answered, so it is dropped.
    input_.clear();
    input_.shrink_to_fit();
    input_pos_ = 0;
}

void HttpConn::compact() {
    if (input_pos_ == input_.size()) {
        input_.clear();
        input_pos_ = 0;
    } else if (input_pos_ > 0 && input_pos_ >= input_.size() / 2) {
        input_.erase(0, input_pos_);
        input_pos_ = 0;
    }
}

}  // namespace hs
