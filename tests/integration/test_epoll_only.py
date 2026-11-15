"""Behaviour specific to the edge-triggered epoll server."""

import json
import os
import re
import select
import signal
import socket
import time

from harness import (WWW, S, check, epoll_only, get, limit_fds, need_fds, no_server, read_response,
                     readable_within, rss_bytes, tcp_wmem_max)

with open(os.path.join(WWW, "file16k.bin"), "rb") as _f:
    FILE16K = _f.read()


@epoll_only
@no_server
def test_many_idle_connections(ctx):
    # The contrast to test_pool_only::test_idle_clients_block_pool: idle
    # keep-alive connections cost the epoll server a little memory each, not a
    # thread, so 500 of them do not delay a new client.
    need_fds(1024)  # raised before the server starts, so it inherits the limit too
    srv = ctx.start("--threads", "2")
    idle = []
    try:
        for _ in range(500):
            idle.append(srv.connect())
        start = time.monotonic()
        with srv.connect() as s:
            s.sendall(get("/health"))
            r = read_response(s, timeout=S(1))
        elapsed = time.monotonic() - start
        check(r.status == 200, r)
        check(elapsed <= S(1), "/health took %.2fs with 500 idle connections" % elapsed)

        # The idle connections were really accepted (not just queued in the
        # kernel backlog) and are still open.
        deadline = time.monotonic() + S(2)
        while True:
            doc = json.loads(srv.request(get("/api/stats")).body)
            if doc["active_connections"] >= 500 or time.monotonic() > deadline:
                break
            time.sleep(0.05)
        check(doc["active_connections"] >= 500, "only %d connections active" % doc["active_connections"])
        readable, _, _ = select.select(idle, [], [], 0)
        check(not readable, "%d idle connections were closed early" % len(readable))
    finally:
        for s in idle:
            s.close()


@epoll_only
@no_server
def test_queued_connection_accepted_after_descriptors_free_up(ctx):
    # The listening socket is edge-triggered. A client that connects while
    # accept() fails with EMFILE raises its edge then, and that edge is used up
    # by the failing accept. When descriptors are freed later, nothing new
    # arrives on the listening socket, so only the accept retry timer can pick
    # the client up. (test_resources::test_fd_exhaustion_recovers checks
    # recovery with a new connection, whose own edge would hide a missing
    # retry.)
    need_fds(512)
    srv = ctx.start("--threads", "1", preexec=limit_fds(64))
    extra = []
    waiter = None
    try:
        for _ in range(100):
            try:
                extra.append(srv.connect(timeout=S(1)))
            except OSError:
                pass
        time.sleep(S(0.2))  # let the server hit EMFILE
        check("Too many open files" in srv.stderr(), "the server never ran out of descriptors")

        waiter = srv.connect()
        waiter.sendall(get("/health"))
        # Long enough for the waiter's edge to be consumed by a failing accept.
        time.sleep(S(0.3))
        check(not readable_within(waiter, 0), "the waiting client was answered while out of descriptors")

        # Free descriptors without opening any new connection: the server
        # sees EOF on the connections it accepted and closes them.
        start = time.monotonic()
        for s in extra:
            s.close()
        extra = []
        r = read_response(waiter, timeout=S(1))
        elapsed = time.monotonic() - start
        check(r.status == 200, r)
        check(elapsed <= S(1), "queued client answered after %.2fs" % elapsed)
    finally:
        for s in extra:
            s.close()
        if waiter is not None:
            waiter.close()


def _read_responses_slowly(sock, count, piece):
    """Reads `count` pipelined responses `piece` bytes at a time and returns
    their (status, body) pairs. The stream is parsed by hand so every byte,
    including the boundaries between responses, is checked."""
    buf = bytearray()
    results = []
    reads = 0
    sock.settimeout(S(5))
    while len(results) < count:
        while True:
            end = buf.find(b"\r\n\r\n")
            if end < 0:
                break
            head = bytes(buf[:end]).decode("latin-1")
            m = re.search(r"\r\ncontent-length:\s*(\d+)", head, re.IGNORECASE)
            check(m is not None, "response without Content-Length: %r" % head)
            length = int(m.group(1))
            if len(buf) < end + 4 + length:
                break
            status = int(head.split(" ", 2)[1])
            results.append((status, bytes(buf[end + 4:end + 4 + length])))
            del buf[:end + 4 + length]
        if len(results) >= count:
            break
        chunk = sock.recv(piece)
        check(chunk, "connection closed after %d of %d responses" % (len(results), count))
        buf += chunk
        reads += 1
        if reads % 64 == 0:
            time.sleep(0.002)  # stay slower than the server, so its writes keep stalling
    check(not buf, "%d unexpected bytes after the last response" % len(buf))
    return results


@epoll_only
def test_large_response_slow_reader(srv):
    # Pipelined 16 KiB responses, read 1 KiB at a time through a small receive
    # buffer. There are more of them than the server's socket send buffer can
    # ever hold (it autotunes up to tcp_wmem's maximum), so its writes come up
    # short and must be resumed on EPOLLOUT. The padded requests (about 400
    # bytes each) need many 16 KiB reads, and the 1 MiB output high-water mark
    # stops reading while some of them are still in the kernel buffer: with
    # edge-triggered epoll no new EPOLLIN arrives for those bytes, so the
    # server has to resume reading by itself once its output drains.
    count = max(200, (tcp_wmem_max() + 2 * 1024 * 1024) // 16384)
    request = get("/file16k.bin", headers={"X-Pad": "p" * 360})
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        s.settimeout(S(5))
        s.connect(("127.0.0.1", srv.port))
        s.sendall(request * count)
        results = _read_responses_slowly(s, count, 1024)
    finally:
        s.close()
    for i, (status, body) in enumerate(results):
        check(status == 200, "response %d has status %d" % (i, status))
        check(body == FILE16K, "response %d body differs from file16k.bin" % i)


@epoll_only
def test_pipeline_flood_bounded(srv):
    # A client that pipelines requests for large responses and never reads.
    # Unanswered requests must stay in the kernel's buffers (TCP flow control
    # then stops the client), not in the server's memory.
    total = 20000
    burst = get("/file16k.bin") * 500  # about 20 KB; sent 40 times
    rss_before = rss_bytes(srv.proc.pid)
    flood = srv.connect()
    try:
        flood.setblocking(False)
        sent_requests = 0
        view = memoryview(b"")
        end = time.monotonic() + 2.0
        while time.monotonic() < end:
            if not view and sent_requests < total:
                view = memoryview(burst)
                sent_requests += 500
            if not view:
                time.sleep(0.05)  # everything sent; keep the window open for the full 2 s
                continue
            try:
                view = view[flood.send(view):]
            except BlockingIOError:
                select.select([], [flood], [], 0.05)

        rss_after = rss_bytes(srv.proc.pid)
        print("    flood: %d requests handed to the kernel, RSS %.1f -> %.1f MiB"
              % (sent_requests - len(view) // len(get("/file16k.bin")),
                 rss_before / 2**20, rss_after / 2**20))
        # The server did start answering (so the test is not passing because
        # it never read anything) ...
        check(readable_within(flood, S(1)), "no response arrived on the flooding connection")
        # ... but kept at most about one high-water mark of output plus one
        # read of input for it.
        check(rss_after < 64 * 2**20, "server RSS %.1f MiB during the flood" % (rss_after / 2**20))

        start = time.monotonic()
        with srv.connect() as other:
            other.sendall(get("/health"))
            r = read_response(other, timeout=S(1))
        check(r.status == 200, r)
        elapsed = time.monotonic() - start
        check(elapsed <= S(1), "/health took %.2fs during the flood" % elapsed)
    finally:
        flood.close()


@epoll_only
@no_server
def test_connections_spread_over_threads(ctx):
    # Informational only: SO_REUSEPORT spreads connections by a hash of the
    # client address and port, so the split varies from run to run. Each event
    # loop logs how many connections it accepted when it exits.
    srv = ctx.start("--threads", "2", "--log-level", "debug")
    for _ in range(40):
        srv.request(get("/health", headers={"Connection": "close"}))
    srv.stop(signal.SIGTERM)
    counts = re.findall(r"event loop (\d+) exiting after accepting (\d+) connections", srv.stderr())
    if counts:
        print("    accepted per event loop: " + ", ".join("loop %s: %s" % c for c in counts))
    else:
        print("    per-loop counts not found in the server log")

