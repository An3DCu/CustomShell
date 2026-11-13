"""History: the history built-in, the file, Up and Down, and Ctrl+R.

The terminal fixture points HISTFILE at a file in the test's own folder, so
the tests never touch the real ~/.acsh_history.
"""

import re

import pexpect
import pytest

PROMPT = r"acsh\$ "
LEFT, RIGHT, UP, DOWN = "\x1b[D", "\x1b[C", "\x1b[A", "\x1b[B"
BACKSPACE = "\x7f"


@pytest.fixture
def editor(terminal):
    shell = terminal(env={"TERM": "xterm", "PATH": "/usr/bin:/bin"})
    shell.expect(PROMPT)
    return shell


def lines(shell, *typed):
    """Enters each line and waits for the prompt after it. The editor draws
    "acsh$ " again on every key, so only one right after a newline counts."""
    for line in typed:
        shell.sendline(line)
        shell.expect(r"\r\n" + PROMPT)


def history_output(shell):
    """The numbered lines `history` prints. It waits for the prompt after
    them, so a slow acsh (under valgrind) can't be read halfway."""
    shell.sendline("history")
    shell.expect(r"\r\n" + PROMPT)
    numbered = re.findall(r"^ +\d+  .*$", shell.before.replace("\r", ""), re.M)
    return "".join(line + "\n" for line in numbered)


def run_keys(shell, keys):
    """Types keys, presses Enter, and returns what the command printed
    between = and =."""
    shell.send(keys + "\r")
    shell.expect(r"\r\n(=[^\r\n]*=)\r\n")
    return shell.match.group(1)


# ---- the list and the file ---------------------------------------------------


def test_history_lists_the_lines_with_numbers(prompt):
    lines(prompt, "echo a", "echo b")
    assert history_output(prompt) == "    1  echo a\n    2  echo b\n    3  history\n"


def test_a_repeated_or_empty_line_is_not_kept(prompt):
    lines(prompt, "echo a", "echo a", "", "   ", "echo b", "echo a")
    assert history_output(prompt) == ("    1  echo a\n    2  echo b\n    3  echo a\n"
                                      "    4  history\n")


def test_history_n_and_history_c(prompt, workdir):
    lines(prompt, "echo a", "echo b", "echo c")
    prompt.sendline("history 2")
    prompt.expect_exact("    3  echo c\r\n    4  history 2\r\n")
    prompt.expect(PROMPT)
    lines(prompt, "history -c")
    assert (workdir / ".acsh_history").read_text() == ""
    assert history_output(prompt) == "    1  history\n"


def test_each_line_reaches_the_file_at_once(prompt, workdir):
    lines(prompt, "echo a", "echo b")
    assert (workdir / ".acsh_history").read_text() == "echo a\necho b\n"


def test_a_new_shell_reads_the_file_and_keeps_histsize(terminal, workdir):
    (workdir / ".acsh_history").write_text("a\nb\nc\nd\ne\n")
    shell = terminal(env={"HISTSIZE": "3"})
    shell.expect(PROMPT)
    assert (workdir / ".acsh_history").read_text() == "c\nd\ne\n"  # cut back
    assert history_output(shell) == "    1  d\n    2  e\n    3  history\n"


def test_without_home_there_is_no_file(terminal, workdir):
    shell = terminal(unset=("HISTFILE", "HOME"))
    shell.expect(PROMPT)
    lines(shell, "echo a")
    assert history_output(shell) == "    1  echo a\n    2  history\n"
    assert list(workdir.iterdir()) == []


def test_histfile_set_in_the_session(prompt, workdir):
    lines(prompt, f"HISTFILE={workdir}/other", "echo a")
    assert (workdir / "other").read_text() == "echo a\n"


def test_each_line_of_a_longer_command_is_its_own_entry(prompt):
    prompt.sendline("echo '=a")
    prompt.expect("> ")
    prompt.sendline("b='")
    prompt.expect(PROMPT)
    assert history_output(prompt) == "    1  echo '=a\n    2  b='\n    3  history\n"


# ---- Up and Down -------------------------------------------------------------


def test_up_goes_back_and_down_comes_back_to_the_line_being_typed(editor):
    lines(editor, "echo =one=", "echo =two=")
    assert run_keys(editor, UP + UP) == "=one="
    assert run_keys(editor, "echo =dr" + UP + UP + DOWN + DOWN + "aft=") == "=draft="


def test_a_change_to_an_old_line_isnt_kept(editor):
    lines(editor, "echo =one=", "echo =two=")
    assert run_keys(editor, UP + BACKSPACE + "X=" + DOWN + UP) == "=two="


# ---- Ctrl+R ------------------------------------------------------------------


def test_ctrl_r_finds_and_ctrl_r_again_goes_further_back(editor):
    lines(editor, "echo =alpha=", "echo =beta=", "echo =alpine=")
    editor.send("\x12alp")
    editor.expect_exact("search: alp  ->  echo =alpine=")
    editor.send("\x12")
    editor.expect_exact("search: alp  ->  echo =alpha=")
    assert run_keys(editor, "") == "=alpha="


def test_ctrl_r_without_a_match_and_ctrl_g(editor):
    lines(editor, "echo =alpha=")
    editor.send("echo =kept=" + "\x12zzz")
    editor.expect_exact("search: zzz  (no match)")
    assert run_keys(editor, "\x07") == "=kept="  # Ctrl+G: the line as it was


def test_an_arrow_key_keeps_the_match_for_editing(editor):
    lines(editor, "echo =beta=")
    assert run_keys(editor, "\x12bet" + RIGHT + BACKSPACE + "2=") == "=beta2="


def test_ctrl_c_in_a_search_drops_the_line(editor):
    lines(editor, "echo =beta=")
    editor.send("\x12bet\x03")
    editor.expect_exact("^C\r\n")
    editor.expect(PROMPT)
    assert run_keys(editor, "echo =$?=") == "=130="
