"""if, while, until and for, break and continue, read, true, false and :.

Most cases run in acsh and in dash and compare stdout and the exit status.
acsh has no $((...)) yet. The loops count with strings (n=${n}x) or read
lines from a file.
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
        "if true; then echo y; fi; echo $?\n",
        "if false; then echo y; fi; echo $?\n",
        "if false; then echo y; else echo n; fi\n",
        "if false; then :; elif true; then echo elif; fi\n",
        "if false; then :; elif false; then :; else echo else; fi\n",
        "if true; then false; fi; echo $?\n",
        "if false; then :; else false; fi; echo $?\n",
        "if ! false; then echo negated; fi\n",
        "if true && false; then echo no; else echo and-or; fi\n",
        "if true; then if false; then :; else echo inner; fi; fi\n",
        "if\ntrue\nthen\necho multi\nfi\n",
        "if true; then echo a; fi | tr a b\n",
        "if true; then echo a; fi && echo b\n",
        "! if false; then :; fi; echo $?\n",
        "X=1; if [ $X = 1 ]; then echo one; fi\n",
        "if false; then echo; fi > out; cat out; echo st $?\n",
    ],
)
def test_if(same_as_dash, script):
    same_as_dash(script)


@pytest.mark.parametrize(
    "script",
    [
        "n=; while [ \"$n\" != xxx ]; do n=${n}x; echo $n; done\n",
        "n=; until [ \"$n\" = xxx ]; do n=${n}x; echo $n; done\n",
        "while false; do echo no; done; echo $?\n",
        "true; while false; do :; done; echo $?\n",
        "while true; do break; done; echo $?\n",
        "while :; do echo once; break; done\n",
        "for x in a b c; do echo $x; done\n",
        "for x in a \"b c\" *.nope; do echo \"[$x]\"; done\n",
        "for x in; do echo $x; done; echo $?\n",
        "false; for x in; do :; done; echo $?\n",
        "X=1; for X in a; do :; done; echo $X\n",
        "for x in a b; do echo $x; done; echo $x\n",
        "Y='p q'; for x in $Y \"$Y\"; do echo \"[$x]\"; done\n",
        "for x in do done; do echo $x; done\n",
        "for x\ndo echo $x; done; echo none\n",
        "for x in a b c; do if [ $x = b ]; then break; fi; echo $x; done; echo st $?\n",
        "for x in a b c; do if [ $x = b ]; then continue; fi; echo $x; done\n",
        "for i in 1 2; do for j in a b; do if [ $j = b ]; then continue 2; fi; echo $i$j; done; echo never; done\n",
        "for i in 1 2; do for j in a b; do break 2; done; echo never; done; echo out\n",
        "for i in 1 2; do break 5; done; echo after\n",
        "break; echo after $?\n",
        "continue; echo after $?\n",
        "for x in a b; do echo $x; done > out; cat out\n",
        "for x in b a c; do echo $x; done | sort\n",
        "for x in 1 2 3; do echo $x; done | while read l; do echo got $l; done\n",
        "for x in a b; do for y in 1 2; do echo $x$y; done; done\n",
        "for x in a; do false; done; echo $?\n",
        "while true; do while true; do break 2; done; done; echo out\n",
        "for x in a b c; do echo $x; [ $x = b ] && break; done\n",
    ],
)
def test_loops(same_as_dash, script):
    same_as_dash(script)


def test_loop_reads_a_file_from_a_redirection(same_as_dash):
    same_as_dash(
        "printf '1\\n2\\n3\\n' > f\nwhile read l; do echo got $l; done < f\n"
        "read first < f; echo still $first\n"
    )


def test_for_with_the_scripts_arguments(acsh, workdir):
    for shell in filter(None, (acsh, DASH)):
        result = subprocess.run([shell, "-c", "for a; do echo \"[$a]\"; done; for a do shift; done; echo $#",
                                 "n", "x", "y z"], capture_output=True, text=True, cwd=workdir)
        assert result.stdout == "[x]\n[y z]\n0\n"  # the loop shifted twice. Its list was a copy.


def test_for_words_are_expanded_once(run, workdir):
    (workdir / "a.c").touch()
    result = run(stdin="for f in *.c; do touch new.c; echo $f; done\n")
    assert result.stdout == "a.c\n"


def test_a_long_loop(run, workdir):
    (workdir / "lines").write_text("".join(f"{i}\n" for i in range(3000)))
    result = run(stdin="n=0\nwhile read l; do n=$l; done < lines\necho $n\n")
    assert result.stdout == "2999\n"


# ---- read ----------------------------------------------------------------------


@pytest.mark.parametrize(
    "line, names, ifs",
    [
        ("  a  b  c  ", "x y", None),
        ("a b", "x y z", None),
        ("a", "x", None),
        ("", "x", None),
        ("a:b:c", "x y", ":"),
        ("a:b:", "x y", ":"),
        ("a:b::", "x y", ":"),
        ("a::b", "x y z", ":"),
        (" a : b ", "x y", ": "),
        ("a\\\\ b c", "x y", None),
        ("a\\\\:b:c", "x y", ":"),
    ],
)
def test_read_splits_like_dash(same_as_dash, line, names, ifs):
    setifs = f"IFS='{ifs}'\n" if ifs is not None else ""
    shown = "".join(f"[${n}]" for n in names.split())
    same_as_dash(f"printf '%s\\n' '{line}' > f\n{setifs}read {names} < f\necho \"$? {shown}\"\n")


def test_read_raw_and_joined_lines(same_as_dash):
    same_as_dash(
        "printf 'a\\\\ b c\\n' > f\nread -r x y < f\necho \"[$x][$y]\"\n"
        "printf 'a\\\\\\nb\\n' > g\nread x < g\necho \"[$x]\"\nread -r x < g\necho \"[$x]\"\n"
    )


def test_read_at_the_end(same_as_dash):
    same_as_dash(
        "printf 'no newline' > f\nread x < f\necho \"$? [$x]\"\n"
        ": > e\nread x < e\necho \"$? [$x]\"\n"
    )


def test_read_leaves_the_rest_of_stdin(run):
    result = run(stdin="read a\nfirst\nread b\nsecond\necho $a $b\ncat\nrest\n")
    assert result.stdout == "first second\nrest\n"


def test_read_without_names_uses_reply(run):
    result = run(stdin="read\n  keep  spaces  \necho \"[$REPLY]\"\n")
    assert result.stdout == "[  keep  spaces  ]\n"


def test_read_errors(run):
    result = run(stdin="read 1x\necho $?\nread -p x\necho $?\n")
    assert result.stderr == (
        "acsh: read: 1x: bad variable name\n"
        "acsh: read: -p: invalid option\n"
    )
    assert result.stdout == "2\n2\n"


# ---- errors and other built-ins --------------------------------------------------


def test_true_false_and_colon(same_as_dash):
    same_as_dash("true; echo $?\nfalse; echo $?\n: a b c; echo $?\n")


def test_loop_count_errors(run):
    result = run(stdin="for i in 1; do break 0; done; echo $?\nfor i in 1; do continue x; done; echo $?\n")
    assert result.stderr == (
        "acsh: break: 0: loop count out of range\n"
        "acsh: continue: x: loop count out of range\n"
    )
    assert result.stdout == "1\n1\n"


@pytest.mark.parametrize(
    "script, message",
    [
        ("fi\n", "acsh: syntax error near 'fi'\n"),
        ("if true; then fi\n", "acsh: syntax error near 'fi'\n"),
        ("if true then echo y; fi\n", "acsh: syntax error near 'fi'\n"),
        ("while true; done\n", "acsh: syntax error near 'done'\n"),
        ("for 1x in a; do :; done\n", "acsh: '1x' can't be a loop variable\n"),
        ("if true; then echo a; fi echo b\n", "acsh: syntax error near 'echo'\n"),
    ],
)
def test_syntax_errors(run, script, message):
    result = run(stdin=f"echo before\n{script}echo after\n")
    assert result.stderr == message
    assert result.stdout == "before\n"
    assert result.returncode == 2


@pytest.mark.parametrize(
    "script, message",
    [
        ("if true; then echo a\n", "acsh: missing 'fi'\n"),
        ("while true\n", "acsh: missing 'do'\n"),
        ("for x in a; do echo\n", "acsh: missing 'done'\n"),
    ],
)
def test_script_ends_inside_a_block(run, script, message):
    result = run(stdin=script)
    assert result.stderr == message
    assert result.returncode == 2


def test_help_lists_the_new_builtins(run):
    result = run(stdin="help\n")
    for name in [":", "break", "continue", "false", "read", "true"]:
        assert f"\n  {name} " in result.stdout


# ---- in a terminal -------------------------------------------------------------


def test_block_over_several_lines_at_the_prompt(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("for x in a b")
    shell.expect("> ")
    shell.sendline("do")
    shell.expect("> ")
    shell.sendline("echo got-$x")
    shell.expect("> ")
    shell.sendline("done")
    shell.expect("got-a\r\ngot-b\r\n")
    shell.expect(r"\$ ")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_ctrl_c_stops_a_loop_of_builtins(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("while :; do :; done; echo after")
    shell.expect("echo after\r\n")
    time.sleep(0.5)
    shell.sendintr()
    shell.expect(r"\$ ")
    shell.sendline("echo status $?")
    shell.expect("status 130")
    assert "after" not in shell.before
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_ctrl_c_stops_a_loop_of_programs(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("while true; do sleep 1; done")
    shell.expect("done\r\n")
    time.sleep(0.5)
    shell.sendintr()
    shell.expect(r"\$ ")
    shell.sendline("echo status $?")
    shell.expect("status 130")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)


def test_ctrl_c_stops_read(terminal):
    shell = terminal()
    shell.expect(r"\$ ")
    shell.sendline("read x; echo after")
    shell.expect("echo after\r\n")
    time.sleep(0.3)
    shell.sendintr()
    shell.expect(r"\$ ")
    shell.sendline("echo status $?")
    shell.expect("status 130")
    assert "after" not in shell.before
    shell.sendline("exit")
    shell.expect(pexpect.EOF)
