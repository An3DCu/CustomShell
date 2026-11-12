"""kill: signals to jobs and processes, and kill -l.

The forms dash knows are compared with dash; the ones only bash knows
(-SIGTERM, lowercase names) with bash. The messages and the -l list are
acsh's own.
"""

import os
import shutil
import subprocess
import time
from pathlib import Path

import pexpect
import pytest

DASH = shutil.which("dash")
BASH = shutil.which("bash")


@pytest.fixture
def same_as(acsh, workdir):
    def _check(other, script):
        if other is None:
            pytest.skip("the shell to compare with isn't installed")
        results = []
        for shell in (acsh, other):
            folder = workdir / os.path.basename(shell)
            folder.mkdir(exist_ok=True)
            results.append(subprocess.run([shell], input=script, capture_output=True,
                                          text=True, cwd=folder, timeout=10))
        mine, theirs = results
        assert mine.stdout == theirs.stdout, f"stderr: {mine.stderr!r}"
        assert mine.returncode == theirs.returncode

    return _check


@pytest.mark.parametrize(
    "script",
    [
        "sleep 5 & kill $!; wait $!; echo $?\n",
        "sleep 5 & kill -9 $!; wait $!; echo $?\n",
        "sleep 5 & kill -s KILL $!; wait $!; echo $?\n",
        "sleep 5 & kill -0 $!; echo $?; kill $!\n",  # 0 only checks it's there
        "kill -l 143; kill -l 9\n",
    ],
)
def test_like_dash(same_as, script):
    same_as(DASH, script)


@pytest.mark.parametrize(
    "script",
    [
        "sleep 5 & kill -SIGKILL $!; wait $!; echo $?\n",
        "sleep 5 & kill -hup $!; wait $!; echo $?\n",
        "sleep 5 & kill -s sigusr1 $!; wait $!; echo $?\n",
        "sleep 5 & kill -HUP -- $!; wait $!; echo $?\n",
        # every process of the job. The pause lets both reach exec first: under
        # valgrind a signal that comes before that is lost.
        "sleep 5 | sleep 5 & sleep 0.5; kill %1; wait %1; echo $?\n",
        "kill -l TERM sigkill\n",
    ],
)
def test_like_bash(same_as, script):
    same_as(BASH, script)


def test_kill_errors(run):
    result = run(stdin=(
        "kill 2147483647 abc %3\necho $?\n"
        "kill -FOO 1\necho $?\n"
        "kill\necho $?\n"
        "kill -s\necho $?\n"
    ))
    assert result.stderr == (
        "acsh: kill: 2147483647: no such process\n"
        "acsh: kill: abc: not a pid or a job\n"
        "acsh: kill: %3: no such job\n"
        "acsh: kill: FOO: no such signal\n"
        "acsh: kill: usage: kill [-SIGNAL] %n | pid ...\n"
        "acsh: kill: usage: kill [-SIGNAL] %n | pid ...\n"
    )
    assert result.stdout == "1\n1\n2\n2\n"


def test_kill_dash_l_lists_number_name_and_meaning(run):
    lines = run(stdin="kill -l\n").stdout.splitlines()
    assert lines[0] == " 1  HUP     Hangup"
    assert " 9  KILL    Killed" in lines
    assert "15  TERM    Terminated" in lines
    assert len(lines) == 31  # Linux's standard signals, HUP to SYS


def only_the_shell_left(shell):
    """Wait until every other process on the shell's terminal has ended."""
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        others = 0
        for stat in Path("/proc").glob("[0-9]*/stat"):
            try:
                fields = stat.read_text().rsplit(")", 1)[1].split()
            except OSError:
                continue
            if int(fields[3]) == shell.pid and int(stat.parent.name) != shell.pid:
                others += 1  # fields[3] is the session
        if others == 0:
            return
        time.sleep(0.01)
    raise AssertionError("the job never ended")


def test_kill_wakes_a_stopped_job_so_it_ends(terminal):
    shell = terminal(env={"PATH": "/usr/bin:/bin"})
    shell.expect(r"acsh\$ ")
    shell.sendline("sh -c 'kill -STOP $$; sleep 30'")
    shell.expect_exact("job 1 stopped")
    shell.expect(r"acsh\$ ")
    shell.sendline("explain kill %1")
    shell.expect(r"kill\(-\d+, SIGTERM\)\s+the signal goes to the whole group")
    shell.expect(r"kill\(-\d+, SIGCONT\)\s+it's stopped: wake it so it can end")
    # The notice comes before the next prompt: this one if the job ended
    # first, else the one after Enter. Both are still ahead in the output.
    only_the_shell_left(shell)
    shell.sendline("")
    shell.expect_exact("job 1 done: sh -c 'kill -STOP $$; sleep 30' (Terminated)")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)
