"""Running out of operating-system resources (both servers)."""

import os
import re
import select
import socket
import time

from harness import (WWW, S, check, cpu_seconds, get, limit_fds, need_fds, no_server, read_response,
                     readable_within, rss_bytes, tcp_wmem_max)

with open(os.path.join(WWW, "file16k.bin"), "rb") as _f:
    FILE16K = _f.read()


@no_server
def test_fd_exhaustion_recovers(ctx):
    # With only 64 descriptors the server runs out after a few dozen
    # connections. It must keep serving the connections it has, must not spin
    # on the listening socket while accept() fails, and must recover once
    # descriptors are free again.
    need_fds(512)
    srv = ctx.start("--threads", "2", preexec=limit_fds(64))
    first = srv.connect()
    extra = []
    try:
        first.sendall(get("/health"))
        check(read_response(first).status == 200, "warm-up request failed")
        for _ in range(100):
            try:
                extra.append(srv.connect(timeout=S(1)))
            except OSError:
                pass  # expected once the backlog misbehaves; not what is tested
        time.sleep(S(0.2))  # let the server hit EMFILE

        first.sendall(get("/health"))
        r = read_response(first, timeout=S(2))
        check(r.status == 200, r)
        check("Too many open files" in srv.stderr(), "the server never ran out of descriptors")

        before = cpu_seconds(srv.proc.pid)
        time.sleep(1.0)
        used = cpu_seconds(srv.proc.pid) - before
        check(used < 0.5, "server used %.2fs of CPU in 1s while out of descriptors" % used)
    finally:
        first.close()
        for s in extra:
            s.close()

    deadline = time.monotonic() + S(2)
    last_error = None
    while time.monotonic() < deadline:
        try:
            with srv.connect(timeout=S(1)) as s:
                s.sendall(get("/health"))
                r = read_response(s, timeout=max(0.1, deadline - time.monotonic()))
                if r.status == 200:
                    return
                last_error = r
        except Exception as e:  # reset or timeout while the server catches up
            last_error = e
        time.sleep(0.05)
    check(False, "no 200 within %.1fs after descriptors were freed: %r" % (S(2), last_error))


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


def test_large_response_slow_reader(srv):
    # Pipelined 16 KiB responses, read 1 KiB at a time through a small receive
    # buffer. There are more of them than the server's socket send buffer can
    # ever hold (it autotunes up to tcp_wmem's maximum), so its writes come up
    # short and must be resumed: in pool mode by a blocking send that waits,
    # in epoll mode on EPOLLOUT. The padded requests (about 400 bytes each)
    # need many 16 KiB reads, and the 1 MiB output high-water mark stops
    # reading while some of them are still in the kernel buffer. With
    # edge-triggered epoll no new EPOLLIN arrives for those bytes, so the
    # epoll server has to resume reading by itself once its output drains.
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
