"""Request, keep-alive and idle deadlines.

Lower bounds are not scaled by HS_TEST_SLOWDOWN (closing early is always a
bug); upper bounds are.
"""

import select
import socket
import time

from harness import S, check, flags, get, read_response, wait_eof


@flags("--keepalive-timeout", "0.5")
def test_idle_keepalive_timeout(srv):
    with srv.connect() as s:
        s.sendall(get("/health"))
        r = read_response(s)
        check(r.status == 200, r)
        elapsed, data = wait_eof(s, S(3))
        check(data == b"", "expected a silent close, got %r" % data)
        check(0.4 <= elapsed <= S(1.5), "closed after %.2fs, expected 0.4..%.1f" % (elapsed, S(1.5)))


@flags("--request-timeout", "0.5")
def test_request_timeout_408(srv):
    with srv.connect() as s:
        s.sendall(b"GET / HT")
        start = time.monotonic()
        r = read_response(s, timeout=S(3))
        elapsed = time.monotonic() - start
        check(r.status == 408, r)
        check(r.headers.get("connection") == "close", r.headers)
        check(0.4 <= elapsed <= S(1.5), "408 after %.2fs, expected 0.4..%.1f" % (elapsed, S(1.5)))
        wait_eof(s, S(2))


def _trickle(s, chunks, interval, limit):
    """Sends one chunk every `interval` seconds until the server writes
    something or closes. Returns (seconds until the server spoke, bytes or b"")."""
    start = time.monotonic()
    i = 0
    while time.monotonic() - start < limit:
        if chunks is not None:
            try:
                s.sendall(chunks[i % len(chunks)])
            except (BrokenPipeError, ConnectionResetError):
                pass  # the server already closed; the read below sees it
            i += 1
        r, _, _ = select.select([s], [], [], interval)
        if r:
            return time.monotonic() - start, None
    raise AssertionError("server stayed silent for %.1fs" % limit)


@flags("--request-timeout", "0.5")
def test_slow_headers_still_timeout(srv):
    head = b"GET /health HTTP/1.1\r\nHost: localhost\r\nX-Slow: " + b"a" * 1000
    with srv.connect() as s:
        chunks = [head[i:i + 1] for i in range(len(head))]
        elapsed, _ = _trickle(s, chunks, S(0.1), S(3))
        r = read_response(s)
        check(r.status == 408, r)
        # A deadline refreshed on every byte would never fire at all.
        check(elapsed <= S(1.5), "408 after %.2fs, expected by %.1f" % (elapsed, S(1.5)))


@flags("--keepalive-timeout", "0.5")
def test_fresh_connection_idle_closes(srv):
    with srv.connect() as s:
        elapsed, data = wait_eof(s, S(3))
        check(data == b"", "expected a silent close (no 408), got %r" % data)
        check(0.4 <= elapsed <= S(1.5), "closed after %.2fs" % elapsed)


@flags("--keepalive-timeout", "0.5")
def test_crlf_trickle_still_times_out(srv):
    # Blank lines before a request are allowed and do not start one, so they
    # must not keep an idle connection alive either.
    with srv.connect() as s:
        elapsed, _ = _trickle(s, [b"\r\n"], S(0.1), S(3))
        _, data = wait_eof(s, S(1))
        check(data == b"", "expected a silent close (no 408), got %r" % data)
        check(elapsed <= S(1.5), "closed after %.2fs, expected by %.1f" % (elapsed, S(1.5)))


@flags("--threads", "1", "--keepalive-timeout", "0.5")
def test_prompt_request_not_dropped_by_idle_timeout(srv):
    # A client that sent its request right after connecting is not idle, even
    # if the server only gets to it later (in pool mode: after waiting in the
    # queue behind `holder` for longer than the keep-alive timeout).
    with srv.connect() as holder, srv.connect() as client:
        client.sendall(get("/health"))
        r = read_response(client, timeout=S(3))
        check(r.status == 200, r)
        wait_eof(holder, S(2))


@flags("--write-timeout", "0.5")
def test_write_timeout_closes_stalled_reader(srv):
    # 400 pipelined 16 KiB responses (6.4 MB) are far more than the socket
    # buffers hold, so the server's send stalls while this client reads nothing.
    count = 400
    with srv.connect() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.sendall(get("/file16k.bin") * count)
        time.sleep(S(1.5))  # well past the write timeout
        # Draining the socket buffers takes moments. A server still writing
        # would send all 6.4 MB and then keep the connection open.
        _, data = wait_eof(s, S(3))
        check(len(data) < count * 16384, "all %d bytes arrived; the server never gave up" % len(data))
