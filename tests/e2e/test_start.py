"""acsh starts, answers --version and --help, and turns down what it doesn't know."""

import re

import pexpect


def test_version(run):
    result = run("--version")
    assert result.returncode == 0
    assert re.fullmatch(r"acsh \d+\.\d+\.\d+(-dev)?\n", result.stdout)
    assert result.stderr == ""


def test_help(run):
    result = run("--help")
    assert result.returncode == 0
    assert result.stdout.startswith("usage: acsh")
    assert result.stderr == ""


def test_unknown_option(run):
    result = run("--nope")
    assert result.returncode == 2
    assert result.stdout == ""
    assert result.stderr.startswith("acsh: unknown option '--nope'\nusage: acsh")


def test_runs_in_a_terminal(acsh):
    """pexpect gives acsh a pseudo-terminal. Later tests send Ctrl+C and keys through one."""
    child = pexpect.spawn(acsh, ["--help"], encoding="utf-8", timeout=5)
    child.expect("usage: acsh")
    child.expect(pexpect.EOF)
    child.close()
    assert child.exitstatus == 0
