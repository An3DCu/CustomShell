"""Process groups and the terminal, in a pseudo-terminal.

ps reports a process's group (pgid) and the terminal's foreground group
(tpgid). The checks run ps from inside sh -c, with single quotes so the
$$ is sh's own pid, not acsh's.
"""

import re
import time


def numbers(shell, line, count):
    """Sends line and returns the first count numbers it prints."""
    shell.sendline(line)
    shell.expect_exact(line + "\r\n")  # the echo of what was typed
    shell.expect(r"acsh\$ ")
    found = [int(n) for n in re.findall(r"\d+", shell.before)]
    assert len(found) >= count, shell.before
    return found[:count]


def test_a_command_gets_its_own_group_and_the_terminal(prompt):
    shell_pid, shell_pgid = numbers(prompt, "ps -o pid=,pgid= -p $$", 2)
    assert shell_pgid == shell_pid  # the shell leads its own group
    pid, pgid, tpgid = numbers(prompt, "sh -c 'ps -o pid=,pgid=,tpgid= -p $$'", 3)
    assert pgid == pid             # sh leads a new group
    assert pgid != shell_pgid
    assert tpgid == pgid           # and that group has the terminal


def test_a_pipeline_shares_one_group(prompt):
    first, second = numbers(
        prompt, "sh -c 'ps -o pgid= -p $$; sleep 0.3' | sh -c 'cat; ps -o pgid= -p $$'", 2)
    assert first == second


def test_the_shell_gets_the_terminal_back(prompt):
    numbers(prompt, "sh -c 'ps -o pgid= -p $$'", 1)
    pgid, tpgid = numbers(prompt, "ps -o pgid=,tpgid= -p $$", 2)
    # While this ps runs, its own group has the terminal, not the shell's.
    assert tpgid != pgid
    # And afterwards the shell can read from the terminal again.
    prompt.sendline("read x")
    prompt.sendline("hello")
    prompt.sendline("echo got $x")
    prompt.expect("got hello")


def test_a_command_in_front_can_read_the_terminal(prompt):
    prompt.sendline("head -n 1")
    prompt.expect_exact("head -n 1\r\n")
    prompt.sendline("typed")
    prompt.expect_exact("typed\r\ntyped\r\n")  # the echo, then head's output
    prompt.expect(r"acsh\$ ")


def test_terminal_modes_come_back_after_a_command(prompt):
    prompt.sendline("stty -echo")
    prompt.expect(r"acsh\$ ")
    prompt.sendline("echo visible")
    # With the shell's modes back, the terminal echoes what's typed again.
    prompt.expect_exact("echo visible\r\nvisible\r\n")


def test_a_loop_in_a_pipeline_leaves_job_control_to_the_shell(prompt):
    # The loop runs in a child. It used to take the terminal back for the
    # shell after its first sleep, got SIGTTOU, and the pipeline stopped.
    prompt.sendline("for i in 1 2; do sleep 0.2; done | cat; echo done $?")
    prompt.expect(r"job 1 stopped|done 0")
    assert prompt.match.group(0) == "done 0"


def test_explain_shows_the_group_and_the_terminal(prompt):
    prompt.sendline("explain true | cat")
    # the shell's own lines come first; the child's are copied in after them
    prompt.expect(r"fork\(\) = \d+")
    prompt.expect(r"tcsetpgrp\(\d+, \d+\)\s+the pipeline's group gets the terminal")
    prompt.expect(r"\] setpgid\(0, \d+\)\s+a new process group, led by this child")
    prompt.expect(r"\] setpgid\(0, \d+\)\s+joins the pipeline's group")
    prompt.expect(r"tcsetpgrp\(\d+, \d+\)\s+the shell takes the terminal back")
    prompt.expect(r"acsh\$ ")


def test_an_interactive_acsh_inside_acsh(prompt, acsh):
    prompt.sendline(f"'{acsh}'")
    prompt.expect(r"acsh\$ ")
    prompt.sendline("sh -c 'ps -o pgid=,tpgid= -p $$'")
    prompt.expect(r"(\d+)\s+(\d+)")
    assert prompt.match.group(1) == prompt.match.group(2)
    prompt.expect(r"acsh\$ ")
    prompt.sendline("exit")
    prompt.expect(r"acsh\$ ")  # back in the outer one
    prompt.sendline("echo outer")
    prompt.expect("outer")


def test_scripts_keep_their_commands_in_their_own_group(run):
    result = run(stdin="sh -c 'ps -o pgid= -p $$'\nps -o pgid= -p $$\n")
    child, shell = result.stdout.split()
    assert child == shell


def test_no_terminal_copy_without_job_control(run):
    result = run(stdin="sh -c 'ls /proc/$PPID/fd'\n")
    assert result.stdout.split() == ["0", "1", "2"]
