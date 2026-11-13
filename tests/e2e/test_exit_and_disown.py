"""exit and Ctrl+D with jobs left, and disown.

The stopped jobs stop themselves with kill -STOP, at a known point.
"""

import os
import signal
import time

import pexpect

PROMPT = r"acsh\$ "
STOPPED = "sh -c 'kill -STOP $$; sleep 30'"
WARNING = (f"acsh: stopped jobs: job 1 ({STOPPED})\r\n"
           "acsh: exit again to leave; they'll get SIGHUP\r\n")


def stop_one(shell):
    shell.sendline(STOPPED)
    shell.expect_exact("job 1 stopped")
    shell.expect(PROMPT)


def start_one(shell):
    """sleep 30 in the background. Returns its pid."""
    shell.sendline("sleep 30 &")
    shell.expect(r"started: sleep 30 \(pid (\d+)\)")
    pid = int(shell.match.group(1))
    shell.expect(PROMPT)
    return pid


def gone(pid):
    """True once pid has ended. Its parent, acsh, is gone too. init collects
    it then. /proc/pid disappears."""
    for _ in range(100):
        if not os.path.exists(f"/proc/{pid}"):
            return True
        time.sleep(0.05)
    return False


def still_running(pid):
    """True if pid is still there half a second later."""
    time.sleep(0.5)
    return os.path.exists(f"/proc/{pid}")


def test_exit_warns_about_stopped_jobs_then_leaves(prompt):
    stop_one(prompt)
    running = start_one(prompt)
    prompt.sendline("exit 5")
    prompt.expect_exact(WARNING)
    prompt.expect(PROMPT)
    prompt.sendline("exit")  # right after: this one leaves
    prompt.expect(pexpect.EOF)
    prompt.close()
    assert prompt.exitstatus == 1  # the warned exit's status
    assert gone(running)  # running jobs get SIGHUP too


def test_a_command_in_between_means_a_new_warning(prompt):
    stop_one(prompt)
    prompt.sendline("exit")
    prompt.expect_exact(WARNING)
    prompt.sendline("echo $?")
    prompt.expect_exact("1\r\n")
    prompt.sendline("exit")
    prompt.expect_exact(WARNING)


def test_ctrl_d_works_like_exit(prompt):
    stop_one(prompt)
    prompt.sendcontrol("d")
    prompt.expect_exact(WARNING)
    prompt.expect(PROMPT)
    prompt.sendcontrol("d")
    prompt.expect(pexpect.EOF)


def test_a_disowned_job_keeps_running_after_exit(prompt):
    pid = start_one(prompt)
    prompt.sendline("disown")
    prompt.expect(PROMPT)
    prompt.sendline("exit")
    prompt.expect(pexpect.EOF)
    try:
        assert still_running(pid)
    finally:
        os.kill(pid, signal.SIGKILL)


def test_disown_forms_and_messages(prompt):
    stop_one(prompt)
    pid = start_one(prompt)
    try:
        prompt.sendline("disown")  # the current job: the stopped one
        prompt.expect_exact("acsh: disown: job 1 is stopped; it stays stopped\r\n")
        prompt.sendline("disown %7")
        prompt.expect_exact("acsh: disown: %7: no such job\r\n")
        prompt.sendline("disown -a; jobs; disown; echo $?")
        prompt.expect_exact("acsh: disown: no current job\r\n1\r\n")
    finally:
        os.kill(pid, signal.SIGKILL)  # disowned: nothing else would end it


def test_a_scripts_jobs_keep_running_after_it_ends(run):
    result = run(stdin="sleep 30 > /dev/null 2>&1 &\necho $!\n")
    pid = int(result.stdout)
    try:
        assert still_running(pid)  # no SIGHUP from a script
    finally:
        os.kill(pid, signal.SIGKILL)
