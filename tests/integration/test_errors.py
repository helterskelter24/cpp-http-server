"""Error statuses produced by the parser, the router and the static file handler."""

import time

from harness import S, check, flags, get, read_response, req, tcp_wmem_max, wait_eof


def expect_error_and_close(srv, raw, status):
    with srv.connect() as s:
        s.sendall(raw)
        r = read_response(s)
        check(r.status == status, "expected %d, got %r" % (status, r))
        check(r.headers.get("connection") == "close", r.headers)
        wait_eof(s, S(2))
        return r


def test_not_found_404(srv):
    r = srv.request(get("/no/such/thing"))
    check(r.status == 404, r)


def test_method_not_allowed_405(srv):
    r = srv.request(req("POST", "/health", body=b"x"))
    check(r.status == 405, r)
    check("GET" in r.headers.get("allow", ""), r.headers)


def test_bad_request_400(srv):
    # HTTP/1.1 without Host is a protocol error; the connection must close.
    expect_error_and_close(srv, b"GET /health HTTP/1.1\r\n\r\n", 400)


def test_unknown_method_501(srv):
    r = srv.request(req("BREW", "/health"))
    check(r.status == 501, r)


def test_version_505(srv):
    expect_error_and_close(srv, b"GET /health HTTP/2.0\r\nHost: x\r\n\r\n", 505)


def test_chunked_501(srv):
    raw = req("POST", "/api/echo", headers={"Transfer-Encoding": "chunked"})
    expect_error_and_close(srv, raw, 501)


@flags("--max-header-bytes", "256")
def test_header_too_large_431(srv):
    expect_error_and_close(srv, get("/health", headers={"X-Big": "a" * 400}), 431)


@flags("--max-body-bytes", "100")
def test_body_too_large_413(srv):
    # Only the head is sent: the declared length alone is enough to refuse it,
    # and leaving no unread body means the close is a clean FIN, not a reset.
    expect_error_and_close(srv, req("POST", "/api/echo", headers={"Content-Length": "200"}), 413)


def test_traversal_blocked(srv):
    for path in ("/../CMakeLists.txt", "/%2e%2e/CMakeLists.txt"):
        r = srv.request(get(path))
        check(r.status == 404, "%s gave %r" % (path, r))
        check(b"cmake_minimum_required" not in r.body, r.body)


@flags("--threads", "1")
def test_client_disconnect_mid_request(srv):
    # One thread, so the follow-up request is served only if the server let go
    # of the abandoned connection (with two, a pool worker could stay stuck on
    # it and the other worker would still answer).
    s = srv.connect()
    s.sendall(b"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Len")
    time.sleep(0.05)
    s.close()
    time.sleep(0.05)
    r = srv.request(get("/health"))
    check(r.status == 200, r)



def test_client_gone_while_server_writes(srv):
    # The client pipelines requests for several megabytes of responses and
    # closes at once without reading. The server is still writing when the
    # client's kernel answers with a reset, so a later send() fails with EPIPE.
    # That must cost only this connection: without MSG_NOSIGNAL (and SIGPIPE
    # ignored) the default SIGPIPE action would kill the whole process.
    count = max(400, (tcp_wmem_max() + 2 * 1024 * 1024) // 16384)
    for _ in range(5):
        s = srv.connect()
        s.sendall(get("/file16k.bin") * count)
        s.close()
    time.sleep(S(0.3))
    r = srv.request(get("/health"))
    check(r.status == 200, r)
