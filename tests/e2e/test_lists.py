"""Lists (;, &&, ||, newlines), !, comments and lines that go on.

As in the variable tests, most of these run in acsh and dash and compare
stdout and the exit status.
"""

import os
import shutil
import subprocess
import time

import pexpect
import pytest

DASH = shutil.which("dash")


@pytest.fixture
def same_as_dash(acsh, workdir):
    if DASH is None:
        pytest.skip("dash isn't installed")

    def _check(script):
        results = []
        for shell in (acsh, DASH):
            folder = workdir / os.path.basename(shell)
            folder.mkdir(exist_ok=True)
            results.append(
                subprocess.run([shell], input=script, capture_output=True, text=True,
                               cwd=folder, timeout=10)
            )
        mine, theirs = results
        assert mine.stdout == theirs.stdout, f"stderr: {mine.stderr!r}"
        assert mine.returncode == theirs.returncode
        return mine

    return _check


@pytest.mark.parametrize(
    "script",
    [
        "echo a; echo b\n",
        "echo a;echo b;\n",
        "false; echo $?\n",
        "true && echo yes\n",
        "false && echo no\n",
        "false || echo yes\n",
        "true || echo no\n",
        "false && echo no || echo yes\n",
        "true || echo no && echo yes\n",
        "false || false || echo third\n",
        "true && false && echo no; echo $?\n",
        "false && echo no; echo $?\n",
        "! false; echo $?\n",
        "! true; echo $?\n",
        "! false && echo yes\n",
        "! echo hi | grep -q x; echo $?\n",
        "echo a | tr a b && echo c | tr c d\n",
        "X=1; echo $X; X=2 && echo $X\n",
        "cd /tmp && pwd; cd / || echo no; pwd\n",
        "exit 3; echo no\n",
        "true && exit 4 || echo no\n",
    ],
)
def test_lists(same_as_dash, script):
    same_as_dash(script)


@pytest.mark.parametrize(
    "script",
    [
        "echo a # comment; echo b\n",
        "echo a#b #c\n",
        "echo '#x' \"#y\" \\#z\n",
        "# only a comment\necho after\n",
        "   # indented\necho after\n",
        "echo a # it's fine\n",
    ],
)
def test_comments(same_as_dash, script):
    same_as_dash(script)


@pytest.mark.parametrize(
    "script",
    [
        "echo a \\\nb\n",
        "ec\\\nho joined\n",
        "echo \"a\nb\"\n",
        "echo 'a\nb'\n",
        "echo a |\ncat\n",
        "echo a &&\n\necho b\n",
        "false ||\necho b\n",
        "printf '%s\\n' one \\\n  two \\\n  three\n",
    ],
)
def test_commands_over_several_lines(same_as_dash, script):
    same_as_dash(script)


def test_empty_lines_keep_the_status(same_as_dash):
    same_as_dash("false\n\n\n# x\necho $?\n")


@pytest.mark.parametrize(
    "script, message",
    [
        ("; echo a\n", "acsh: syntax error near ';'\n"),
        ("echo a;; echo b\n", "acsh: syntax error near ';'\n"),
        ("echo a && && echo b\n", "acsh: syntax error near '&&'\n"),
        ("echo a | ; echo b\n", "acsh: syntax error near ';'\n"),
        ("echo a )\n", "acsh: syntax error near ')'\n"),
        ("(echo a)\n", "acsh: '(' isn't supported yet\n"),
        ("sleep 1 &\n", "acsh: '&' isn't supported yet\n"),
    ],
)
def test_syntax_errors_stop_a_script(run, script, message):
    result = run(stdin=f"echo before\n{script}echo after\n")
    assert result.stderr == message
    assert result.stdout == "before\n"
    assert result.returncode == 2


@pytest.mark.parametrize(
    "script, message",
    [
        ("echo a ||\n", "acsh: line ends with '||'\n"),
        ("echo a &&\n\n", "acsh: line ends with '&&'\n"),
        ("echo a |", "acsh: line ends with '|'\n"),
        ("echo \"a\nb\n", "acsh: unclosed double quote\n"),
        ("echo $(ls\n", "acsh: unclosed '$('\n"),
    ],
)
def test_script_ending_in_the_middle_of_a_command(run, script, message):
    result = run(stdin=f"echo before\n{script}")
    assert result.stderr == message
    assert result.stdout == "before\n"
    assert result.returncode == 2


def test_dollar_paren_is_one_word(run):
    result = run(stdin="echo $(ls -l | wc)\necho $?\n")
    assert result.stderr == "acsh: '$(' isn't supported yet\n"
    assert result.stdout == "2\n"


def test_expansion_errors_do_not_stop_a_script(run):
    result = run(stdin="echo ${}; echo same line\necho next line\n")
    assert result.stdout == "same line\nnext line\n"
    assert result.returncode == 0


def test_rest_of_stdin_still_goes_to_commands(run):
    result = run(stdin="cat; echo done\nhello\n")
    assert result.stdout == "hello\ndone\n"


def test_continuation_prompt(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("echo 'a")
    shell.expect("> ")
    shell.sendline("b' |")
    shell.expect("> ")
    shell.sendline("tr ab AB")
    shell.expect("A\r\nB\r\n")
    shell.expect(r"\$ ")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_ctrl_c_drops_an_unfinished_command(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("echo 'never")
    shell.expect("> ")
    shell.sendintr()
    shell.expect(r"\$ ")
    shell.sendline("echo $?")
    shell.expect("130")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_ctrl_c_stops_the_rest_of_a_list(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("sleep 10; echo after")
    shell.expect("sleep 10; echo after\r\n")
    time.sleep(0.5)  # let sleep start
    shell.sendintr()
    shell.expect(r"\$ ")
    shell.sendline("echo status $?")
    shell.expect("status 130")
    assert "after" not in shell.before
    shell.sendline("exit")
    shell.expect(pexpect.EOF)
