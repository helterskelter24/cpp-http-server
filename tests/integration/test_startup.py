"""Command-line handling and start-up failures."""

import os

from harness import S, Skip, check, is_sanitizer_build, limit_address_space, no_server, run_binary


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


@no_server
def test_thread_creation_failure_exits_cleanly(ctx):
    # 200 threads need 1.6 GB of address space for their 8 MiB stacks, so with
    # 300 MiB the server runs out part-way through starting them. It must say
    # so and exit 1, not end in std::terminate (SIGABRT) with threads still
    # joinable.
    if is_sanitizer_build(ctx.binary):
        raise Skip("sanitizer runtimes cannot start under an address-space limit")
    code, _, err = run_binary(ctx.base_args() + ["--threads", "200"], S(5),
                              preexec=limit_address_space(300 * 2**20))
    check(code == 1, "exit code %r, stderr %r" % (code, err))
    check("cannot create threads" in err, err)
