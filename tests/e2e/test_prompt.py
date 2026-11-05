"""The interactive shell, in a pseudo-terminal: the prompt, exit and Ctrl+D."""

import os
import pwd
import re
import socket

import pexpect


def expected_prompt(folder):
    user = pwd.getpwuid(os.geteuid()).pw_name
    host = socket.gethostname().split(".")[0]
    mark = "#" if os.geteuid() == 0 else "$"
    return f"{user}@{host}:{folder}{mark} "


def test_prompt_shows_user_host_and_folder(terminal, workdir):
    shell = terminal(env={"HOME": "/nonexistent-home"})
    shell.expect_exact(expected_prompt(workdir))


def test_prompt_shortens_home(terminal, workdir):
    projects = workdir / "projects"
    projects.mkdir()
    shell = terminal(env={"HOME": str(workdir)}, cwd=projects)
    shell.expect_exact(expected_prompt("~/projects"))
    shell.sendline("cd")
    shell.expect_exact(expected_prompt("~"))
    shell.sendline("cd /")
    shell.expect_exact(expected_prompt("/"))


def test_prompt_matches_whole_folder_names_only(terminal, workdir):
    (workdir / "ac2").mkdir()
    shell = terminal(env={"HOME": str(workdir / "ac")}, cwd=workdir / "ac2")
    shell.expect_exact(expected_prompt(workdir / "ac2"))


def test_prompt_comes_back_after_each_command(terminal):
    shell = terminal()
    shell.expect(r"\$ $")
    shell.sendline("echo one")
    shell.expect("one\r\n")
    shell.expect(r"\$ $")
    shell.sendline("nosuchcommand")
    shell.expect("acsh: nosuchcommand: command not found\r\n")
    shell.expect(r"\$ $")


def test_exit_leaves_quietly(terminal):
    shell = terminal()
    shell.expect(r"\$ $")
    shell.sendline("exit 5")
    shell.expect(pexpect.EOF)
    shell.close()
    assert shell.exitstatus == 5
    assert "exit 5\r\n" == shell.before  # just the typed line echoed back


def test_ctrl_d_leaves_with_a_bare_newline(terminal):
    shell = terminal()
    shell.expect(r"\$ $")
    shell.sendline("false")
    shell.expect(r"\$ $")
    shell.sendeof()
    shell.expect(pexpect.EOF)
    shell.close()
    assert shell.exitstatus == 1  # the last command's status
    assert re.fullmatch(r"\r?\n", shell.before)
