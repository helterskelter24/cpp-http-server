"""The load generator (bench/loadgen) against a real server.

These tests need the loadgen binary: run_all.py --loadgen PATH. Without it they
are skipped. Runs are kept to about a second because the host may have only two
cores shared by client and server."""

import json
import subprocess
import time

from harness import S, check, get, loadgen_binary, no_server, read_response, run_binary

CSV_FIELDS = ["label", "connections", "idle", "keepalive", "requests", "errors",
              "duration_s", "rps", "p50_ms", "p95_ms", "p99_ms", "max_ms"]


def loadgen_args(srv, *extra, path="/health"):
    return [loadgen_binary(), "--host", "127.0.0.1", "--port", str(srv.port), "--path", path] + list(extra)


def parse_csv(out):
    lines = out.strip().splitlines()
    check(len(lines) == 1, "expected one CSV line, got %r" % out)
    fields = lines[0].split(",")
    check(len(fields) == 12, "expected 12 CSV fields, got %d: %r" % (len(fields), lines[0]))
    return dict(zip(CSV_FIELDS, fields))


def check_percentiles(row):
    p50, p95, p99, mx = (float(row[k]) for k in ("p50_ms", "p95_ms", "p99_ms", "max_ms"))
    check(0 < p50 <= p95 <= p99 <= mx, "percentiles out of order: %r" % row)


@no_server
def test_smoke_keepalive(ctx):
    srv = ctx.start()
    code, out, err = run_binary(loadgen_args(srv, "-c", "4", "-d", "1", "--warmup", "0", "--csv",
                                             "--label", "smoke"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    row = parse_csv(out)
    check(row["label"] == "smoke", row)
    check(row["connections"] == "4" and row["idle"] == "0" and row["keepalive"] == "1", row)
    check(int(row["requests"]) > 0, row)
    check(row["errors"] == "0", row)
    check_percentiles(row)
    # Throughput is successful responses over the requested duration, to the
    # precision the CSV prints.
    check(abs(float(row["rps"]) - int(row["requests"]) / float(row["duration_s"])) < 0.01, row)
    check(row["duration_s"] == "1.000", row)


@no_server
def test_smoke_no_keepalive(ctx):
    srv = ctx.start()
    code, out, err = run_binary(loadgen_args(srv, "-c", "4", "-d", "1", "--warmup", "0",
                                             "--no-keepalive", "--csv"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    row = parse_csv(out)
    check(row["keepalive"] == "0", row)
    check(int(row["requests"]) > 0 and row["errors"] == "0", row)
    check_percentiles(row)


@no_server
def test_two_threads_and_warmup(ctx):
    srv = ctx.start("--threads", "2")
    code, out, err = run_binary(loadgen_args(srv, "-c", "6", "-t", "2", "-d", "1", "--warmup", "0.3",
                                             "--csv"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    row = parse_csv(out)
    check(int(row["requests"]) > 0 and row["errors"] == "0", row)
    check_percentiles(row)


@no_server
def test_server_closing_after_max_requests_is_not_an_error(ctx):
    # The server sends Connection: close after 5 requests; the client must
    # reconnect and carry on without counting that.
    srv = ctx.start("--max-keepalive-requests", "5")
    code, out, err = run_binary(loadgen_args(srv, "-c", "2", "-d", "1", "--warmup", "0", "--csv"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    row = parse_csv(out)
    check(int(row["requests"]) > 20 and row["errors"] == "0", row)
    stats = json.loads(srv.request(get("/api/stats")).body)
    check(stats["connections_accepted"] > 4, "no reconnects happened: %r" % stats)


@no_server
def test_counts_errors(ctx):
    srv = ctx.start()
    code, out, err = run_binary(loadgen_args(srv, "-c", "2", "-d", "1", "--warmup", "0", "--csv",
                                             path="/nope"), S(15))
    check(code == 1, "exit code %d, stderr %r" % (code, err))
    row = parse_csv(out)
    check(row["requests"] == "0" and int(row["errors"]) > 0, row)


@no_server
def test_connection_refused_exits_1(ctx):
    # A port nobody listens on: start a server only to learn a free port, then stop it.
    srv = ctx.start()
    port = srv.port
    srv.stop()
    code, out, err = run_binary([loadgen_binary(), "--port", str(port), "-c", "2", "-d", "1",
                                 "--warmup", "0", "--csv"], S(15))
    check(code == 1, "exit code %d, stderr %r" % (code, err))
    row = parse_csv(out)
    check(row["requests"] == "0" and int(row["errors"]) > 0, row)


@no_server
def test_idle_connections_open(ctx):
    srv = ctx.start()
    proc = subprocess.Popen(loadgen_args(srv, "-c", "2", "--idle", "20", "-d", "2", "--warmup", "0"),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        # 2 active connections + 20 idle + the probe's own connection (open while
        # /api/stats is answered). A loadgen that opened fewer idle connections
        # than asked never reaches 23. The highest value seen counts, because
        # an active connection may be reconnecting at the moment of a probe.
        seen = 0
        deadline = time.monotonic() + S(1.5)
        while time.monotonic() < deadline and proc.poll() is None and seen < 23:
            seen = max(seen, json.loads(srv.request(get("/api/stats")).body)["active_connections"])
            time.sleep(0.05)
        check(seen >= 23, "at most %d active connections; expected 2 + 20 idle + the probe" % seen)
        out, err = proc.communicate(timeout=S(15))
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    check(proc.returncode == 0, "exit code %d, stderr %r" % (proc.returncode, err))
    text = out.decode()
    check("idle_closed=0" in text, text)


@no_server
def test_idle_connection_closed_by_server_is_counted(ctx):
    srv = ctx.start("--keepalive-timeout", "0.5")
    code, out, err = run_binary(loadgen_args(srv, "-c", "1", "--idle", "3", "-d", "2", "--warmup", "0"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    check("idle_closed=3" in out, out)


@no_server
def test_human_output(ctx):
    srv = ctx.start()
    code, out, err = run_binary(loadgen_args(srv, "-c", "1", "-d", "1", "--warmup", "0"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    for word in ("requests", "errors", "duration", "requests/s", "p50", "p95", "p99", "max"):
        check(word in out, "missing %r in output:\n%s" % (word, out))


@no_server
def test_usage_errors_exit_2(ctx):
    binary = loadgen_binary()
    for args in (["--port", "8080", "-c", "0"], ["--port", "8080", "-d", "abc"], ["-c", "1"],
                 ["--port", "8080", "--bogus"], ["--port", "8080", "--label", "a,b"],
                 ["--port", "8080", "--path", "health"]):
        code, out, err = run_binary([binary] + args, S(5))
        check(code == 2, "args %r: exit code %d" % (args, code))
        check(err.strip() != "", "args %r: no message on stderr" % (args,))


@no_server
def test_too_many_connections_for_fd_limit_exits_2(ctx):
    import resource
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard == resource.RLIM_INFINITY:
        hard = 1 << 20
    code, out, err = run_binary([loadgen_binary(), "--port", "9", "-c", str(hard + 1), "-d", "1"], S(5))
    check(code == 2 and "file descriptor" in err, "exit %d, stderr %r" % (code, err))


@no_server
def test_help_exits_0(ctx):
    code, out, err = run_binary([loadgen_binary(), "--help"], S(5))
    check(code == 0 and "--no-keepalive" in out, "exit %d, out %r" % (code, out))


@no_server
def test_warmup_requests_are_not_counted(ctx):
    # One second of warm-up then half a second measured: the server sees roughly
    # three times as many requests as loadgen may report. Counting warm-up
    # samples would make the two numbers almost equal.
    srv = ctx.start()
    code, out, err = run_binary(loadgen_args(srv, "-c", "2", "-d", "0.5", "--warmup", "1", "--csv"), S(15))
    check(code == 0, "exit code %d, stderr %r" % (code, err))
    counted = int(parse_csv(out)["requests"])
    served = json.loads(srv.request(get("/api/stats")).body)["requests_total"]
    check(counted > 0 and served > 1.5 * counted, "loadgen counted %d of %d served requests" % (counted, served))


@no_server
def test_idle_connect_failure_is_a_setup_error(ctx):
    # Nothing listens on the port, so the idle connections cannot be opened.
    # The run must not report numbers claiming idle=2.
    srv = ctx.start()
    port = srv.port
    srv.stop()
    for extra in ([], ["--csv"]):
        code, out, err = run_binary([loadgen_binary(), "--port", str(port), "-c", "3", "--idle", "2",
                                     "-d", "1", "--warmup", "0"] + extra, S(15))
        check(code == 2, "args %r: exit code %d, stdout %r" % (extra, code, out))
        check("idle" in err, "no explanation on stderr: %r" % err)
        check(out.strip() == "", "numbers were printed for a failed setup: %r" % out)


@no_server
def test_first_request_of_preconnected_no_keepalive_connection_is_timed_from_run_start(ctx):
    # A pool server with one worker, held by an idle client, delays the answer
    # to loadgen's first request until the holder leaves. The connection itself
    # is established during loadgen's setup (the kernel completes it), so its
    # first request is sent at run start. That request must be counted, with
    # its real latency (about the hold time), not dropped as if it had begun
    # before the run. Pool mode is used whatever mode the suite runs in,
    # because only one-connection-per-worker gives a deterministic delay.
    from harness import ServerProc
    srv = ServerProc(ctx.binary, "pool", ["--threads", "1"])
    ctx.servers.append(srv)
    holder = srv.connect()
    holder.sendall(get("/health"))
    read_response(holder)  # the worker is now pinned to this connection
    proc = subprocess.Popen(loadgen_args(srv, "-c", "1", "-d", "1", "--warmup", "0", "--no-keepalive",
                                         "--csv"),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(0.4)
        holder.close()
        out, err = proc.communicate(timeout=S(15))
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    check(proc.returncode == 0, "exit code %d, stderr %r" % (proc.returncode, err))
    row = parse_csv(out.decode())
    check(row["errors"] == "0", row)
    check(float(row["max_ms"]) >= 300, "the delayed first request was not counted: %r" % row)
