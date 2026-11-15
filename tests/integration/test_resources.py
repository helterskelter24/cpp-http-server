"""Running out of operating-system resources (both servers)."""

import time

from harness import S, check, cpu_seconds, get, limit_fds, need_fds, no_server, read_response


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
