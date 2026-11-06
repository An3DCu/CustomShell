"""One test for every bug the first version of acsh had.

Each test runs the same input that showed the bug in the old version and
checks for the right result. One more lives elsewhere: the prompt showing
/home/user2 as ~2 (tests/unit/test_prompt.c).
"""

import subprocess

import pexpect


def test_cat_keeps_zero_bytes(tools, workdir):
    # Old: cat read lines into C strings, so each line stopped at its first
    # 0 byte and "ab\0cd" came out as "ab".
    (workdir / "nul.txt").write_bytes(b"ab\0cd\nsecond line\n")
    result = subprocess.run([str(tools / "cat"), "nul.txt"], capture_output=True, cwd=workdir,
                            timeout=10)
    assert result.stdout == b"ab\0cd\nsecond line\n"


def test_no_space_before_a_redirection(run, workdir):
    # Old: the word before > was never ended, so the file held "hi>out.txt".
    run(stdin="echo hi>out.txt\n")
    assert (workdir / "out.txt").read_text() == "hi\n"


def test_echo_without_newline_into_a_file(run, workdir):
    # Old: "hi" stayed in stdio's buffer and came out on the terminal after
    # stdout was put back, so the file was empty.
    result = run(stdin="echo -n hi > f.txt\necho [end]\n")
    assert (workdir / "f.txt").read_text() == "hi"
    assert result.stdout == "[end]\n"


def test_pipes(run, workdir):
    # Old: no pipes. "|", "wc" and "-l" were passed to ls.
    (workdir / "a").write_text("")
    (workdir / "b").write_text("")
    result = run(stdin="ls | wc -l\n")
    assert result.stdout.strip() == "2"
    assert result.stderr == ""


def test_exec_errors_say_what_went_wrong(run, workdir):
    # Old: all three said "command not found".
    (workdir / "notexec.sh").write_text("echo should not run\n")
    (workdir / "notexec.sh").chmod(0o644)
    result = run(stdin="./notexec.sh\n/tmp\nnosuchcommand\n")
    assert result.stderr == (
        "acsh: ./notexec.sh: Permission denied\n"
        "acsh: /tmp: Is a directory\n"
        "acsh: nosuchcommand: command not found\n"
    )


def test_unclosed_quote_is_an_error(run):
    # Old: accepted silently, with the newline swallowed into the word.
    result = run(stdin='echo "hello\n')
    assert result.stdout == ""
    assert result.stderr == "acsh: unclosed double quote\n"
    assert result.returncode == 2


def test_ctrl_c_does_not_kill_the_shell(terminal):
    # Old: Ctrl+C on a running command killed acsh itself.
    shell = terminal()
    shell.expect(r"\$ $")
    shell.sendline("sh -c 'echo started; sleep 10'")
    shell.expect("started\r\n")
    shell.sendintr()
    shell.expect(r"\$ $", timeout=3)
    assert shell.isalive()
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_piped_input_shows_no_prompt_and_prints_nothing_twice(run):
    # Old: the prompt went into the output, and after a failed command
    # everything before it came out twice: fork copied stdio's buffer and
    # the failed child flushed its copy on exit().
    result = run(stdin="echo one\nnosuchcommand\necho two\n")
    assert result.stdout == "one\ntwo\n"
    assert result.stderr == "acsh: nosuchcommand: command not found\n"
