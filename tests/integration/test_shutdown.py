"""Graceful shutdown on SIGTERM/SIGINT."""

import signal
import socket
import time

from harness import S, check, flags, get, read_response, wait_eof

POST_HEAD = b"POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n"


def test_sigterm_exits_zero(srv):
    start = time.monotonic()
    code = srv.stop(signal.SIGTERM, timeout=S(2))
    check(code == 0, "exit code %r" % code)
    check(time.monotonic() - start <= S(2), "slow exit")


def test_sigint_exits_zero(srv):
    code = srv.stop(signal.SIGINT, timeout=S(2))
    check(code == 0, "exit code %r" % code)


def test_inflight_request_completes(srv):
    with srv.connect() as s:
        s.sendall(POST_HEAD)
        time.sleep(S(0.1))  # let the server read the head, so the request is in flight
        srv.send_signal(signal.SIGTERM)
        time.sleep(S(0.2))
        s.sendall(b"hello")
        r = read_response(s)
        check(r.status == 200, r)
        check(b'"body":"hello"' in r.body, r.body)
        check(r.headers.get("connection") == "close", r.headers)
        wait_eof(s, S(2))
    code = srv.wait(S(3))
    check(code == 0, "exit code %r" % code)


def test_idle_keepalive_closed_on_shutdown(srv):
    with srv.connect() as s:
        s.sendall(get("/health"))
        check(read_response(s).status == 200, "health failed")
        srv.send_signal(signal.SIGTERM)
        elapsed, data = wait_eof(s, S(1))
        check(data == b"", data)
    code = srv.wait(S(3))
    check(code == 0, "exit code %r" % code)


def test_no_new_connections_after_sigterm(srv):
    with srv.connect() as s:
        s.sendall(POST_HEAD)
        time.sleep(S(0.1))
        srv.send_signal(signal.SIGTERM)
        time.sleep(S(0.2))  # the listening socket is closed by now

        # Either the connect is refused, or (if it raced into a backlog that
        # was then discarded) the socket is reset or closed without an answer.
        try:
            with socket.create_connection(("127.0.0.1", srv.port), timeout=S(2)) as late:
                try:
                    late.sendall(get("/health"))
                    _, data = wait_eof(late, S(2))
                    check(data == b"", "a connection made after SIGTERM was served: %r" % data)
                except (ConnectionResetError, BrokenPipeError):
                    pass
        except ConnectionRefusedError:
            pass

        s.sendall(b"hello")
        r = read_response(s)
        check(r.status == 200, r)
    code = srv.wait(S(3))
    check(code == 0, "exit code %r" % code)


@flags("--shutdown-grace", "0.5")
def test_grace_deadline_forces_exit(srv):
    with srv.connect() as s:
        s.sendall(POST_HEAD)  # the body never comes
        time.sleep(S(0.1))
        start = time.monotonic()
        srv.send_signal(signal.SIGTERM)
        code = srv.wait(S(2))
        check(code == 0, "exit code %r" % code)
        check(time.monotonic() - start >= 0.4, "exited before the grace period")
