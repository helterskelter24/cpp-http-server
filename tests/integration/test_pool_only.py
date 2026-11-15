"""Behaviour specific to the blocking thread-pool server."""

import signal
import time

from harness import S, check, flags, get, pool_only, read_response, readable_within, wait_eof


@pool_only
@flags("--threads", "1", "--queue-capacity", "1")
def test_queue_full_503(srv):
    busy = srv.connect()  # occupies the only worker
    busy.sendall(get("/health"))
    check(read_response(busy).status == 200, "first request failed")
    queued = srv.connect()  # fills the one queue slot
    time.sleep(S(0.1))
    try:
        for _ in range(2):
            with srv.connect() as extra:
                # Nothing is sent: the server answers on its own, and with no
                # unread request bytes its close cannot turn into a reset.
                r = read_response(extra)
                check(r.status == 503, r)
                check(r.headers.get("connection") == "close", r.headers)
                wait_eof(extra, S(2))
    finally:
        busy.close()
        queued.close()


@pool_only
@flags("--threads", "2")
def test_idle_clients_block_pool(srv):
    # Documents the known weakness of one-connection-per-worker: two idle
    # keep-alive clients are enough to make a third one wait.
    idle = []
    for _ in range(2):
        s = srv.connect()
        s.sendall(get("/health"))
        check(read_response(s).status == 200, "warm-up request failed")
        idle.append(s)
    with srv.connect() as waiting:
        waiting.sendall(get("/health"))
        check(not readable_within(waiting, 0.3), "third client was served while both workers were held")
        idle[0].close()
        r = read_response(waiting, timeout=S(1))
        check(r.status == 200, r)
    idle[1].close()


@pool_only
@flags("--threads", "1")
def test_queued_connection_closed_on_shutdown(srv):
    # The only worker is held by `busy`, so `queued` waits in the pool's queue.
    # At shutdown the queued task must close its socket without serving it,
    # even though a request is already waiting on it.
    busy = srv.connect()
    busy.sendall(get("/health"))
    check(read_response(busy).status == 200, "first request failed")
    with srv.connect() as queued:
        queued.sendall(get("/health"))
        time.sleep(S(0.1))
        srv.send_signal(signal.SIGTERM)
        _, data = wait_eof(queued, S(2))
        check(data == b"", "a connection queued at shutdown was served: %r" % data)
    busy.close()
    code = srv.wait(S(3))
    check(code == 0, "exit code %r" % code)
