"""Command-line handling and start-up failures."""

import os

from harness import S, check, no_server, run_binary


@no_server
def test_port_in_use(ctx):
    first = ctx.start()
    code, _, err = run_binary(ctx.base_args(port=first.port), S(2))
    check(code == 1, "exit code %r, stderr %r" % (code, err))
    check("in use" in err or str(first.port) in err, err)


@no_server
def test_missing_root(ctx):
    root = "/nonexistent/hs-root-%d" % os.getpid()
    code, _, err = run_binary(ctx.base_args(root=root), S(2))
    check(code == 1, "exit code %r, stderr %r" % (code, err))
    check(root in err, err)


@no_server
def test_bad_flag(ctx):
    code, out, err = run_binary(ctx.base_args() + ["--no-such-flag"], S(2))
    check(code == 2, "exit code %r" % code)
    check("--no-such-flag" in err and "Usage:" in err, err)
    check(out == "", out)


@no_server
def test_help_flag(ctx):
    code, out, err = run_binary([ctx.binary, "--help"], S(2))
    check(code == 0, "exit code %r" % code)
    check("Usage:" in out, out)
