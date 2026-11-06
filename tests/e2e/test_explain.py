"""Explain mode: the plan, the system calls, and the status.

pids change on every run, so before comparing, each pid is replaced by P1,
P2, ... in the order it first shows up, and any run of spaces inside a line
becomes two (a pid's length moves the notes' column). PATH is fixed, so the
"found in PATH" notes are the same everywhere.
"""

import re
import textwrap

import pexpect
import pytest

ENV = {"PATH": "/usr/bin:/bin"}


def normalize(text):
    pids = {}

    def name(match):
        pid = match.group(0)
        if pid not in pids:
            pids[pid] = f"P{len(pids) + 1}"
        return pids[pid]

    text = re.sub(r"(?<=fork\(\) = )\d+|(?<=waitpid\()\d+|(?<=\[)\d+(?=\])", name, text)
    return sq(text)


def sq(text):
    """Runs of spaces inside each line become two; the indent stays."""
    lines = []
    for line in text.split("\n"):
        indent = len(line) - len(line.lstrip(" "))
        lines.append(line[:indent] + re.sub(r" {2,}", "  ", line[indent:]))
    return "\n".join(lines)


def explained(run, line):
    result = run(stdin=line + "\n", env=ENV)
    return normalize(result.stderr), result


def block(text):
    return sq(textwrap.dedent(text).lstrip("\n"))


def test_pipeline_with_a_redirection(run, workdir):
    err, result = explained(run, "explain printf 'a\\nb\\n' | tr a-z A-Z > out")
    assert err == block("""
        plan
          pipeline of 2
          ├─ printf 'a\\nb\\n'
          └─ tr a-z A-Z  stdout > out
        run
          pipe() = [3, 4]                          what goes into 4 comes out of 3
          fork() = P1                              a child for printf
          [P1] dup2(4, 1)                          stdout goes into the pipe
          [P1] expand: printf 'a\\nb\\n'  ->  printf a\\nb\\n
          [P1] execvp(printf a\\nb\\n)               becomes /usr/bin/printf, found in PATH
          close(4)                                 the shell lets go of its end
          fork() = P2                              a child for tr
          [P2] dup2(3, 0)                          stdin comes from the pipe
          [P2] open("out", O_WRONLY|O_CREAT|O_TRUNC) = 3
          [P2] dup2(3, 1)                          stdout goes to out
          [P2] close(3)
          [P2] execvp(tr a-z A-Z)                  becomes /usr/bin/tr, found in PATH
          close(3)                                 the shell lets go of its end
          waitpid(P1) = exit 0
          waitpid(P2) = exit 0
        status 0
        """)
    assert (workdir / "out").read_text() == "A\nB\n"
    assert result.stdout == ""


def test_builtin_saves_and_restores_its_fds(run, workdir):
    err, _ = explained(run, "explain echo hi > f 2>&1")
    assert err == block("""
        plan
          echo hi  stdout > f  stderr >& 1
        run
          built-in echo                            no fork: it runs in the shell itself
          fcntl(1, F_DUPFD_CLOEXEC, 10) = 11       keep stdout, to put it back after
          open("f", O_WRONLY|O_CREAT|O_TRUNC) = 3
          dup2(3, 1)                               stdout goes to f
          close(3)
          fcntl(2, F_DUPFD_CLOEXEC, 10) = 12       keep stderr, to put it back after
          dup2(1, 2)                               stderr goes where stdout goes
          dup2(12, 2)                              stderr back as it was
          close(12)
          dup2(11, 1)                              stdout back as it was
          close(11)
        status 0
        """)
    assert (workdir / "f").read_text() == "hi\n"


def test_assignment_for_one_program(run):
    err, _ = explained(run, "explain X=1 env > /dev/null")
    assert err == block("""
        plan
          X=1 env  stdout > /dev/null
        run
          fork() = P1                              a child for env
          [P1] open("/dev/null", O_WRONLY|O_CREAT|O_TRUNC) = 3
          [P1] dup2(3, 1)                          stdout goes to /dev/null
          [P1] close(3)
          [P1] set X=1                             only in this command's environment
          [P1] execvp(env)                         becomes /usr/bin/env, found in PATH
          waitpid(P1) = exit 0
        status 0
        """)


def test_for_loop_with_a_redirection(run):
    err, _ = explained(run, "explain for x in a b; do echo $x; done > g")
    assert err == block("""
        plan
          for x in a b  stdout > g
          └─ do
             └─ echo $x
        run
          fcntl(1, F_DUPFD_CLOEXEC, 10) = 11       keep stdout, to put it back after
          open("g", O_WRONLY|O_CREAT|O_TRUNC) = 3
          dup2(3, 1)                               stdout goes to g
          close(3)
          for: x=a, round 1 of 2
          expand: echo $x  ->  echo a
          built-in echo                            no fork: it runs in the shell itself
          for: x=b, round 2 of 2
          expand: echo $x  ->  echo b
          built-in echo                            no fork: it runs in the shell itself
          dup2(11, 1)                              stdout back as it was
          close(11)
        status 0
        """)


def test_if_and_elif(run):
    err, result = explained(run, "explain if false; then :; elif true; then echo y; fi")
    assert err == block("""
        plan
          if
          ├─ condition
          │  └─ false
          ├─ then
          │  └─ :
          ├─ elif condition
          │  └─ true
          └─ then
             └─ echo y
        run
          built-in false                           no fork: it runs in the shell itself
          if: the condition gave 1                 so: try the elif
          built-in true                            no fork: it runs in the shell itself
          elif: the condition gave 0               so: then
          built-in echo                            no fork: it runs in the shell itself
        status 0
        """)
    assert result.stdout == "y\n"


def test_while_that_never_runs(run):
    err, _ = explained(run, "explain while false; do :; done")
    assert sq("  while: the condition gave 1    so: the loop ends\n") in err


def test_lists_and_and_or(run):
    err, result = explained(run, "explain echo a && false || echo b; echo c")
    assert err.startswith(block("""
        plan
          list of 2, one after the other
          ├─ and-or of 3
          │  ├─ echo a
          │  ├─ && if that worked: false
          │  └─ || if that failed: echo b
          └─ echo c
        run
        """))
    assert result.stdout == "a\nb\nc\n"


def test_negation_and_a_builtin_in_a_child(run):
    err, _ = explained(run, "explain ! true | cat")
    assert "  ! pipeline of 2\n" in err
    assert sq("  [P1] true runs in this child    no exec: the shell's own code does it\n") in err
    assert err.endswith("status 1\n")


def test_failures_are_explained_too(run):
    err, _ = explained(run, "explain cat < nope\nexplain nope")
    assert sq("  [P1] open(\"nope\", O_RDONLY) failed    No such file or directory\n") in err
    assert "  waitpid(P1) = exit 1\nstatus 1\n" in err
    assert sq("  [P2] execvp(nope)    not in PATH\n") in err
    assert "acsh: nope: command not found\n" in err
    assert err.endswith("  waitpid(P2) = exit 127\nstatus 127\n")


def test_explanation_is_not_caught_by_the_commands_stderr(run, workdir):
    err, _ = explained(run, "explain ls /nope 2> errors")
    assert "plan\n" in err
    assert "status 2\n" in err
    errors = (workdir / "errors").read_text()
    assert "nope" in errors
    assert "plan" not in errors


def test_children_get_no_extra_fds(run):
    result = run(stdin="explain ls /proc/self/fd\n", env=ENV)
    # ls opens one fd of its own for the folder; the explain copy and the
    # report pipe are close-on-exec
    assert result.stdout.split() == ["0", "1", "2", "3"]


def test_only_the_explained_line(run):
    err, result = explained(run, "explain echo a\necho b")
    assert err.count("plan\n") == 1
    assert result.stdout == "a\nb\n"


def test_explain_every_line_with_the_option(run):
    result = run("--explain", stdin="echo a\necho b\n", env=ENV)
    assert result.stderr.count("plan\n") == 2
    assert result.stderr.count("status 0\n") == 2


def test_set_turns_it_on_and_off(run):
    result = run(stdin="set -o\nset -o explain\necho a\nset +o explain\necho b\nset -o\n", env=ENV)
    assert result.stdout == "explain         off\na\nb\nexplain         off\n"
    assert result.stderr.count("plan\n") == 2  # echo a, and the set +o line
    assert "  echo a\n" in result.stderr
    assert "  echo b\n" not in result.stderr


def test_explained_source_explains_its_lines(run, workdir):
    (workdir / "lib.sh").write_text("echo one\necho two\n")
    result = run(stdin="explain . ./lib.sh\n", env=ENV)
    assert result.stderr.count("plan\n") == 3


def test_explain_needs_a_command(run):
    result = run(stdin="explain\necho never\n")
    assert result.stderr == "acsh: explain: needs a command after it\n"
    assert result.returncode == 2


def test_explain_in_the_middle_of_a_line(run):
    result = run(stdin="echo a; explain echo b\necho $?\n")
    assert result.stderr == "acsh: explain: put it at the start of a line, before the command\n"
    assert result.stdout == "a\n2\n"


def test_quoted_explain_is_a_command_name(run):
    result = run(stdin="'explain' x\n")
    assert result.stderr == "acsh: explain: put it at the start of a line, before the command\n"


def test_shell_has_no_explain_fd_until_it_is_used(run):
    result = run(stdin="sh -c 'ls /proc/$PPID/fd'\nset -o explain\nsh -c 'ls /proc/$PPID/fd'\n")
    # before: only 0, 1 and 2; after: the copy of stderr at 10 as well (ls
    # sorts by name, so 10 comes before 2)
    assert result.stdout.split() == ["0", "1", "2", "0", "1", "10", "2"]


# ---- set ---------------------------------------------------------------------


def test_set_lists_variables(run):
    result = run(stdin="A_TEST_VAR=\"it's\"\nset\n", env={"ZZ_TEST": "z"})
    assert "A_TEST_VAR='it'\"'\"'s'\n" in result.stdout
    assert "ZZ_TEST='z'\n" in result.stdout


def test_set_parameters(run):
    result = run("-c", "set -- a 'b c'; echo $# $2; set x; echo $# $1; set --; echo $#", "n", "old")
    assert result.stdout == "2 b c\n1 x\n0\n"


def test_set_errors(run):
    result = run(stdin="set -e\necho $?\nset -o nope\necho $?\n")
    assert result.stderr == (
        "acsh: set: -e isn't supported yet (only -o explain)\n"
        "acsh: set: nope: no such option (acsh has explain)\n"
    )
    assert result.stdout == "2\n2\n"


def test_help_lists_explain_and_set(run):
    result = run(stdin="help\n")
    assert "\n  explain " in result.stdout
    assert "\n  set " in result.stdout


def test_explain_at_the_prompt(terminal):
    shell = terminal(env=ENV)
    shell.expect(r"\$ ")
    shell.sendline("explain echo hi | cat")
    shell.expect("pipeline of 2")
    shell.expect("waitpid")
    shell.expect("status 0")
    shell.expect(r"\$ ")
    shell.sendline("exit")
    shell.expect(pexpect.EOF)
