"""Background jobs: &, the notices, jobs, and no zombies.

Without a terminal the cases run in acsh and in dash and compare. The
notices only show in an interactive shell, so those tests use the terminal.
"""

import os
import shutil
import subprocess
import time

import pexpect
import pytest

DASH = shutil.which("dash")


@pytest.fixture
def same_as_dash(acsh, workdir):
    if DASH is None:
        pytest.skip("dash isn't installed")

    def _check(script):
        results = []
        for shell in (acsh, DASH):
            folder = workdir / os.path.basename(shell)
            folder.mkdir(exist_ok=True)
            results.append(subprocess.run([shell], input=script, capture_output=True,
                                          text=True, cwd=folder, timeout=10))
        mine, theirs = results
        assert mine.stdout == theirs.stdout, f"stderr: {mine.stderr!r}"
        assert mine.returncode == theirs.returncode
        return mine

    return _check


@pytest.mark.parametrize(
    "script",
    [
        "echo in-bg & sleep 0.2; echo fg\n",
        "echo a | tr a b & sleep 0.2; echo c\n",
        "cat & sleep 0.2; echo after\n",       # stdin is /dev/null without job control
        "X=1; X=2 & sleep 0.1; echo $X\n",     # a job can't change the shell
        "cd / & sleep 0.1; pwd | grep -c '^/$'\n",
        "for i in 1 2; do echo $i; done & sleep 0.2\n",
        "false & echo $?\n",                    # starting a job always works
        "sleep 0.1 & sleep 0.1 & echo two\n",
    ],
)
def test_like_dash(same_as_dash, script):
    same_as_dash(script)


@pytest.mark.parametrize("line", ["a && b &", "a || b &", "! a &"])
def test_lists_in_the_background_are_not_allowed_yet(run, line):
    result = run(stdin=f"echo before\n{line}\necho after\n")
    assert result.stderr == "acsh: only a single pipeline can run in the background (for now)\n"
    assert result.stdout == "before\n"
    assert result.returncode == 2


def test_jobs_in_a_script(run):
    result = run(stdin="sleep 0.3 &\njobs\nsleep 0.5\njobs\necho end\n")
    assert result.stdout == "job 1  running  sleep 0.3\njob 1  done     sleep 0.3 (exit 0)\nend\n"


def test_no_zombies_in_a_script(run):
    lines = "true &\n" * 20 + "sleep 0.3\n" + "ps -o stat= --ppid $$ | grep -c Z\n"
    result = run(stdin=lines)
    assert result.stdout.split()[-1] == "0"


# ---- in a terminal -------------------------------------------------------------


@pytest.fixture
def prompt(terminal):
    shell = terminal(env={"PATH": "/usr/bin:/bin"})
    shell.expect(r"acsh\$ ")
    yield shell
    if shell.isalive():
        shell.sendline("exit")
        shell.expect(pexpect.EOF)


def test_notices_when_a_job_starts_and_ends(prompt):
    prompt.sendline("sleep 0.3 &")
    prompt.expect(r"job 1 started: sleep 0\.3 \(pid \d+\)")
    prompt.expect(r"acsh\$ ")
    time.sleep(0.6)
    prompt.sendline("")
    prompt.expect_exact("job 1 done: sleep 0.3 (exit 0)")
    prompt.expect(r"acsh\$ ")
    prompt.sendline("jobs; echo none-left")
    prompt.expect(r"none-left")
    assert "job 1" not in prompt.before


def test_notice_for_a_job_that_was_killed(prompt):
    prompt.sendline("sh -c 'kill -TERM $$' &")
    prompt.expect("job 1 started")
    time.sleep(0.3)
    prompt.sendline("")
    prompt.expect_exact("job 1 done: sh -c 'kill -TERM $$' (Terminated)")


def test_jobs_lists_them(prompt):
    prompt.sendline("sleep 10 &")
    prompt.expect("job 1 started")
    prompt.sendline("sleep 10 | cat &")
    prompt.expect("job 2 started")
    prompt.sendline("jobs")
    prompt.expect_exact("job 1  running  sleep 10\r\njob 2  running  sleep 10 | cat\r\n")


def test_numbers_go_up_and_start_again(prompt):
    prompt.sendline("true &")
    prompt.expect("job 1 started")
    time.sleep(0.3)
    prompt.sendline("")
    prompt.expect("job 1 done")
    prompt.sendline("true &")
    prompt.expect("job 1 started")  # nothing left, so 1 again


def test_a_job_is_not_in_front(prompt):
    prompt.sendline("sh -c 'ps -o pgid=,tpgid= -p $$' &")
    prompt.expect(r"(\d+)\s+(\d+)\r\n")
    assert prompt.match.group(1) != prompt.match.group(2)


def test_ctrl_c_doesnt_reach_a_job(prompt):
    prompt.sendline("sleep 10 &")
    prompt.expect("job 1 started")
    prompt.expect(r"acsh\$ ")
    prompt.sendline("sleep 10")
    prompt.expect_exact("sleep 10\r\n")
    time.sleep(0.5)
    prompt.sendintr()
    prompt.expect(r"acsh\$ ")
    prompt.sendline("jobs")
    prompt.expect_exact("job 1  running  sleep 10")


def test_a_job_that_reads_the_terminal_stops(prompt):
    # Only the group in front may read the terminal; a job that tries gets
    # SIGTTIN and stops. Ctrl+Z and fg come with #33.
    prompt.sendline("cat &")
    prompt.expect("job 1 started")
    time.sleep(0.3)
    prompt.sendline("jobs")
    prompt.expect_exact("job 1  stopped  cat")


def test_no_zombies_at_the_prompt(prompt):
    for _ in range(5):
        prompt.sendline("true &")
        prompt.expect("started")
    time.sleep(0.5)
    prompt.sendline("")
    prompt.expect(r"acsh\$ ")
    prompt.sendline("ps -o stat= --ppid $$ | grep -c Z")
    prompt.expect(r"\r\n(\d+)\r\n")
    assert prompt.match.group(1) == "0"


def test_a_job_is_collected_while_the_prompt_waits(prompt):
    # The self-pipe wakes the prompt when the job ends, so it doesn't stay a
    # zombie until the next Enter; the notice still waits for that Enter.
    prompt.sendline("sleep 0.2 &")
    prompt.expect("job 1 started")
    prompt.expect(r"acsh\$ ")
    time.sleep(0.8)
    states = subprocess.run(["ps", "-o", "stat=", "--ppid", str(prompt.pid)],
                            capture_output=True, text=True).stdout.split()
    assert not any(s.startswith("Z") for s in states)
    with pytest.raises(pexpect.TIMEOUT):
        prompt.expect("done", timeout=0.3)
    prompt.sendline("")
    prompt.expect_exact("job 1 done: sleep 0.2 (exit 0)")


def test_explain_shows_a_job(prompt):
    prompt.sendline("explain sleep 0.1 &")
    prompt.expect("in the background \\(&\\): sleep 0.1")
    prompt.expect(r"job 1\s+in the background: the shell doesn't wait for it")
    prompt.expect(r"job 1 started: sleep 0\.1")
    prompt.expect("status 0")
    assert "waitpid" not in prompt.before
