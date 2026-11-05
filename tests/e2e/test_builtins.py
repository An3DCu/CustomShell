"""The built-ins: cd, pwd, echo, exit and help."""

import pytest

# ---- cd and pwd -------------------------------------------------------------


def test_pwd(run, workdir):
    assert run(stdin="pwd\n").stdout == f"{workdir}\n"


def test_cd_and_pwd(run, workdir):
    (workdir / "sub").mkdir()
    result = run(stdin="cd sub\npwd\ncd ..\npwd\n")
    assert result.stdout == f"{workdir / 'sub'}\n{workdir}\n"
    assert result.returncode == 0


def test_cd_absolute(run):
    assert run(stdin="cd /\npwd\n").stdout == "/\n"


def test_cd_changes_the_folder_for_programs(run, workdir):
    (workdir / "sub").mkdir()
    assert run(stdin="cd sub\n/bin/pwd\n").stdout == f"{workdir / 'sub'}\n"


def test_cd_without_argument_goes_home(run, workdir):
    home = workdir / "home"
    home.mkdir()
    assert run(stdin="cd\npwd\n", env={"HOME": str(home)}).stdout == f"{home}\n"


def test_cd_without_home(run):
    result = run(stdin="cd\n", unset=["HOME"])
    assert result.stderr == "acsh: cd: HOME not set\n"
    assert result.returncode == 1


def test_cd_dash_goes_back_and_says_where(run, workdir):
    (workdir / "sub").mkdir()
    result = run(stdin="cd sub\ncd -\npwd\n", unset=["OLDPWD"])
    assert result.stdout == f"{workdir}\n{workdir}\n"


def test_cd_dash_without_oldpwd(run):
    result = run(stdin="cd -\n", unset=["OLDPWD"])
    assert result.stderr == "acsh: cd: OLDPWD not set\n"
    assert result.returncode == 1


def test_cd_updates_pwd_and_oldpwd(run, workdir):
    (workdir / "sub").mkdir()
    result = run(stdin="cd sub\nprintenv PWD OLDPWD\n")
    assert result.stdout == f"{workdir / 'sub'}\n{workdir}\n"


def test_cd_errors(run, workdir):
    (workdir / "file").write_text("")
    result = run(stdin="cd /nonexistent\n")
    assert result.stderr == "acsh: cd: /nonexistent: No such file or directory\n"
    assert result.returncode == 1

    result = run(stdin="cd file\n")
    assert result.stderr == "acsh: cd: file: Not a directory\n"

    result = run(stdin="cd a b\n")
    assert result.stderr == "acsh: cd: too many arguments\n"
    assert result.returncode == 1


def test_failed_cd_stays_put(run, workdir):
    assert run(stdin="cd /nonexistent\npwd\n").stdout == f"{workdir}\n"


# ---- echo -------------------------------------------------------------------
# Each expected output was also checked against bash's own echo.

ECHO_CASES = [
    ("echo hello world", "hello world\n"),
    ("echo", "\n"),
    ("echo 'a  b'", "a  b\n"),
    ("echo -n hi", "hi"),
    ("echo -n", ""),
    ("echo 'a\\tb'", "a\\tb\n"),
    ("echo -e 'a\\tb'", "a\tb\n"),
    ("echo -e 'a\\nb'", "a\nb\n"),
    ("echo -e -E 'a\\tb'", "a\\tb\n"),
    ("echo -nE x", "x"),
    ("echo -en 'x\\n'", "x\n"),
    ("echo -x", "-x\n"),
    ("echo - a", "- a\n"),
    ("echo -- a", "-- a\n"),
    ("echo hi -n", "hi -n\n"),
    ("echo -e 'a\\cb' c", "a"),
    ("echo -e '\\0101\\x41\\x4a'", "AAJ\n"),
    ("echo -e '\\\\'", "\\\n"),
    ("echo -e '\\q'", "\\q\n"),
    ("echo -e 'x\\x'", "x\\x\n"),
    ("echo -e '\\e[0m'", "\x1b[0m\n"),
]


@pytest.mark.parametrize("line, want", ECHO_CASES)
def test_echo(run, line, want):
    result = run(stdin=line + "\n")
    assert result.stdout == want
    assert result.returncode == 0


def test_echo_without_newline_comes_out_right_away(run):
    # Built-ins flush their output, so it can't end up after later output.
    assert run(stdin="echo -n a\n/bin/echo b\necho -n c\n").stdout == "ab\nc"


# ---- exit -------------------------------------------------------------------


@pytest.mark.parametrize(
    "line, status",
    [("exit 3", 3), ("exit 0", 0), ("exit 255", 255), ("exit 256", 0), ("exit -1", 255), ("exit +7", 7)],
)
def test_exit_with_a_status(run, line, status):
    result = run(stdin=f"{line}\necho not here\n")
    assert result.returncode == status
    assert result.stdout == ""
    assert result.stderr == ""


def test_exit_without_argument_uses_the_last_status(run):
    assert run(stdin="false\nexit\n").returncode == 1
    assert run(stdin="true\nexit\n").returncode == 0


@pytest.mark.parametrize("arg", ["abc", "3x", "''", "99999999999999999999"])
def test_exit_needs_a_number(run, arg):
    result = run(stdin=f"exit {arg}\necho not here\n")
    shown = "" if arg == "''" else arg
    assert result.stderr == f"acsh: exit: {shown}: numeric argument required\n"
    assert result.stdout == ""
    assert result.returncode == 2


def test_exit_with_too_many_arguments_stays(run):
    result = run(stdin="exit 1 2\necho still here\n")
    assert result.stderr == "acsh: exit: too many arguments\n"
    assert result.stdout == "still here\n"
    assert result.returncode == 0


# ---- help -------------------------------------------------------------------


def test_help_lists_every_builtin(run):
    result = run(stdin="help\n")
    assert result.returncode == 0
    for name in ["cd", "echo", "exit", "help", "pwd"]:
        assert f"\n  {name}" in result.stdout


def test_help_for_one_builtin(run):
    result = run(stdin="help cd\n")
    assert result.stdout.startswith("  cd [dir | -]")
    assert result.stdout.count("\n") == 1


def test_help_for_something_unknown(run):
    result = run(stdin="help nope\n")
    assert result.stderr == "acsh: help: no help for 'nope'\n"
    assert result.returncode == 1
