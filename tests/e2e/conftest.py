"""Shared fixtures for the end-to-end tests.

The tests run a built acsh. Which one comes from the ACSH environment variable
(the Makefile sets it), so the same tests check the normal build and the
sanitizer build.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="session")
def acsh():
    """Absolute path to the acsh under test."""
    path = (ROOT / os.environ.get("ACSH", "build/acsh")).resolve()
    if not path.is_file():
        pytest.exit(f"{path} doesn't exist. Run make first.", returncode=2)
    return str(path)


@pytest.fixture
def run(acsh, tmp_path):
    """Run acsh with arguments and optional stdin, in an empty temp folder.

    Returns the finished process (returncode, stdout, stderr). Nothing here is
    a terminal; tests that need one use pexpect.
    """

    def _run(*args, stdin="", timeout=10):
        return subprocess.run(
            [acsh, *args],
            input=stdin,
            capture_output=True,
            text=True,
            cwd=tmp_path,
            timeout=timeout,
        )

    return _run
