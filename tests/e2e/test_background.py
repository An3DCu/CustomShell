"""Background jobs: &, the notices, jobs, and no zombies.

Without a terminal the cases run in acsh and in dash and compare. The
notices only show in an interactive shell. Those tests use the terminal.
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
        "echo \"[$!]\"\n",                      # empty until something runs in the background
        "true & p=$!; [ -n \"$p\" ] && echo set\n",
    ],
)
def test_like_dash(same_as_dash, script):
    same_as_dash(script)


@pytest.mark.parametrize(
    "script",
    [
        "false & wait $!; echo $?\n",
        "sh -c 'exit 7' & p=$!; wait $p; echo $?\n",
        "sleep 0.2 & sleep 0.1 & wait; echo all $?\n",
        "wait; echo $?\n",
        "sleep 0.1 & wait %1; echo $?\n",
        "true & false & wait %%; echo $?\n",
        "false & true & wait %-; echo $?\n",
        "echo a | cat & wait; echo b\n",
        "for i in 1 2; do echo $i; done & wait\n",
        "sh -c 'kill -TERM $$' & wait $!; echo $?\n",
        "false & sleep 0.2; wait $!; echo $?\n",   # done before wait, still known
    ],
)
def test_wait_like_dash(same_as_dash, script):
    same_as_dash(script)


def test_wait_errors(run):
    result = run(stdin="wait 99999\necho $?\nwait %9\necho $?\nwait abc\necho $?\n")
    assert result.stderr == (
        "acsh: wait: pid 99999 isn't a child of this shell\n"
        "acsh: wait: %9: no such job\n"
        "acsh: wait: abc: not a pid or a job\n"
    )
    assert result.stdout == "127\n127\n2\n"


def test_wait_says_a_signal_killed_the_job(run):
    result = run(stdin="sh -c 'kill -TERM $$' & wait $!; echo $?\n")
    assert result.stdout == "143\n"
    assert result.stderr == "Terminated\n"


def test_a_script_job_ignores_ctrl_c(acsh, workdir):
    # Without job control a background command ignores SIGINT. Ctrl+C
    # (sent to the whole group here) then only stops what's in front.
    # Twenty jobs, each sent SIGINT right after it starts. The signal can
    # come before the child has set up ignoring it. None may die of it.
    script = workdir / "s.sh"
    script.write_text("for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n"
                      "  sleep 0.3 & kill -INT $!\n"
                      "done\nsleep 0.6\njobs | grep -c Interrupt\n")
    result = subprocess.run([acsh, str(script)], capture_output=True, text=True, timeout=10)
    assert result.stdout == "0\n"


def test_dollar_bang_is_the_last_command_of_the_job(run):
    # The job prints its own pid; $! has to be the same number. With a
    # pipeline, $! is its last command.
    result = run(stdin="sh -c 'echo $$' & sleep 0.2; echo $!\n"
                       "true | sh -c 'echo $$' & sleep 0.2; echo $!\n")
    lines = result.stdout.split()
    assert lines[0] == lines[1]
    assert lines[2] == lines[3]


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


def test_notices_when_a_job_starts_and_ends(prompt):
    prompt.sendline("sleep 0.3 &")
    prompt.expect(r"job 1 started: sleep 0\.3 \(pid (\d+)\)")
    pid = prompt.match.group(1)
    prompt.expect(r"acsh\$ ")
    prompt.sendline("echo $!")
    prompt.expect(f"\r\n{pid}\r\n")  # the notice and $! agree
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
    prompt.expect("job 1 started")  # nothing left. It's 1 again.


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
    # Only the group in front may read the terminal. A job that tries gets
    # SIGTTIN and stops. In front again, after fg, it can read.
    prompt.sendline("cat &")
    prompt.expect("job 1 started")
    time.sleep(0.3)
    prompt.sendline("jobs")
    prompt.expect_exact("job 1  stopped  cat")
    prompt.expect_exact("job 1 stopped: cat")  # the notice before the prompt
    prompt.sendline("fg")
    prompt.expect_exact("fg\r\ncat\r\n")
    prompt.sendline("typed")
    prompt.expect_exact("typed\r\ntyped\r\n")  # the echo, then cat's output
    prompt.sendcontrol("d")
    prompt.expect(r"acsh\$ ")


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
    # The self-pipe wakes the prompt when the job ends. The job doesn't stay
    # a zombie until the next Enter. The notice still waits for that Enter.
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


def test_ctrl_c_stops_wait_and_the_job(prompt):
    prompt.sendline("sleep 10 &")
    prompt.expect("job 1 started")
    prompt.sendline("wait")
    prompt.expect_exact("wait\r\n")
    time.sleep(0.5)
    prompt.sendintr()
    prompt.expect(r"acsh\$ ")
    prompt.sendline("echo $?")
    prompt.expect("130")
    # The job got the Ctrl+C too. Its notice says so. Nothing runs.
    time.sleep(0.2)
    prompt.sendline("jobs; echo end")
    prompt.expect("end\r\n")
    output = prompt.before
    prompt.expect(r"acsh\$ ")
    assert "running" not in output


def test_wait_waits_for_a_stopped_job_too(prompt):
    prompt.sendline("cat &")  # stops: it reads the terminal from the background
    prompt.expect("job 1 started")
    time.sleep(0.3)
    prompt.sendline("wait")
    prompt.expect_exact("wait\r\n")
    with pytest.raises(pexpect.TIMEOUT):
        prompt.expect(r"acsh\$ ", timeout=1)
    prompt.sendintr()
    prompt.expect(r"acsh\$ ")
    prompt.sendline("echo $?")
    prompt.expect("130")


def test_explain_shows_a_job(prompt):
    prompt.sendline("explain sleep 0.1 &")
    prompt.expect("in the background \\(&\\): sleep 0.1")
    prompt.expect(r"job 1\s+in the background: the shell doesn't wait for it")
    prompt.expect(r"job 1 started: sleep 0\.1")
    prompt.expect("status 0")
    assert "waitpid" not in prompt.before
