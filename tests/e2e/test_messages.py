"""Where errors say they happened, the hints under some of them, and the
prompt's colors."""

import subprocess

import pexpect
import pytest


def write(path, text, mode=0o644):
    path.write_text(text)
    path.chmod(mode)
    return path


# ---- file and line in scripts ----------------------------------------------------


def test_errors_in_a_script_say_where(acsh, workdir):
    write(workdir / "s.sh",
          "echo one\nnope\nfor x in a b; do\n  echo $x\n  nope2 $x\ndone\ncd /nonexistent\n")
    result = subprocess.run([acsh, "s.sh"], capture_output=True, text=True, cwd=workdir)
    assert result.stderr == (
        "acsh: s.sh: line 2: nope: command not found\n"
        "acsh: s.sh: line 5: nope2: command not found\n"
        "acsh: s.sh: line 5: nope2: command not found\n"
        "acsh: s.sh: line 7: cd: /nonexistent: No such file or directory\n"
    )


def test_each_command_of_a_line_has_that_line(acsh, workdir):
    write(workdir / "s.sh", "\n\necho a | nope_a; nope_b\n")
    result = subprocess.run([acsh, "s.sh"], capture_output=True, text=True, cwd=workdir)
    assert sorted(result.stderr.splitlines()) == [
        "acsh: s.sh: line 3: nope_a: command not found",
        "acsh: s.sh: line 3: nope_b: command not found",
    ]


def test_syntax_error_in_a_script_says_where(acsh, workdir):
    write(workdir / "s.sh", "echo one\n\necho x |\n| y\n")
    result = subprocess.run([acsh, "s.sh"], capture_output=True, text=True, cwd=workdir)
    assert result.stderr == "acsh: s.sh: line 4: syntax error near '|'\n"


def test_dash_c_errors_say_where(run):
    assert run("-c", "echo a; nope").stderr == "acsh: -c: line 1: nope: command not found\n"
    assert run("-c", "true\nnope", "name").stderr == "acsh: name: line 2: nope: command not found\n"


def test_sourced_file_errors_say_where_and_then_stop_saying(run, workdir):
    write(workdir / "lib.sh", "echo in lib\n\nnope3\n")
    result = run(stdin=". ./lib.sh\nnope4\n")
    assert result.stderr == (
        "acsh: ./lib.sh: line 3: nope3: command not found\n"
        "acsh: nope4: command not found\n"
    )


def test_errors_from_stdin_stay_short(run):
    result = run(stdin="\n\nnope\n")
    assert result.stderr == "acsh: nope: command not found\n"


def test_redirection_and_expansion_errors_say_where(acsh, workdir):
    write(workdir / "s.sh", "cat < missing\necho ${}\n")
    result = subprocess.run([acsh, "s.sh"], capture_output=True, text=True, cwd=workdir)
    assert result.stderr == (
        "acsh: s.sh: line 1: missing: No such file or directory\n"
        "acsh: s.sh: line 2: ${}: bad substitution\n"
    )


# ---- hints -----------------------------------------------------------------------


@pytest.fixture
def prompt(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    yield shell
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_hint_for_a_file_in_this_folder(prompt, workdir):
    write(workdir / "build.sh", "echo built\n", 0o755)
    prompt.sendline("build.sh")
    prompt.expect("acsh: build.sh: command not found\r\n  hint: it's in this folder; run it as ./build.sh\r\n")
    prompt.expect(r"\$ ")


def test_hint_for_a_file_that_isnt_executable(prompt, workdir):
    write(workdir / "run.sh", "echo ran\n", 0o644)
    prompt.sendline("./run.sh")
    prompt.expect_exact("acsh: ./run.sh: Permission denied\r\n"
                        "  hint: it isn't executable; chmod +x ./run.sh, or run it with acsh ./run.sh\r\n")


@pytest.mark.parametrize("line, fixed", [("X = 1", "X=1"), ("X =1", "X=1"), ("NAME = a b", "NAME=a")])
def test_hint_for_spaces_around_equals(prompt, line, fixed):
    name = line.split()[0]
    prompt.sendline(line)
    prompt.expect_exact(f"acsh: {name}: command not found\r\n"
                        f"  hint: no spaces around = in an assignment: {fixed}\r\n")


def test_hint_for_a_lonely_pipe(prompt):
    prompt.sendline("| grep x")
    prompt.expect_exact("acsh: syntax error near '|'\r\n  hint: a | needs a command on both sides\r\n")


def test_no_hint_for_other_errors(prompt):
    prompt.sendline("nope_nope")
    prompt.expect_exact("acsh: nope_nope: command not found\r\n")
    prompt.sendline("echo next")
    prompt.expect("next")
    assert "hint:" not in prompt.before  # "hint" alone is in this test's folder name


def test_no_hints_in_scripts(run, workdir):
    write(workdir / "build.sh", "echo built\n", 0o755)
    result = run(stdin="build.sh\nX = 1\n| x\n")
    assert "hint:" not in result.stderr


# ---- prompt colors ---------------------------------------------------------------


def test_prompt_in_color_on_a_color_terminal(terminal):
    shell = terminal(env={"TERM": "xterm"})
    shell.expect_exact("\x1b[1;32m")
    shell.expect_exact("\x1b[0m \x1b[1;34m")
    shell.expect_exact("\x1b[0m\r\nacsh")
    shell.sendline("false")
    shell.expect_exact("  \x1b[31mexit 1\x1b[0m\r\nacsh")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


@pytest.mark.parametrize("env", [{"TERM": "xterm", "NO_COLOR": "1"}, {"TERM": "dumb"}])
def test_no_colors(terminal, env):
    shell = terminal(env=env)
    shell.expect(r"acsh\$ ")
    assert "\x1b" not in shell.before
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_no_colors_without_term(terminal):
    shell = terminal(unset=["TERM"], env={"TERM": ""})
    shell.expect(r"acsh\$ ")
    assert "\x1b" not in shell.before
    shell.sendline("exit")
    shell.expect(pexpect.EOF)
