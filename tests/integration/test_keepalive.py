"""Persistent connections, pipelining and the rules for closing."""

import socket
import time

from harness import S, check, flags, get, read_response, req, wait_eof


def test_two_requests_one_socket(srv):
    with srv.connect() as s:
        for path in ("/health", "/api/users/7"):
            s.sendall(get(path))
            r = read_response(s)
            check(r.status == 200, r)
            check(r.headers.get("connection") == "keep-alive", r.headers)


def test_pipelined_three(srv):
    with srv.connect() as s:
        s.sendall(get("/api/users/1") + get("/api/users/2") + get("/api/users/3"))
        for i in (1, 2, 3):
            r = read_response(s)
            check(r.status == 200, r)
            check(b'"id":"%d"' % i in r.body, "response %d out of order: %r" % (i, r.body))


def test_connection_close_honoured(srv):
    with srv.connect() as s:
        s.sendall(get("/health", headers={"Connection": "close"}))
        r = read_response(s)
        check(r.status == 200, r)
        check(r.headers.get("connection") == "close", r.headers)
        _, extra = wait_eof(s, S(2))
        check(extra == b"", extra)


def test_http10_closes_by_default(srv):
    with srv.connect() as s:
        s.sendall(get("/health", version="HTTP/1.0"))
        r = read_response(s)
        check(r.status == 200, r)
        check(r.headers.get("connection") == "close", r.headers)
        wait_eof(s, S(2))


@flags("--max-keepalive-requests", "2")
def test_max_keepalive_requests(srv):
    with srv.connect() as s:
        s.sendall(get("/health"))
        r1 = read_response(s)
        check(r1.headers.get("connection") == "keep-alive", r1.headers)
        s.sendall(get("/health"))
        r2 = read_response(s)
        check(r2.status == 200, r2)
        check(r2.headers.get("connection") == "close", r2.headers)
        wait_eof(s, S(2))


def test_half_close_still_answered(srv):
    with srv.connect() as s:
        s.sendall(get("/file16k.bin"))
        s.shutdown(socket.SHUT_WR)
        r = read_response(s)
        check(r.status == 200 and len(r.body) == 16384, r)
        wait_eof(s, S(2))


def test_request_split_slowly(srv):
    data = req("POST", "/api/echo", body=b"split-body")
    with srv.connect() as s:
        for i in range(0, len(data), 5):
            s.sendall(data[i:i + 5])
            time.sleep(0.01)
        r = read_response(s)
        check(r.status == 200, r)
        check(b'"body":"split-body"' in r.body, r.body)
