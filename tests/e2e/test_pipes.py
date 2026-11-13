"""Pipelines: a | b | c."""

import pytest


def test_two_commands(run):
    assert run(stdin="printf 'b\\na\\n' | sort\n").stdout == "a\nb\n"


def test_three_commands(run, workdir):
    for name in ["a.c", "b.c", "c.txt"]:
        (workdir / name).write_text("")
    assert run(stdin="ls | grep c$ | wc -l\n").stdout.strip() == "2"


def test_no_spaces_around_the_pipe(run):
    assert run(stdin="echo hi|tr a-z A-Z\n").stdout == "HI\n"


def test_long_pipeline(run):
    line = "echo hi" + " | cat" * 200 + "\n"
    result = run(stdin=line, timeout=30)
    assert result.stdout == "hi\n"
    assert result.returncode == 0


@pytest.mark.not_under_valgrind("valgrind keeps file descriptors of its own open")
def test_shell_holds_no_fds_after_a_long_pipeline(run):
    # Every pipe end the shell opened for 200 commands must be closed again.
    # The child sh lists the fds of its parent, acsh itself.
    line = "echo hi" + " | cat" * 200 + "\n"
    result = run(stdin=line + "sh -c 'ls /proc/$PPID/fd'\n", timeout=30)
    assert result.stdout.split() == ["hi", "0", "1", "2"]


def test_writer_stops_when_the_reader_is_done(run):
    # yes never stops on its own. It only ends with SIGPIPE once head exits
    # and nobody holds the pipe's read end. A forgotten end hangs this test.
    result = run(stdin="yes | head -n 3\n", timeout=10)
    assert result.stdout == "y\ny\ny\n"
    assert result.stderr == ""  # SIGPIPE in a pipeline stays quiet
    assert result.returncode == 0


def test_status_is_the_last_commands(run):
    assert run(stdin="false | true\n").returncode == 0
    assert run(stdin="true | false\n").returncode == 1
    assert run(stdin="echo hi | sh -c 'exit 7'\n").returncode == 7


def test_errors_in_the_middle(run):
    result = run(stdin="nosuchcommand | echo after\n")
    assert result.stdout == "after\n"
    assert result.stderr == "acsh: nosuchcommand: command not found\n"
    assert result.returncode == 0

    result = run(stdin="echo hi | nosuchcommand\n")
    assert result.returncode == 127


def test_every_command_runs_at_the_same_time(run):
    # If the commands ran one after another, this would take 3 seconds.
    result = run(stdin="sleep 1 | sleep 1 | sleep 1\n", timeout=2.5)
    assert result.returncode == 0


def test_rest_of_stdin_goes_to_the_first_command(run):
    assert run(stdin="tr a-z A-Z | cat\nhello\n").stdout == "HELLO\n"


# A command gets stdin, stdout and stderr. None of the fds the shell keeps for
# itself reach it. That covers the pipe ends of other commands, the script the
# shell reads, and explain's copy of stderr and its report pipe. (`; true`
# keeps sh from replacing itself with ls. ls would list its own open folder.)
PROBE = "sh -c 'ls /proc/$$/fd; true'"


@pytest.mark.parametrize("where", ["in a pipeline", "in a script", "explained"])
def test_children_get_only_0_1_and_2(run, workdir, where):
    if where == "in a pipeline":
        result = run(stdin=f"echo | {PROBE} | cat\n")
    elif where == "in a script":
        (workdir / "s.sh").write_text(PROBE + "\n")
        result = run("s.sh")
    else:
        result = run(stdin=f"explain {PROBE}\n")
    assert result.stdout.split() == ["0", "1", "2"]


# ---- built-ins in a pipeline run in a child ---------------------------------


def test_builtin_output_into_a_pipe(run):
    assert run(stdin="echo hello | tr a-z A-Z\n").stdout == "HELLO\n"
    assert run(stdin="help | head -n 1\n").stdout == "acsh built-ins:\n"


def test_cd_in_a_pipeline_does_not_change_the_shell(run, workdir):
    result = run(stdin="cd / | cat\npwd\n")
    assert result.stdout == f"{workdir}\n"


def test_exit_in_a_pipeline_does_not_end_the_shell(run):
    result = run(stdin="exit 3 | cat\necho still here\n")
    assert result.stdout == "still here\n"
    assert result.returncode == 0

    result = run(stdin="echo a | exit 4\n")
    assert result.returncode == 4


@pytest.mark.parametrize("line", ["echo -n a | cat", "echo -n a | cat | cat"])
def test_builtin_output_without_newline(run, line):
    assert run(stdin=line + "\n").stdout == "a"
