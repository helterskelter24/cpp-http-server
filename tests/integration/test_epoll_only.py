"""Behaviour specific to the edge-triggered epoll server."""

import json
import os
import re
import select
import signal
import time

from harness import (WWW, S, check, epoll_only, get, limit_fds, need_fds, no_server, read_response,
                     readable_within, rss_bytes)

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
def test_idle_connections_release_large_output_buffers(ctx):
    # Each client in turn pipelines requests for about 1.6 MB of responses,
    # reads them all and then stays connected without sending anything. The
    # server's output buffer grows to its 1 MiB high-water mark for each of
    # them; an idle connection must give that back, or 64 of them would hold
    # 64 MiB or more. Clients go one at a time so the freed memory can be
    # reused by the next one and RSS stays flat. ASan holds freed memory in a
    # quarantine (to catch use-after-free), which would defeat that reuse, so
    # it is turned off for this server; other builds ignore ASAN_OPTIONS.
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = (env.get("ASAN_OPTIONS", "") + ":quarantine_size_mb=0").lstrip(":")
    srv = ctx.start("--threads", "1", env=env)
    count = 100
    clients = []
    try:
        # One client first, so allocations the server makes only once (and
        # the first buffer) are already in the baseline.
        rss_before = None
        for i in range(65):
            s = srv.connect()
            clients.append(s)
            s.sendall(get("/file16k.bin") * count)
            for _ in range(count):
                r = read_response(s)
                check(r.status == 200 and len(r.body) == len(FILE16K), r)
            if i == 0:
                rss_before = rss_bytes(srv.proc.pid)
        rss_after = rss_bytes(srv.proc.pid)
        growth = rss_after - rss_before
        print("    64 idle connections after large responses: RSS %.1f -> %.1f MiB"
              % (rss_before / 2**20, rss_after / 2**20))
        check(growth < 24 * 2**20, "RSS grew by %.1f MiB for 64 idle connections" % (growth / 2**20))
    finally:
        for s in clients:
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


@epoll_only
@no_server
def test_connections_spread_over_threads(ctx):
    # SO_REUSEPORT spreads connections by a hash of the client address and
    # port, so the split varies from run to run, but every loop must take part.
    # Each event loop logs how many connections it accepted when it exits.
    # With 40 connections and 2 loops, a correct server sends all of them to
    # one loop with probability 2 * 2**-40, so a failure here means a loop is
    # not accepting (or not running at all).
    srv = ctx.start("--threads", "2", "--log-level", "debug")
    for _ in range(40):
        srv.request(get("/health", headers={"Connection": "close"}))
    srv.stop(signal.SIGTERM)
    counts = re.findall(r"event loop (\d+) exiting after accepting (\d+) connections", srv.stderr())
    print("    accepted per event loop: " + ", ".join("loop %s: %s" % c for c in counts))
    per_loop = {int(loop): int(n) for loop, n in counts}
    check(sorted(per_loop) == [0, 1], "expected exit lines from loops 0 and 1, got %r" % counts)
    check(sum(per_loop.values()) == 40, "loops accepted %r, expected 40 in total" % per_loop)
    check(min(per_loop.values()) >= 1, "one loop accepted nothing: %r" % per_loop)

