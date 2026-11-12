"""Ctrl+C and Ctrl+\\: they stop the command, never the interactive shell.

Most of these run acsh in a pseudo-terminal, since Ctrl+C only means
something there: the terminal turns it into SIGINT for the whole
foreground group. The expected behavior was recorded from bash first.
"""

import signal
import subprocess

import pexpect

PROMPT = r"\$ $"
LONG = "sleep 10"


def leave(shell):
    """Type exit and return the shell's exit status (the last command's)."""
    shell.sendline("exit")
    shell.expect(pexpect.EOF)
    shell.close()
    return shell.exitstatus


# ---- Ctrl+C -----------------------------------------------------------------


def test_ctrl_c_at_the_prompt_gives_a_new_line(terminal):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendintr()
    shell.expect(r"\^C\r\n.*" + PROMPT)
    assert shell.isalive()
    assert leave(shell) == 130  # like bash


def test_ctrl_c_right_after_the_prompt_is_never_lost(terminal):
    # Ctrl+C can come after the prompt is printed but before the shell waits
    # for input. A shell that only interrupts read() loses it then and hangs.
    # Under valgrind, where everything is slow, that happened every time.
    shell = terminal()
    for _ in range(20):
        shell.expect(PROMPT)
        shell.sendintr()
    shell.expect(PROMPT)
    assert leave(shell) == 130


def test_ctrl_c_on_a_command_counts_once(terminal, in_front):
    # The shell gets the Ctrl+C too while the command runs. It must not
    # come back as a second Ctrl+C at the next prompt.
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline(LONG)
    in_front(shell, "sleep")
    shell.sendintr()
    shell.expect(PROMPT, timeout=3)
    try:
        extra = shell.read_nonblocking(1000, timeout=0.5)
    except pexpect.TIMEOUT:
        extra = ""
    assert extra == ""  # no second new line and prompt
    shell.sendline("true")
    shell.expect(PROMPT)
    assert leave(shell) == 0


def test_ctrl_c_drops_a_half_typed_line(terminal):
    shell = terminal()
    shell.expect(PROMPT)
    shell.send("echo abc")
    shell.sendintr()
    shell.expect(PROMPT)
    shell.sendline("echo ok")
    shell.expect("ok\r\n")
    assert "abc\r\n" not in shell.before  # echo abc never ran
    shell.expect(PROMPT)
    assert leave(shell) == 0


def test_ctrl_c_stops_the_command_not_the_shell(terminal, in_front):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline(LONG)
    in_front(shell, "sleep")
    shell.sendintr()
    shell.expect(r"\^C\r\n.*" + PROMPT, timeout=3)  # well before the 10 s sleep ends
    assert shell.isalive()
    assert leave(shell) == 130


def test_ctrl_c_stops_a_whole_pipeline(terminal, in_front):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline(f"{LONG} | cat | cat")
    in_front(shell, "sleep")
    in_front(shell, "cat", count=2)  # the status is the last cat's
    shell.sendintr()
    shell.expect(PROMPT, timeout=3)
    assert leave(shell) == 130


def test_shell_survives_sigint_sent_to_it(terminal):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline("sh -c 'kill -INT $PPID'")
    shell.expect(PROMPT)
    shell.sendline("echo alive")
    shell.expect("alive\r\n")
    assert leave(shell) == 0


def test_commands_get_the_default_sigint(terminal):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline("sh -c 'kill -INT $$; echo survived'")
    shell.expect(PROMPT)
    assert "survived" not in shell.before.split("\r\n", 1)[1]
    assert leave(shell) == 130


# ---- Ctrl+\ -----------------------------------------------------------------


def test_ctrl_backslash_stops_the_command(terminal, in_front):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline(LONG)
    in_front(shell, "sleep")
    shell.sendcontrol("\\")
    shell.expect(r"Quit.*\r\n.*" + PROMPT, timeout=3)
    assert leave(shell) == 131


def test_ctrl_backslash_at_the_prompt_is_ignored(terminal):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendcontrol("\\")
    shell.sendline("echo alive")
    shell.expect("alive\r\n")
    assert leave(shell) == 0


def test_commands_get_the_default_sigquit(terminal):
    # The shell ignores SIGQUIT, and an ignored signal survives exec, so the
    # child has to put it back itself.
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline("sh -c 'kill -QUIT $$; echo survived'")
    shell.expect("Quit")
    shell.expect(PROMPT)
    assert "survived" not in shell.before
    assert leave(shell) == 131


# ---- scripts keep the defaults ----------------------------------------------


def test_sigint_stops_a_script(run):
    result = run(stdin="sh -c 'kill -INT $PPID'\necho not here\n")
    assert result.returncode == -signal.SIGINT
    assert result.stdout == ""


def test_script_keeps_signals_that_were_ignored_before(acsh, workdir):
    # Under nohup and the like, SIGINT is ignored before the shell starts.
    # POSIX says the commands of a script keep ignoring it.
    result = subprocess.run(
        [acsh],
        input="sh -c 'kill -INT $$; echo survived'\n",
        capture_output=True,
        text=True,
        cwd=workdir,
        timeout=10,
        preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_IGN),
    )
    assert result.stdout == "survived\n"


# ---- the rest of the shell in a terminal ------------------------------------


def test_pipes_redirections_and_builtins_in_a_terminal(terminal, workdir):
    shell = terminal()
    shell.expect(PROMPT)
    shell.sendline("printf 'b\\na\\n' | sort")
    shell.expect("a\r\nb\r\n")
    shell.expect(PROMPT)
    shell.sendline("echo hi > f")
    shell.expect(PROMPT)
    shell.sendline("cat < f")
    shell.expect("hi\r\n")
    shell.expect(PROMPT)
    shell.sendline("cd /")
    shell.expect(PROMPT)
    shell.sendline("pwd")
    shell.expect("/\r\n")
    shell.expect(PROMPT)
    shell.sendline("false")
    shell.expect(PROMPT)
    assert leave(shell) == 1
    assert (workdir / "f").read_text() == "hi\n"
