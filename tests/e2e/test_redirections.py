"""Redirections: < > >> 2> 2>&1 >&- and friends."""

import os
import stat

import pytest


def read(path):
    return path.read_text()


# ---- output -----------------------------------------------------------------


def test_output_to_a_file(run, workdir):
    result = run(stdin="echo hello > out.txt\n")
    assert result.stdout == ""
    assert read(workdir / "out.txt") == "hello\n"


def test_output_replaces_the_file(run, workdir):
    (workdir / "f").write_text("old contents\n")
    run(stdin="echo new > f\n")
    assert read(workdir / "f") == "new\n"


def test_append(run, workdir):
    run(stdin="echo one >> log\necho two >> log\n")
    assert read(workdir / "log") == "one\ntwo\n"


def test_program_output_to_a_file(run, workdir):
    run(stdin="printf 'a\\nb\\n' > out\n")
    assert read(workdir / "out") == "a\nb\n"


def test_new_file_permissions_follow_the_umask(run, workdir):
    run(stdin="echo x > f\n")
    mode = stat.S_IMODE(os.stat(workdir / "f").st_mode)
    umask = os.umask(0)
    os.umask(umask)
    assert mode == 0o666 & ~umask


def test_redirection_anywhere_in_the_command(run, workdir):
    run(stdin="> a echo first\necho > b second\n")
    assert read(workdir / "a") == "first\n"
    assert read(workdir / "b") == "second\n"


def test_quoted_file_name(run, workdir):
    run(stdin="echo hi > 'my file.txt'\n")
    assert read(workdir / "my file.txt") == "hi\n"


def test_last_output_redirection_wins(run, workdir):
    run(stdin="echo hi > a > b\n")
    assert read(workdir / "a") == ""  # created and emptied, like bash
    assert read(workdir / "b") == "hi\n"


def test_redirection_without_a_command(run, workdir):
    (workdir / "f").write_text("old\n")
    result = run(stdin="> f\n> new\n")
    assert result.returncode == 0
    assert read(workdir / "f") == ""
    assert read(workdir / "new") == ""


# ---- input ------------------------------------------------------------------


def test_input_from_a_file(run, workdir):
    (workdir / "in.txt").write_text("b\na\n")
    assert run(stdin="sort < in.txt\n").stdout == "a\nb\n"


def test_input_and_output_together(run, workdir):
    (workdir / "in").write_text("hello\n")
    run(stdin="tr a-z A-Z < in > out\n")
    assert read(workdir / "out") == "HELLO\n"


def test_input_into_a_pipeline(run, workdir):
    (workdir / "in").write_text("abc\n")
    assert run(stdin="cat < in | wc -c\n").stdout.strip() == "4"


# ---- stderr and fd numbers --------------------------------------------------


def test_stderr_to_a_file(run, workdir):
    result = run(stdin="ls /nonexistent 2> err\n")
    assert result.stderr == ""
    assert "nonexistent" in read(workdir / "err")
    assert result.returncode != 0


def test_stderr_append(run, workdir):
    run(stdin="ls /nope1 2>> err\nls /nope2 2>> err\n")
    text = read(workdir / "err")
    assert "nope1" in text and "nope2" in text


def test_stderr_into_stdout(run):
    result = run(stdin="ls /nonexistent 2>&1\n")
    assert "nonexistent" in result.stdout
    assert result.stderr == ""


def test_order_of_redirections_matters(run, workdir):
    # 2>&1 first: stderr goes where stdout is now (the captured stdout),
    # then stdout moves to f.
    result = run(stdin="ls /nonexistent 2>&1 > f\n")
    assert "nonexistent" in result.stdout
    assert read(workdir / "f") == ""

    # > f first: both end up in f.
    result = run(stdin="ls /nonexistent > g 2>&1\n")
    assert result.stdout == ""
    assert "nonexistent" in read(workdir / "g")


def test_stderr_through_a_pipe(run):
    result = run(stdin="ls /nonexistent 2>&1 | wc -l\n")
    assert result.stdout.strip() == "1"
    assert result.stderr == ""


def test_builtin_output_to_stderr(run):
    result = run(stdin="echo oops >&2\n")
    assert result.stdout == ""
    assert result.stderr == "oops\n"


def test_other_fd_numbers(run, workdir):
    run(stdin="echo hi 3> f >&3\n")
    assert read(workdir / "f") == "hi\n"


def test_redirecting_fd_10_does_not_break_the_shell(run, workdir):
    # While echo runs, the shell keeps its own stdout at fd 10. Then 10> asks
    # for fd 10 too, so the shell has to move its copy out of the way first.
    result = run(stdin="echo hi > f 10> ten\necho after\n")
    assert read(workdir / "f") == "hi\n"
    assert (workdir / "ten").exists()
    assert result.stdout == "after\n"


def test_closing_stdout(run):
    result = run(stdin="echo hi >&-\n")
    assert result.stdout == ""
    assert result.stderr == "acsh: echo: write error: Bad file descriptor\n"
    assert result.returncode == 1


# ---- errors -----------------------------------------------------------------


def test_missing_input_file(run):
    result = run(stdin="cat < missing\necho after\n")
    assert result.stderr == "acsh: missing: No such file or directory\n"
    assert result.stdout == "after\n"


def test_missing_input_file_status(run):
    assert run(stdin="cat < missing\n").returncode == 1
    assert run(stdin="< missing\n").returncode == 1


def test_output_into_a_folder(run, workdir):
    (workdir / "sub").mkdir()
    result = run(stdin="echo hi > sub\n")
    assert result.stderr == "acsh: sub: Is a directory\n"
    assert result.returncode == 1


def test_output_into_a_read_only_file(run, workdir):
    (workdir / "ro").write_text("keep\n")
    (workdir / "ro").chmod(0o444)
    result = run(stdin="echo hi > ro\n")
    assert result.stderr == "acsh: ro: Permission denied\n"
    assert result.returncode == 1
    assert read(workdir / "ro") == "keep\n"


def test_output_into_a_missing_folder(run):
    result = run(stdin="echo hi > /nonexistent/f\n")
    assert result.stderr == "acsh: /nonexistent/f: No such file or directory\n"
    assert result.returncode == 1


def test_dup_from_a_closed_fd(run):
    result = run(stdin="echo hi >&7\n")
    assert result.stderr == "acsh: 7: Bad file descriptor\n"
    assert result.returncode == 1


def test_dup_target_must_be_a_number(run):
    result = run(stdin="echo hi >&file\n")
    assert result.stderr == "acsh: file: bad file descriptor\n"
    assert result.returncode == 1


def test_failed_redirection_does_not_run_the_command(run, workdir):
    result = run(stdin="touch made < missing\n")
    assert not (workdir / "made").exists()
    assert result.returncode == 1


def test_write_error_is_reported(run):
    result = run(stdin="echo hi > /dev/full\n")
    assert result.stderr == "acsh: echo: write error: No space left on device\n"
    assert result.returncode == 1


# ---- the shell's own fds come back ------------------------------------------


@pytest.mark.parametrize(
    "line",
    [
        "echo hi > f",
        "pwd > f",
        "echo hi 2> f",
        "echo hi >&2",
        "echo hi > f 2>&1",
        "echo hi >&-",
        "echo hi > /nonexistent/f",
        "cd / > f",
        "> f",
        "< missing",
    ],
)
def test_shell_output_is_back_after(run, line):
    result = run(stdin=f"{line}\necho after\nnosuchcommand\n")
    assert result.stdout.endswith("after\n")
    assert result.stderr.endswith("acsh: nosuchcommand: command not found\n")


def test_input_is_back_after_a_builtin(run, workdir):
    (workdir / "in").write_text("from file\n")
    result = run(stdin="pwd < in\ncat\nfrom stdin\n")
    assert result.stdout == f"{workdir}\nfrom stdin\n"


def test_shell_leaks_no_fds(run):
    lines = "echo hi > f\n" * 20 + "echo hi > f 10> g\n" + "cat < f > h\n"
    # The child sh lists the fds of its parent, acsh itself.
    result = run(stdin=lines + "sh -c 'ls /proc/$PPID/fd'\n")
    assert result.stdout.split() == ["0", "1", "2"]


def test_exit_with_a_redirection(run, workdir):
    result = run(stdin="exit 3 > f\necho not here\n")
    assert result.returncode == 3
    assert result.stdout == ""


def test_exit_with_a_failed_redirection_stays(run):
    result = run(stdin="exit 3 > /nonexistent/f\necho still here\n")
    assert result.stdout == "still here\n"
    assert result.returncode == 0
