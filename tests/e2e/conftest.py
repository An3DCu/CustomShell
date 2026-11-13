"""Shared fixtures for the end-to-end tests.

The tests run a built acsh. Which one comes from the ACSH environment variable
(the Makefile sets it). The same tests check the normal build and the
sanitizer build.
"""

import os
import shlex
import signal
import subprocess
import time
from pathlib import Path

import pexpect
import pytest

ROOT = Path(__file__).resolve().parents[2]


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "not_under_valgrind(reason): left out of `make test-valgrind`, for the reason given",
    )


@pytest.fixture(scope="session")
def acsh():
    """Absolute path to the acsh under test."""
    path = (ROOT / os.environ.get("ACSH", "build/acsh")).resolve()
    if not path.is_file():
        pytest.exit(f"{path} doesn't exist. Run make first.", returncode=2)
    return str(path)


@pytest.fixture(scope="session")
def tools():
    """The folder with my own tools (cat, head, wc, ...) for this build."""
    path = (ROOT / os.environ.get("TOOLS", "build/tools")).resolve()
    if not path.is_dir():
        pytest.exit(f"{path} doesn't exist. Run make first.", returncode=2)
    return path


@pytest.fixture
def workdir(tmp_path):
    """An empty folder for the test, with symlinks resolved so paths compare."""
    return Path(os.path.realpath(tmp_path))


def _env(env, unset):
    merged = dict(os.environ)
    merged.update(env or {})
    for name in unset or ():
        merged.pop(name, None)
    return merged


@pytest.fixture
def run(acsh, workdir):
    """Run acsh with arguments and stdin, in the test's folder.

    run("--version") or run(stdin="echo hi\\n"). Returns the finished
    process (returncode, stdout, stderr). Nothing here is a terminal. acsh
    shows no prompt then. Tests that need one use the `terminal` fixture.
    """

    def _run(*args, stdin="", env=None, unset=(), cwd=None, timeout=10):
        return subprocess.run(
            [acsh, *args],
            input=stdin,
            capture_output=True,
            text=True,
            cwd=cwd or workdir,
            env=_env(env, unset),
            timeout=timeout,
        )

    return _run


def _default_signals():
    """In the child, before acsh starts. Python ignores SIGPIPE and SIGXFSZ.
    A child would keep that. A terminal starts a shell with them at their
    defaults. subprocess (the run fixture) puts them back too."""
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    signal.signal(signal.SIGXFSZ, signal.SIG_DFL)


@pytest.fixture
def terminal(acsh, workdir):
    """Start acsh in a pseudo-terminal, the way a person would use it.

    Returns a pexpect child. The test sends lines and keys and waits for
    output. The child is killed at the end if it's still running.
    """
    children = []

    def _start(env=None, unset=(), cwd=None):
        # With no arguments, pexpect splits the command on spaces. This
        # repo's path may have some. Quoting keeps it one word. The history
        # file is the test's own, never the user's ~/.acsh_history.
        defaults = {"TERM": "dumb", "HISTFILE": str(workdir / ".acsh_history")}
        child = pexpect.spawn(
            shlex.quote(acsh),
            cwd=str(cwd or workdir),
            env=_env({**defaults, **(env or {})}, unset),
            preexec_fn=_default_signals,
            encoding="utf-8",
            timeout=5,
        )
        children.append(child)
        return child

    yield _start
    for child in children:
        if child.isalive():
            child.terminate(force=True)


@pytest.fixture
def prompt(terminal):
    """acsh in a terminal, at its first prompt, with a short PATH. At the end
    it leaves with exit. valgrind can then check it for leaks."""
    shell = terminal(env={"PATH": "/usr/bin:/bin"})
    shell.expect(r"acsh\$ ")
    yield shell
    if shell.isalive():
        shell.sendline("exit")
        if shell.expect([pexpect.EOF, "exit again"]) == 1:
            shell.sendline("exit")  # it had stopped jobs: the second exit leaves
            shell.expect(pexpect.EOF)


@pytest.fixture
def in_front():
    """Wait until a program runs in front on a terminal() shell.

    Only the group in front gets Ctrl+C. A test that presses it too early
    can miss the program while it's still starting. Output can't tell.
    `sh -c 'echo started; sleep 10'` prints before sleep exists.
    """

    def _wait(shell, name, count=1):
        """count: how many processes called name, for a pipeline like cat | cat."""
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            group = os.tcgetpgrp(shell.child_fd)
            found = 0
            for stat in Path("/proc").glob("[0-9]*/stat"):
                try:
                    text = stat.read_text()
                except OSError:
                    continue  # it ended meanwhile
                comm = text[text.index("(") + 1 : text.rindex(")")]
                state, _, pgrp = text[text.rindex(")") + 2 :].split()[:3]
                # not T: after fg it has the terminal a moment before SIGCONT
                if comm == name and int(pgrp) == group and state != "T":
                    found += 1
            if found >= count:
                return
            time.sleep(0.01)
        raise AssertionError(f"{name} never ran in front")

    return _wait
