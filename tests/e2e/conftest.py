"""Shared fixtures for the end-to-end tests.

The tests run a built acsh. Which one comes from the ACSH environment variable
(the Makefile sets it), so the same tests check the normal build and the
sanitizer build.
"""

import os
import shlex
import subprocess
import time
from pathlib import Path

import pexpect
import pytest

ROOT = Path(__file__).resolve().parents[2]


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
    process (returncode, stdout, stderr). Nothing here is a terminal, so acsh
    shows no prompt; tests that need one use the `terminal` fixture.
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


@pytest.fixture
def terminal(acsh, workdir):
    """Start acsh in a pseudo-terminal, the way a person would use it.

    Returns a pexpect child. The test sends lines and keys and waits for
    output. The child is killed at the end if it's still running.
    """
    children = []

    def _start(env=None, unset=(), cwd=None):
        # With no arguments, pexpect splits the command on spaces, and this
        # repo's path may have some. Quoting keeps it one word.
        child = pexpect.spawn(
            shlex.quote(acsh),
            cwd=str(cwd or workdir),
            env=_env({"TERM": "dumb", **(env or {})}, unset),
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
def in_front():
    """Wait until a program runs in front on a terminal() shell.

    Only the group in front gets Ctrl+C, so a test that presses it too
    early can miss the program while it's still starting. Output can't
    tell: `sh -c 'echo started; sleep 10'` prints before sleep exists.
    """

    def _wait(shell, name):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            group = os.tcgetpgrp(shell.child_fd)
            for stat in Path("/proc").glob("[0-9]*/stat"):
                try:
                    text = stat.read_text()
                except OSError:
                    continue  # it ended meanwhile
                comm = text[text.index("(") + 1 : text.rindex(")")]
                pgrp = int(text[text.rindex(")") + 2 :].split()[2])
                if comm == name and pgrp == group:
                    return
            time.sleep(0.01)
        raise AssertionError(f"{name} never ran in front")

    return _wait
