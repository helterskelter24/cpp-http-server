"""Helpers for driving the server binary over raw sockets.

Only the standard library is used. Responses are parsed by hand rather than
with http.client so tests can see exactly what went over the wire: pipelined
responses, a missing body after HEAD, or a connection closed without a reply.
"""

import os
import re
import select
import signal
import socket
import subprocess
import threading
import time
import weakref

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WWW = os.path.join(REPO, "www")

# Sanitizer builds run several times slower; HS_TEST_SLOWDOWN stretches every
# upper bound and wait in the suite by this factor. Lower bounds are not scaled:
# a server must never time out *earlier* than it was told to.
SLOWDOWN = float(os.environ.get("HS_TEST_SLOWDOWN", "1"))


def S(seconds):
    """A timing window or wait, scaled by HS_TEST_SLOWDOWN."""
    return seconds * SLOWDOWN


SANITIZER_MARKERS = (
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "runtime error:",
    "WARNING: ThreadSanitizer",
)

LISTEN_RE = re.compile(r"listening on (\S+):(\d+) mode=(\w+) threads=(\d+)")


class TestFailure(AssertionError):
    pass


class Skip(Exception):
    """Raised by a test that cannot run on this host; the message says why."""


def check(cond, message):
    if not cond:
        raise TestFailure(message)


# ---- requests and responses ----


def req(method, path, headers=None, body=b"", version="HTTP/1.1"):
    """Builds raw request bytes. Adds Host (for HTTP/1.1) and Content-Length (when there is a body)."""
    lines = ["%s %s %s" % (method, path, version)]
    headers = dict(headers or {})
    if version == "HTTP/1.1" and "Host" not in headers:
        headers["Host"] = "localhost"
    if body and "Content-Length" not in headers:
        headers["Content-Length"] = str(len(body))
    for name, value in headers.items():
        lines.append("%s: %s" % (name, value))
    if isinstance(body, str):
        body = body.encode()
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + body


def get(path, **kwargs):
    return req("GET", path, **kwargs)


class Response:
    def __init__(self, status, headers, body, raw_head):
        self.status = status
        self.headers = headers  # lower-cased names
        self.body = body
        self.raw_head = raw_head

    def __iter__(self):
        return iter((self.status, self.headers, self.body))

    def __repr__(self):
        return "Response(%d, %r, %r)" % (self.status, self.headers, self.body[:80])


class ConnectionClosed(Exception):
    """The peer closed (or reset) the connection before a full response arrived."""


# Bytes received after the end of one response belong to the next pipelined
# response, so they are kept per socket until the next read_response() call.
_leftover = weakref.WeakKeyDictionary()


def _recv_more(sock, buf):
    try:
        chunk = sock.recv(65536)
    except ConnectionResetError:
        chunk = b""
    if not chunk:
        raise ConnectionClosed("connection closed after %d bytes: %r" % (len(buf), bytes(buf[:200])))
    buf += chunk


def read_response(sock, head=False, timeout=None):
    """Reads exactly one response: status line, headers, then Content-Length body
    bytes (none for a HEAD response). Raises ConnectionClosed on EOF/reset first."""
    sock.settimeout(S(5) if timeout is None else timeout)
    buf = bytearray(_leftover.pop(sock, b""))
    while b"\r\n\r\n" not in buf:
        _recv_more(sock, buf)
    end = buf.index(b"\r\n\r\n")
    head_text = bytes(buf[:end]).decode("latin-1")
    del buf[: end + 4]

    lines = head_text.split("\r\n")
    parts = lines[0].split(" ", 2)
    check(len(parts) >= 2 and parts[0].startswith("HTTP/"), "bad status line %r" % lines[0])
    status = int(parts[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()

    length = 0 if head else int(headers.get("content-length", "0"))
    while len(buf) < length:
        _recv_more(sock, buf)
    body = bytes(buf[:length])
    del buf[:length]
    if buf:
        _leftover[sock] = bytes(buf)
    return Response(status, headers, body, head_text)


def wait_eof(sock, timeout):
    """Waits for the peer to close. Returns (seconds until EOF, bytes received
    before it). A reset counts as EOF. Fails if the socket stays open."""
    start = time.monotonic()
    data = bytearray(_leftover.pop(sock, b""))
    deadline = start + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TestFailure("socket still open after %.2fs (got %r)" % (timeout, bytes(data[:200])))
        sock.settimeout(remaining)
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        except ConnectionResetError:
            chunk = b""
        if not chunk:
            return time.monotonic() - start, bytes(data)
        data += chunk


def readable_within(sock, timeout):
    """True if the socket has data (or EOF) within `timeout` seconds."""
    if _leftover.get(sock):
        return True
    r, _, _ = select.select([sock], [], [], timeout)
    return bool(r)


# ---- the server process ----


def _base_args(binary, mode, port=0, root=WWW):
    return [binary, "--port", str(port), "--bind", "127.0.0.1", "--root", root, "--mode", mode]


def run_binary(args, timeout):
    """Runs the binary to completion. Returns (exit code, stdout, stderr).
    Kills it and fails if it is still running after `timeout` seconds."""
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        raise TestFailure("%s did not exit within %.1fs" % (" ".join(args), timeout))
    return proc.returncode, out.decode(errors="replace"), err.decode(errors="replace")


class ServerProc:
    """One running server. The binary gets `--port 0 --bind 127.0.0.1 --root www
    --mode <mode>` plus `extra`; the port is read from the `listening on` line.

    Never pass a --log-level above info: the `listening on` line is an Info log
    line and the harness waits for it."""

    def __init__(self, binary, mode, extra=(), preexec=None):
        self.binary = binary
        self.mode = mode
        self.args = _base_args(binary, mode) + list(extra)
        self.exit_code = None
        self._stderr_lines = []
        self._stderr_lock = threading.Lock()
        self._listening = threading.Event()
        self.port = None
        self.proc = subprocess.Popen(
            self.args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            preexec_fn=preexec,
        )
        # From here on any exception, including the runner's hard time limit
        # firing during the wait below, must kill the child: the caller never
        # receives this object, so nobody else could clean it up.
        try:
            # stderr is drained continuously so the server never blocks on a full
            # pipe, and so sanitizer reports can be checked after it exits.
            self._reader = threading.Thread(target=self._read_stderr, daemon=True)
            self._reader.start()
            self._listening.wait(S(5))
        except BaseException:
            self.kill()
            raise
        if self.port is None:
            self.kill()
            raise TestFailure("no 'listening on' line (exit code %r; None means still running after %.1fs); "
                              "stderr:\n%s" % (self.exit_code, S(5), self.stderr()))

    def _read_stderr(self):
        for raw in self.proc.stderr:
            line = raw.decode(errors="replace")
            with self._stderr_lock:
                self._stderr_lines.append(line)
            if self.port is None:
                m = LISTEN_RE.search(line)
                if m:
                    self.port = int(m.group(2))
                    self._listening.set()
        # stderr closed: the process exited. Wake the constructor so a server
        # that dies during start-up fails the test at once, not after 5 s.
        self._listening.set()

    def stderr(self):
        with self._stderr_lock:
            return "".join(self._stderr_lines)

    def running(self):
        return self.proc.poll() is None

    def connect(self, timeout=None):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=S(5) if timeout is None else timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return s

    def request(self, raw, head=False):
        """Sends raw request bytes on a new connection and returns the first response."""
        with self.connect() as s:
            s.sendall(raw)
            return read_response(s, head=head)

    def send_signal(self, sig=signal.SIGTERM):
        self.proc.send_signal(sig)

    def wait(self, timeout):
        """Waits for exit; returns the exit code. Fails (and kills) on timeout."""
        try:
            self.exit_code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            raise TestFailure("server did not exit within %.1fs" % timeout)
        self._reader.join(S(5))
        return self.exit_code

    def stop(self, sig=signal.SIGTERM, timeout=None):
        """Sends `sig` and returns the exit code."""
        if self.running():
            self.send_signal(sig)
        return self.wait(S(10) if timeout is None else timeout)

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.exit_code = self.proc.returncode
        reader = getattr(self, "_reader", None)
        if reader is not None and reader.is_alive():
            reader.join(S(5))

    def sanitizer_report(self):
        """The first stderr line that looks like a sanitizer finding, or None."""
        for line in self.stderr().splitlines():
            if any(marker in line for marker in SANITIZER_MARKERS):
                return line
        return None


class Context:
    """What a test gets when it manages servers itself (see run_all.no_server)."""

    def __init__(self, binary, mode):
        self.binary = binary
        self.mode = mode
        self.servers = []

    def start(self, *extra, preexec=None):
        srv = ServerProc(self.binary, self.mode, extra, preexec)
        self.servers.append(srv)
        return srv

    def base_args(self, port=0, root=WWW):
        return _base_args(self.binary, self.mode, port, root)


# ---- test markers read by run_all.py ----


def flags(*extra):
    """Starts this test's server with extra command-line flags."""
    def mark(fn):
        fn.server_flags = list(extra)
        return fn
    return mark


def pool_only(fn):
    fn.only_mode = "pool"
    return fn


def epoll_only(fn):
    fn.only_mode = "epoll"
    return fn


def shared(fn):
    """The test can run on a server shared with the other shared tests of its
    module (it needs no flags and leaves no state that matters to others)."""
    fn.shared = True
    return fn


def no_server(fn):
    """The test gets a Context and starts (or runs) servers itself."""
    fn.no_server = True
    return fn


def time_limit(seconds):
    """Overrides the default hard time limit (before HS_TEST_SLOWDOWN scaling)."""
    def mark(fn):
        fn.time_limit = seconds
        return fn
    return mark
