"""Variables and expansion, checked against dash.

Most tests here run the same lines in acsh and in dash (Ubuntu's /bin/sh)
and compare what comes out. Each shell gets its own empty folder, so files
they create don't mix. stderr isn't compared, since every shell words its
messages differently; the acsh-only tests at the end check acsh's own.
"""

import os
import shutil
import subprocess

import pexpect
import pytest

DASH = shutil.which("dash")


def shell_run(cmd, script, cwd, env=None):
    merged = dict(os.environ)
    merged.update(env or {})
    return subprocess.run(
        cmd, input=script, capture_output=True, text=True, cwd=cwd, env=merged, timeout=10
    )


@pytest.fixture
def same_as_dash(acsh, workdir):
    """same_as_dash(script) runs it in both shells and asserts the same
    stdout and exit status. Returns acsh's result for more checks."""
    if DASH is None:
        pytest.skip("dash isn't installed")

    def _check(script, env=None):
        mine_dir = workdir / "acsh"
        theirs_dir = workdir / "dash"
        mine_dir.mkdir(exist_ok=True)
        theirs_dir.mkdir(exist_ok=True)
        mine = shell_run([acsh], script, mine_dir, env)
        theirs = shell_run([DASH], script, theirs_dir, env)
        assert mine.stdout == theirs.stdout, f"stderr: {mine.stderr!r}"
        assert mine.returncode == theirs.returncode
        return mine

    return _check


# ---- setting and reading --------------------------------------------------


def test_set_and_read(same_as_dash):
    same_as_dash("X=hello\necho $X ${X} \"$X\" '$X' \\$X \"\\$X\"\n")


def test_names_end_at_the_first_odd_character(same_as_dash):
    same_as_dash("X=a\necho $X_b ${X}_b $X-b $X.b\n")


def test_unset_variables_are_empty(same_as_dash):
    same_as_dash("printf '[%s]' $NOPE \"$NOPE\" a$NOPE\necho\n")


def test_assignments_run_left_to_right(same_as_dash):
    same_as_dash("A=1 B=$A\necho $B\n")


def test_an_assignment_can_be_empty(same_as_dash):
    same_as_dash("X=1\nX=\nprintf '[%s]' \"$X\"\necho\n")


def test_assignment_values_are_not_split(same_as_dash):
    same_as_dash("Y='a  b'\nX=$Y\nprintf '[%s]' \"$X\"\necho\n")


def test_status_and_a_lone_dollar(same_as_dash):
    same_as_dash("false\necho $? ${?}\ntrue\necho $?\necho $ \"$\" a$ $%\n")


def test_lone_assignment_sets_status_zero(same_as_dash):
    same_as_dash("false\nX=1\necho $?\n")


# ---- one command only -----------------------------------------------------


def test_assignment_before_a_program_is_for_that_program(same_as_dash):
    same_as_dash("X=1\nX=2 true\necho $X\nY=3 env | grep '^Y='\necho \"[$Y]\"\n")


def test_assignment_before_a_builtin_is_for_that_builtin(same_as_dash):
    same_as_dash("X=2 cd /\necho \"[$X]\"\nHOME=/ cd\npwd\n")


def test_words_expand_before_the_assignment(same_as_dash):
    same_as_dash("X=1\nX=2 echo $X\n")


def test_assignment_in_a_pipeline_stays_in_its_child(same_as_dash):
    same_as_dash("X=5 | cat\necho \"[$X]\"\nX=1\necho $X | cat\n")


# ---- export, unset and the environment -------------------------------------


def test_shell_variables_are_not_passed_on(same_as_dash):
    same_as_dash("X=1\nenv | grep '^X='\nexport X\nenv | grep '^X='\n")


def test_export_with_values(same_as_dash):
    same_as_dash("export X=1 Y\nenv | grep -E '^(X|Y)='\nY=2\nenv | grep '^Y='\n")


def test_export_keeps_spaces(same_as_dash):
    same_as_dash("Y='a  b'\nexport X=$Y Z=~/x\nenv | grep -E '^(X|Z)=' | sort\n", env={"HOME": "/h"})


def test_export_list_quotes_values(same_as_dash):
    same_as_dash("X=\"it's\"\nexport X\nexport -p | grep '^export X='\nexport | grep '^export X='\n")


def test_unset(same_as_dash):
    same_as_dash("X=1\nexport X\nunset X\necho \"[$X]\"\nenv | grep '^X='\nunset X NOPE\necho $?\n")


def test_special_builtins_keep_assignments(same_as_dash):
    same_as_dash("X=2 export X\necho $X\nenv | grep '^X='\nY=1\nY=2 unset Y\necho \"[$Y]\"\n")


def test_changing_path_changes_what_runs(same_as_dash, workdir):
    tool = workdir / "bin" / "hello"
    tool.parent.mkdir()
    tool.write_text("#!/bin/sh\necho mine\n")
    tool.chmod(0o755)
    same_as_dash(f"PATH={tool.parent}:$PATH\nhello\n")


def test_cd_updates_pwd_and_oldpwd(same_as_dash):
    same_as_dash("cd /\necho $PWD\ncd /tmp\necho $PWD $OLDPWD\nenv | grep -E '^(PWD|OLDPWD)=' | sort\n")


# ---- tilde -----------------------------------------------------------------


def test_tilde(same_as_dash):
    same_as_dash("echo ~ ~/a \"~\" \\~ x~ a=~ ~root\n", env={"HOME": "/home/user"})


def test_tilde_follows_home(same_as_dash):
    same_as_dash("HOME=/tmp/h\necho ~\nunset HOME\necho ~\n")


def test_tilde_in_assignments(same_as_dash):
    same_as_dash("X=~/b:~/c\necho $X\n", env={"HOME": "/h"})


# ---- field splitting -------------------------------------------------------


def test_splitting_on_spaces(same_as_dash):
    same_as_dash("X=' a  b '\nprintf '[%s]' $X \"$X\" x$X\"\"\necho\n")


def test_splitting_with_another_ifs(same_as_dash):
    same_as_dash(
        "IFS=:\nX=a::b:\nprintf '[%s]' $X\necho\nX='a b'\nprintf '[%s]' $X\necho\n"
        "IFS=': '\nX=' :a: b'\nprintf '[%s]' $X\necho\n"
    )


def test_empty_ifs_and_unset_ifs(same_as_dash):
    same_as_dash(
        "X='a b'\nIFS=\nprintf '[%s]' $X\necho\nunset IFS\nprintf '[%s]' $X\necho\n"
    )


def test_empty_quotes_keep_a_word(same_as_dash):
    same_as_dash("X=' a'\nprintf '[%s]' \"\"$X\necho\nE=\nprintf '[%s]' $E \"$E\" ''\necho\n")


def test_a_variable_can_hold_the_command(same_as_dash):
    same_as_dash("X='echo hi there'\n$X\nC=cd\n$C /\npwd\nE=\n$E\necho $?\n$E echo still\n")


# ---- what a value can't do ------------------------------------------------


def test_values_are_never_run_as_commands(same_as_dash, workdir):
    result = same_as_dash(
        "X='a; echo pwned > f'\necho $X\nY='$(touch g)'\necho $Y\nZ='| cat'\necho $Z\n"
    )
    assert "pwned\n" not in result.stdout
    assert not (workdir / "acsh" / "f").exists()
    assert not (workdir / "acsh" / "g").exists()


def test_a_value_is_expanded_once(same_as_dash):
    same_as_dash("A=secret\nB='$A'\necho $B \"$B\"\n")


def test_redirection_targets_expand_without_splitting(same_as_dash, workdir):
    same_as_dash("F='a b'\necho hi > $F\ncat \"a b\"\nD=.\necho there >> $D/\"a b\"\ncat \"$F\"\n")
    assert (workdir / "acsh" / "a b").read_text() == "hi\nthere\n"


# ---- special parameters ---------------------------------------------------


def test_dollar_dollar_is_the_shells_pid(run):
    result = run(stdin="echo $$\necho $$ | cat\nsh -c 'echo $PPID'\n")
    first, piped, parent = result.stdout.split()
    assert first.isdigit()
    assert piped == first
    assert parent == first


def test_dollar_zero_is_how_acsh_was_started(run, acsh):
    assert run(stdin="echo $0\n").stdout == f"{acsh}\n"


# ---- acsh's own messages --------------------------------------------------


@pytest.mark.parametrize(
    "line, message",
    [
        ("echo ${A B}", "acsh: missing '}' after '${'\n"),
        ("echo ${}", "acsh: ${}: bad substitution\n"),
        ("echo ${X:-a}", "acsh: ${X:-a}: bad substitution\n"),
        ("echo $(ls)", "acsh: '$(' isn't supported yet\n"),
        ("echo \"`ls`\"", "acsh: '`' isn't supported yet\n"),
    ],
)
def test_expansion_errors(run, line, message):
    result = run(stdin=f"{line}\necho $?\n")
    assert result.stderr == message
    assert result.stdout == "2\n"


def test_expansion_error_runs_nothing(run, workdir):
    result = run(stdin="echo hi > f ${}\nX=${} \necho \"[$X]\"\n")
    assert not (workdir / "f").exists()
    assert result.stdout == "[]\n"


def test_bad_variable_names(run):
    result = run(stdin="export 1x X=1 a-b\necho $? $X\nunset 2y\necho $?\n")
    assert result.stderr == (
        "acsh: export: 1x: bad variable name\n"
        "acsh: export: a-b: bad variable name\n"
        "acsh: unset: 2y: bad variable name\n"
    )
    assert result.stdout == "1 1\n1\n"


def test_unset_with_an_unknown_option(run):
    result = run(stdin="unset -f x\necho $?\n")
    assert result.stderr == "acsh: unset: -f: invalid option\n"
    assert result.stdout == "2\n"


def test_redirection_target_error(run):
    result = run(stdin="echo hi > ${}\necho $?\n")
    assert result.stderr == "acsh: ${}: bad substitution\n"
    assert result.stdout == "1\n"


def test_environment_reaches_programs_unchanged(run):
    result = run(stdin="env\n", env={"ACSH_TEST_ODD": "a b=c'd"})
    assert "ACSH_TEST_ODD=a b=c'd\n" in result.stdout


def test_prompt_follows_home(terminal, workdir):
    child = terminal(env={"HOME": "/"})
    child.expect(r"\$ ")
    child.sendline(f"HOME={workdir}")
    child.expect(r":~\$ ")
    child.sendline("exit")
    child.expect(pexpect.EOF)
