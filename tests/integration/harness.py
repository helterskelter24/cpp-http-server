"""Helpers for driving the server binary over raw sockets.

Only the standard library is used. Responses are parsed by hand rather than
with http.client so tests can see exactly what went over the wire: pipelined
responses, a missing body after HEAD, or a connection closed without a reply.
"""

import os
import re
import resource
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

def sanitizer_line(text):
    """The first line of `text` that looks like a sanitizer finding, or None."""
    for line in text.splitlines():
        if any(marker in line for marker in SANITIZER_MARKERS):
            return line
    return None


def check_no_sanitizer_report(what, stderr):
    """Fails the test if a child process (server or loadgen) reported a
    sanitizer finding on stderr; its exit code alone could hide one, because
    a test may expect a non-zero exit."""
    line = sanitizer_line(stderr)
    check(line is None, "sanitizer report in %s stderr: %s" % (what, line and line.strip()))


LISTEN_RE = re.compile(r"listening on (\S+):(\d+) mode=(\w+) threads=(\d+)")


# Path of the loadgen binary; run_all.py sets it from --loadgen.
LOADGEN = None


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


def loadgen_binary():
    """The loadgen binary for tests that drive it; skips the test when run_all.py
    was not given --loadgen."""
    if LOADGEN is None:
        raise Skip("loadgen binary not given (pass --loadgen PATH)")
    return LOADGEN


# ---- host resources ----


def need_fds(count):
    """Raises this process's soft RLIMIT_NOFILE to at least `count` (servers
    started afterwards inherit it). Skips the test if the hard limit is lower."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft != resource.RLIM_INFINITY and soft < count:
        if hard != resource.RLIM_INFINITY and hard < count:
            raise Skip("needs %d file descriptors, hard limit is %d" % (count, hard))
        resource.setrlimit(resource.RLIMIT_NOFILE, (count, hard))


def limit_fds(count):
    """A preexec hook for ServerProc: the server may hold at most `count` descriptors."""
    def hook():
        _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (count, hard))
    return hook


def limit_address_space(nbytes):
    """A preexec hook: the process may map at most `nbytes` of virtual memory."""
    def hook():
        resource.setrlimit(resource.RLIMIT_AS, (nbytes, nbytes))
    return hook


def is_sanitizer_build(binary):
    """True if the binary carries the ASan or TSan runtime. Those reserve
    terabytes of address space at start-up, so they cannot run under an
    RLIMIT_AS cap at all."""
    with open(binary, "rb") as f:
        data = f.read()
    return b"__asan_init" in data or b"__tsan_init" in data


def tcp_wmem_max():
    """The most a socket's send buffer can grow to by autotuning. Tests that
    need a server's writes to stall must send more than this."""
    try:
        with open("/proc/sys/net/ipv4/tcp_wmem") as f:
            return int(f.read().split()[2])
    except (OSError, ValueError, IndexError):
        return 4 * 1024 * 1024


def rss_bytes(pid):
    """Resident set size of a process, from /proc/<pid>/status."""
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    raise TestFailure("no VmRSS line for pid %d" % pid)


def cpu_seconds(pid):
    """User plus system CPU time of a process, from /proc/<pid>/stat."""
    with open("/proc/%d/stat" % pid) as f:
        # The command name (field 2) may contain spaces, so split after its ')'.
        fields = f.read().rpartition(")")[2].split()
    # utime and stime are fields 14 and 15 of the whole line.
    ticks = int(fields[11]) + int(fields[12])
    return ticks / os.sysconf("SC_CLK_TCK")


# ---- the server process ----


def _base_args(binary, mode, port=0, root=WWW):
    return [binary, "--port", str(port), "--bind", "127.0.0.1", "--root", root, "--mode", mode]


def run_binary(args, timeout, preexec=None):
    """Runs the binary to completion. Returns (exit code, stdout, stderr).
    Kills it and fails if it is still running after `timeout` seconds, or if
    it printed a sanitizer report."""
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, preexec_fn=preexec)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        raise TestFailure("%s did not exit within %.1fs" % (" ".join(args), timeout))
    err = err.decode(errors="replace")
    check_no_sanitizer_report(os.path.basename(args[0]), err)
    return proc.returncode, out.decode(errors="replace"), err


class ServerProc:
    """One running server. The binary gets `--port 0 --bind 127.0.0.1 --root www
    --mode <mode>` plus `extra`; the port is read from the `listening on` line.

    Never pass a --log-level above info: the `listening on` line is an Info log
    line and the harness waits for it."""

    def __init__(self, binary, mode, extra=(), preexec=None, env=None):
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
            env=env,
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
        return sanitizer_line(self.stderr())


class Context:
    """What a test gets when it manages servers itself (see run_all.no_server)."""

    def __init__(self, binary, mode):
        self.binary = binary
        self.mode = mode
        self.servers = []

    def start(self, *extra, preexec=None, env=None):
        srv = ServerProc(self.binary, self.mode, extra, preexec, env)
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
