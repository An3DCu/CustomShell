"""Awkward input: very long lines, deep nesting, zero bytes, a low fd limit,
no HOME or PATH, and a big paste into the editor. Nothing may crash or hang.
"""

import os
import resource
import subprocess
import time

import pexpect
import pytest


def run_limited(acsh, workdir, script, nofile):
    """acsh with `ulimit -n nofile`, reading script from stdin."""
    def limit():
        resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))
    return subprocess.run([acsh], input=script, capture_output=True, text=True, cwd=workdir,
                          timeout=20, preexec_fn=limit)


def nested(depth):
    return "if true; then " * depth + "echo deep" + "; fi" * depth + "\n"


def test_a_long_line_and_many_arguments(run):
    line = "echo " + "x" * 1_000_000 + "\nset -- " + "a " * 100_000 + "\necho $#\n"
    result = run(stdin=line, timeout=30)  # about 2 s under valgrind, more on CI
    assert result.stdout == "x" * 1_000_000 + "\n100000\n"


def test_a_long_pipeline(run):
    result = run(stdin="echo hi" + " | cat" * 200 + "\n")
    assert result.stdout == "hi\n"


def test_nesting_up_to_1000_levels(run):
    assert run(stdin=nested(1000)).stdout == "deep\n"


# 10,000 crashed the sanitizer build before the limit. Deeper only makes the
# input longer, and under valgrind on CI 100,000 took over 10 seconds to read.
@pytest.mark.parametrize("depth", [1001, 10_000])
def test_deeper_nesting_is_an_error_not_a_crash(run, depth):
    result = run(stdin=nested(depth))
    assert result.stderr == "acsh: nested more than 1000 levels deep\n"
    assert result.returncode == 2


def test_zero_bytes_are_dropped_with_one_warning(acsh, workdir):
    result = subprocess.run([acsh], input=b"echo a\0b\necho c\0d\n", capture_output=True,
                            cwd=workdir, timeout=10)
    assert result.stdout == b"ab\ncd\n"
    assert result.stderr == b"acsh: ignored a zero byte in the input\n"


@pytest.mark.not_under_valgrind("valgrind needs fds of its own, and 8 isn't enough")
def test_a_low_fd_limit_says_why(acsh, workdir):
    result = run_limited(acsh, workdir, "echo b > f\necho $?\n", nofile=8)
    assert result.stderr == ("acsh: > f: can't save fd 1: no fd at 10 or above is free "
                             "(ulimit -n is 8)\n")
    assert result.stdout == "1\n"


def test_no_home_and_no_path(run):
    result = run(stdin="cd\necho $?\nls /dev/null\n", unset=("HOME", "PATH"))
    assert result.stderr == "acsh: cd: HOME not set\n"
    assert result.stdout == "1\n/dev/null\n"  # no PATH: the usual folders, as execvp does


def test_a_big_paste_into_the_editor_is_quick(terminal):
    shell = terminal(env={"TERM": "xterm", "PATH": "/usr/bin:/bin"})
    shell.expect(r"acsh\$ ")
    start = time.monotonic()
    shell.send("echo " + "p" * 20_000 + " | wc -c\r")  # one write, as a paste is
    shell.expect(r"\r\n20001\r\n", timeout=60)
    # drawing the line after every key took minutes for this; once is enough
    assert time.monotonic() - start < 30
