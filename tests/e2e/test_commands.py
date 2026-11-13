"""Running programs: exit statuses, error messages, and reading input safely."""

import os

import pexpect
import pytest


def test_runs_a_program(run):
    result = run(stdin="printf '%s-%s\\n' a b\n")
    assert result.stdout == "a-b\n"
    assert result.stderr == ""
    assert result.returncode == 0


def test_quotes_are_removed_before_running(run):
    result = run(stdin="printf '[%s]\\n' \"a b\" 'c'd \\$e\n")
    assert result.stdout == "[a b]\n[cd]\n[$e]\n"


def test_status_at_end_of_input_is_the_last_commands(run):
    assert run(stdin="true\n").returncode == 0
    assert run(stdin="false\n").returncode == 1
    assert run(stdin="false\ntrue\n").returncode == 0
    assert run(stdin="sh -c 'exit 42'\n").returncode == 42


def test_empty_input_exits_with_zero(run):
    result = run(stdin="")
    assert result.returncode == 0
    assert result.stdout == ""
    assert result.stderr == ""


def test_empty_lines_keep_the_last_status(run):
    assert run(stdin="false\n\n   \n").returncode == 1


def test_last_line_without_newline_still_runs(run):
    assert run(stdin="echo done").stdout == "done\n"


def test_command_not_found(run):
    result = run(stdin="nosuchcommand arg\n")
    assert result.stderr == "acsh: nosuchcommand: command not found\n"
    assert result.returncode == 127


def test_missing_path(run):
    result = run(stdin="./missing\n")
    assert result.stderr == "acsh: ./missing: No such file or directory\n"
    assert result.returncode == 127


def test_permission_denied(run, workdir):
    script = workdir / "notes.sh"
    script.write_text("echo should not run\n")
    script.chmod(0o644)
    result = run(stdin="./notes.sh\n")
    assert result.stderr == "acsh: ./notes.sh: Permission denied\n"
    assert result.stdout == ""
    assert result.returncode == 126


def test_permission_denied_through_path(run, workdir):
    bin_dir = workdir / "bin"
    bin_dir.mkdir()
    (bin_dir / "tool").write_text("#!/bin/sh\necho no\n")
    (bin_dir / "tool").chmod(0o644)
    result = run(stdin="tool\n", env={"PATH": f"{bin_dir}:/usr/bin:/bin"})
    assert result.stderr == "acsh: tool: Permission denied\n"
    assert result.returncode == 126


def test_is_a_directory(run, workdir):
    (workdir / "sub").mkdir()
    result = run(stdin="./sub\n")
    assert result.stderr == "acsh: ./sub: Is a directory\n"
    assert result.returncode == 126


@pytest.mark.parametrize(
    "signal_name, number, message",
    [
        ("TERM", 15, "Terminated\n"),
        ("KILL", 9, "Killed\n"),
        ("INT", 2, ""),   # Ctrl+C stays quiet
        ("PIPE", 13, ""), # so does a closed pipe
    ],
)
def test_killed_by_a_signal(run, signal_name, number, message):
    result = run(stdin=f"sh -c 'kill -{signal_name} $$'\n")
    assert result.returncode == 128 + number
    assert result.stderr == message


def test_crash_is_reported(run):
    result = run(stdin="sh -c 'kill -SEGV $$'\n")
    assert result.returncode == 128 + 11
    assert result.stderr.startswith("Segmentation fault")


def test_shell_carries_on_after_errors(run):
    result = run(stdin="nosuchcommand\n./missing\necho still here\n")
    assert result.stdout == "still here\n"
    assert result.returncode == 0


def test_syntax_error_status_and_message(run):
    result = run(stdin="ls |\n")
    assert result.stderr == "acsh: line ends with '|'\n"
    assert result.returncode == 2

    # A script stops at a syntax error, as in bash and dash: the lines after
    # it may depend on the broken one.
    result = run(stdin="echo before\n| ls\necho after\n")
    assert result.stderr == "acsh: syntax error near '|'\n"
    assert result.stdout == "before\n"
    assert result.returncode == 2


def test_prompt_goes_on_after_a_syntax_error(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("| ls")
    shell.expect("acsh: syntax error near '\\|'")
    shell.sendline("echo $?; echo still here")
    shell.expect("2\r\nstill here")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_unclosed_quote_is_an_error(run):
    result = run(stdin="echo 'abc\n")
    assert result.stderr == "acsh: unclosed single quote\n"
    assert result.stdout == ""
    assert result.returncode == 2


def test_commands_get_the_rest_of_stdin(run):
    # acsh must not read ahead. The lines after `cat` belong to cat. (dash
    # reads ahead and runs "hello" as a command. bash gets this right.)
    assert run(stdin="cat\nhello\n").stdout == "hello\n"
    # sh's read takes exactly one line. The shell gets the next one back.
    result = run(stdin="sh -c 'read x; echo got $x'\nhello\necho after\n")
    assert result.stdout == "got hello\nafter\n"


def test_no_prompt_when_not_on_a_terminal(run):
    result = run(stdin="echo hi\nnosuchcommand\n")
    assert result.stdout == "hi\n"
    assert result.stderr == "acsh: nosuchcommand: command not found\n"


def test_children_see_the_environment(run):
    result = run(stdin="printenv ACSH_TEST\n", env={"ACSH_TEST": "visible"})
    assert result.stdout == "visible\n"


def test_long_line(run):
    words = " ".join(f"w{i}" for i in range(3000))
    result = run(stdin=f"echo {words}\n")
    assert result.stdout == words + "\n"


def test_runs_from_any_folder(run, workdir):
    other = workdir / "other"
    other.mkdir()
    assert run(stdin="pwd\n", cwd=other).stdout == f"{other}\n"
    assert os.path.isdir(other)
