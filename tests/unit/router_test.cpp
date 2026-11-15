#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "http/router.h"
#include "test.h"

namespace {

hs::Request make_request(hs::Method method, const std::string& path) {
    hs::Request req;
    req.method = method;
    req.path = path;
    req.target = path;
    return req;
}

hs::Handler says(const std::string& body) {
    return [body](const hs::Request&) { return hs::Response::text(200, body); };
}

const std::string* header(const hs::Response& r, const std::string& name) {
    for (const auto& h : r.headers)
        if (h.first == name) return &h.second;
    return nullptr;
}

// The throw tests make the router log an error on purpose; send stderr to
// /dev/null meanwhile so the test output stays clean.
class SilenceStderr {
public:
    SilenceStderr() {
        saved_ = dup(STDERR_FILENO);
        int null_fd = open("/dev/null", O_WRONLY);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
    ~SilenceStderr() {
        dup2(saved_, STDERR_FILENO);
        close(saved_);
    }

private:
    int saved_;
};

}  // namespace

TEST(router_literal_match) {
    hs::Router router;
    router.add(hs::Method::GET, "/api/health", says("ok"));
    auto req = make_request(hs::Method::GET, "/api/health");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string("ok"));
    CHECK(req.params.empty());
}

TEST(router_param_match) {
    hs::Router router;
    router.add(hs::Method::GET, "/api/users/:id",
               [](const hs::Request& r) { return hs::Response::text(200, r.params.at("id")); });
    auto req = make_request(hs::Method::GET, "/api/users/42");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string("42"));
    CHECK_EQ(req.params["id"], std::string("42"));
}

TEST(router_multiple_params) {
    hs::Router router;
    router.add(hs::Method::GET, "/a/:x/b/:y", says("two"));
    auto req = make_request(hs::Method::GET, "/a/1/b/2");
    CHECK_EQ(router.dispatch(req).status, 200);
    CHECK_EQ(req.params["x"], std::string("1"));
    CHECK_EQ(req.params["y"], std::string("2"));
}

TEST(router_literal_beats_param) {
    hs::Router router;
    // Registered param first on purpose: order must not decide the winner.
    router.add(hs::Method::GET, "/api/users/:id", says("param"));
    router.add(hs::Method::GET, "/api/users/me", says("literal"));
    auto me = make_request(hs::Method::GET, "/api/users/me");
    CHECK_EQ(router.dispatch(me).body, std::string("literal"));
    CHECK(me.params.empty());
    auto other = make_request(hs::Method::GET, "/api/users/7");
    CHECK_EQ(router.dispatch(other).body, std::string("param"));
}

TEST(router_literal_beats_param_leftmost_segment) {
    hs::Router router;
    router.add(hs::Method::GET, "/:a/x", says("param-first"));
    router.add(hs::Method::GET, "/k/:b", says("literal-first"));
    auto req = make_request(hs::Method::GET, "/k/x");
    CHECK_EQ(router.dispatch(req).body, std::string("literal-first"));
}

TEST(router_trailing_slash_equal) {
    hs::Router router;
    router.add(hs::Method::GET, "/a", says("a"));
    auto with_slash = make_request(hs::Method::GET, "/a/");
    CHECK_EQ(router.dispatch(with_slash).status, 200);
    router.add(hs::Method::GET, "/b/", says("b"));
    auto without = make_request(hs::Method::GET, "/b");
    CHECK_EQ(router.dispatch(without).status, 200);
    auto doubled = make_request(hs::Method::GET, "//a//");
    CHECK_EQ(router.dispatch(doubled).status, 200);
}

TEST(router_root_path) {
    hs::Router router;
    router.add(hs::Method::GET, "/", says("root"));
    auto req = make_request(hs::Method::GET, "/");
    CHECK_EQ(router.dispatch(req).body, std::string("root"));
}

TEST(router_param_needs_nonempty_segment) {
    hs::Router router;
    router.add(hs::Method::GET, "/u/:id", says("x"));
    auto req = make_request(hs::Method::GET, "/u/");
    CHECK_EQ(router.dispatch(req).status, 404);
}

TEST(router_not_found_404) {
    hs::Router router;
    router.add(hs::Method::GET, "/a", says("a"));
    auto req = make_request(hs::Method::GET, "/nope");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 404);
    CHECK(header(res, "Allow") == nullptr);
    auto longer = make_request(hs::Method::GET, "/a/b");
    CHECK_EQ(router.dispatch(longer).status, 404);
}

TEST(router_method_not_allowed) {
    hs::Router router;
    router.add(hs::Method::GET, "/a", says("a"));
    auto req = make_request(hs::Method::POST, "/a");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 405);
    const std::string* allow = header(res, "Allow");
    CHECK(allow != nullptr);
    if (allow) CHECK_EQ(*allow, std::string("GET, HEAD"));
}

TEST(router_allow_header_fixed_order) {
    hs::Router router;
    // Registered out of order; Allow must still follow the fixed order.
    router.add(hs::Method::OPTIONS, "/r", says("o"));
    router.add(hs::Method::DELETE_, "/r", says("d"));
    router.add(hs::Method::POST, "/r", says("p"));
    router.add(hs::Method::GET, "/r", says("g"));
    router.add(hs::Method::PATCH, "/r", says("pa"));
    auto req = make_request(hs::Method::PUT, "/r");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 405);
    const std::string* allow = header(res, "Allow");
    CHECK(allow != nullptr);
    if (allow) CHECK_EQ(*allow, std::string("GET, HEAD, POST, DELETE, PATCH, OPTIONS"));
}

TEST(router_allow_collects_across_matching_patterns) {
    hs::Router router;
    router.add(hs::Method::GET, "/u/me", says("me"));
    router.add(hs::Method::DELETE_, "/u/:id", says("del"));
    auto req = make_request(hs::Method::POST, "/u/me");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 405);
    const std::string* allow = header(res, "Allow");
    CHECK(allow != nullptr);
    if (allow) CHECK_EQ(*allow, std::string("GET, HEAD, DELETE"));
    // DELETE on /u/me falls through the literal route to the param route.
    auto del = make_request(hs::Method::DELETE_, "/u/me");
    CHECK_EQ(router.dispatch(del).body, std::string("del"));
    CHECK_EQ(del.params["id"], std::string("me"));
}

TEST(router_head_uses_get) {
    hs::Router router;
    router.add(hs::Method::GET, "/a", says("body"));
    auto req = make_request(hs::Method::HEAD, "/a");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string("body"));  // the server drops the body when serializing
}

TEST(router_head_without_get_is_405) {
    hs::Router router;
    router.add(hs::Method::POST, "/a", says("p"));
    auto req = make_request(hs::Method::HEAD, "/a");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 405);
    const std::string* allow = header(res, "Allow");
    CHECK(allow != nullptr);
    if (allow) CHECK_EQ(*allow, std::string("POST"));
}

TEST(router_unknown_method_501) {
    hs::Router router;
    router.add(hs::Method::GET, "/a", says("a"));
    auto req = make_request(hs::Method::UNKNOWN, "/a");
    CHECK_EQ(router.dispatch(req).status, 501);
    auto elsewhere = make_request(hs::Method::UNKNOWN, "/nope");
    CHECK_EQ(router.dispatch(elsewhere).status, 501);
}

TEST(router_fallback_for_get_only) {
    hs::Router router;
    router.add(hs::Method::GET, "/api/x", says("x"));
    router.set_fallback(says("static"));
    auto get = make_request(hs::Method::GET, "/index.html");
    CHECK_EQ(router.dispatch(get).body, std::string("static"));
    auto head = make_request(hs::Method::HEAD, "/index.html");
    CHECK_EQ(router.dispatch(head).body, std::string("static"));
    auto post = make_request(hs::Method::POST, "/index.html");
    CHECK_EQ(router.dispatch(post).status, 404);
    // A path that matches a route is never handed to the fallback.
    auto routed = make_request(hs::Method::POST, "/api/x");
    CHECK_EQ(router.dispatch(routed).status, 405);
}

TEST(router_handler_throw_500) {
    SilenceStderr quiet;
    hs::Router router;
    router.add(hs::Method::GET, "/boom", [](const hs::Request&) -> hs::Response {
        throw std::runtime_error("secret internal detail");
    });
    router.add(hs::Method::GET, "/odd", [](const hs::Request&) -> hs::Response { throw 42; });
    auto req = make_request(hs::Method::GET, "/boom");
    auto res = router.dispatch(req);
    CHECK_EQ(res.status, 500);
    CHECK(res.body.find("secret") == std::string::npos);
    auto odd = make_request(hs::Method::GET, "/odd");
    CHECK_EQ(router.dispatch(odd).status, 500);
}

TEST(router_fallback_throw_500) {
    SilenceStderr quiet;
    hs::Router router;
    router.set_fallback([](const hs::Request&) -> hs::Response { throw std::runtime_error("x"); });
    auto req = make_request(hs::Method::GET, "/anything");
    CHECK_EQ(router.dispatch(req).status, 500);
}

TEST(router_concurrent_dispatch) {
    hs::Router router;
    router.add(hs::Method::GET, "/n/:id",
               [](const hs::Request& r) { return hs::Response::text(200, r.params.at("id")); });
    router.add(hs::Method::GET, "/n/me", says("me"));
    std::vector<std::thread> threads;
    std::vector<int> failures(8, 0);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&router, &failures, t] {
            for (int i = 0; i < 2000; ++i) {
                std::string id = std::to_string(t * 10000 + i);
                auto req = make_request(hs::Method::GET, "/n/" + id);
                auto res = router.dispatch(req);
                if (res.status != 200 || res.body != id) ++failures[static_cast<size_t>(t)];
            }
        });
    }
    for (auto& th : threads) th.join();
    for (int f : failures) CHECK_EQ(f, 0);
}
