#!/usr/bin/env python3
"""Runs the integration tests against one server binary in one mode.

    run_all.py --binary build/httpserver --mode pool [-k SUBSTRING]

Every `test_*` function in the `test_*.py` modules next to this file is a test.
Each gets a fresh server (with the flags from @flags) unless it is marked
@shared or @no_server. After each server exits the harness fails the test if
the exit code after a requested shutdown was not 0 or if stderr holds a
sanitizer report. Every test has a hard time limit so a hung server fails the
test instead of hanging the run. A test that cannot run on this host (for
example, too low a file descriptor limit) raises harness.Skip with the reason.
"""

import argparse
import importlib
import inspect
import os
import signal
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import harness  # noqa: E402

DEFAULT_TIME_LIMIT = 20.0


class TestTimeout(Exception):
    pass


def _on_alarm(signum, frame):
    raise TestTimeout("hard time limit reached")


def discover(selector):
    tests = []
    for filename in sorted(os.listdir(HERE)):
        if not (filename.startswith("test_") and filename.endswith(".py")):
            continue
        module = importlib.import_module(filename[:-3])
        for name, fn in vars(module).items():
            if name.startswith("test_") and inspect.isfunction(fn) and fn.__module__ == module.__name__:
                full = "%s::%s" % (module.__name__, name)
                if selector is None or selector in full:
                    tests.append((module.__name__, full, fn))
    return tests


def server_problems(srv, expect_clean_exit):
    """Problems found after a server has exited: sanitizer output, bad exit code."""
    problems = []
    report = srv.sanitizer_report()
    if report:
        problems.append("sanitizer report in server stderr: " + report.strip())
    if expect_clean_exit and srv.exit_code != 0:
        problems.append("server exit code %r after shutdown, expected 0" % srv.exit_code)
    return problems


def finish_server(srv):
    """Stops a server that is still running and returns the problems found.
    A server the test already stopped keeps the exit code it reported."""
    expect_clean = True
    if srv.running():
        try:
            srv.stop()
        except harness.TestFailure as e:
            return [str(e)] + server_problems(srv, False)
    elif srv.exit_code is None:
        # It exited on its own, which no test asks for.
        srv.kill()
        return ["server exited unexpectedly with code %r" % srv.exit_code] + server_problems(srv, False)
    else:
        expect_clean = False  # the test looked at the exit code itself
    return server_problems(srv, expect_clean)


def tail(text, lines=20):
    return "\n".join(text.splitlines()[-lines:])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--mode", required=True, choices=["pool", "epoll"])
    parser.add_argument("-k", dest="selector", default=None, help="run tests whose name contains this")
    args = parser.parse_args()
    binary = os.path.abspath(args.binary)

    signal.signal(signal.SIGALRM, _on_alarm)
    passed = failed = skipped = 0
    shared_srv = None
    shared_module = None

    def close_shared():
        nonlocal shared_srv, failed
        if shared_srv is None:
            return
        problems = finish_server(shared_srv)
        if problems:
            failed += 1
            print("FAIL %s::<shared server>\n  %s\n  server stderr:\n%s"
                  % (shared_module, "\n  ".join(problems), tail(shared_srv.stderr())))
        shared_srv = None

    for module, full, fn in discover(args.selector):
        if module != shared_module:
            close_shared()
            shared_module = module

        only = getattr(fn, "only_mode", None)
        if only and only != args.mode:
            skipped += 1
            print("SKIP %s (%s only)" % (full, only))
            continue

        start = time.monotonic()
        problems = []
        skip_reason = None
        srv = None
        ctx = None
        signal.setitimer(signal.ITIMER_REAL, harness.S(getattr(fn, "time_limit", DEFAULT_TIME_LIMIT)))
        try:
            if getattr(fn, "no_server", False):
                ctx = harness.Context(binary, args.mode)
                fn(ctx)
            elif getattr(fn, "shared", False):
                if shared_srv is not None and not shared_srv.running():
                    close_shared()
                if shared_srv is None:
                    shared_srv = harness.ServerProc(binary, args.mode)
                fn(shared_srv)
            else:
                srv = harness.ServerProc(binary, args.mode, getattr(fn, "server_flags", []))
                fn(srv)
        except harness.Skip as e:
            skip_reason = str(e)
        except Exception:
            problems.append(traceback.format_exc().rstrip())
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            # Servers are always cleaned up, even after a timeout or failure.
            if srv is not None:
                problems += finish_server(srv)
            if ctx is not None:
                for s in ctx.servers:
                    if s.running():
                        s.kill()
                    report = s.sanitizer_report()
                    if report:
                        problems.append("sanitizer report in server stderr: " + report.strip())

        elapsed = time.monotonic() - start
        if skip_reason is not None and not problems:
            skipped += 1
            print("SKIP %s (%s)" % (full, skip_reason))
        elif problems:
            failed += 1
            print("FAIL %s (%.2fs)" % (full, elapsed))
            for p in problems:
                print("  " + p.replace("\n", "\n  "))
            for s in ([srv] if srv else []) + (ctx.servers if ctx else []) + ([shared_srv] if shared_srv and getattr(fn, "shared", False) else []):
                print("  server stderr (last lines):\n" + tail(s.stderr()))
        else:
            passed += 1
            print("PASS %s (%.2fs)" % (full, elapsed))
        sys.stdout.flush()

    close_shared()
    print("%d passed, %d failed, %d skipped" % (passed, failed, skipped))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
