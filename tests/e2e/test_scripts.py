"""Scripts: acsh file, acsh -c, source and ., $0, $1, $#, $@ and shift.

The comparisons run each case in acsh and in dash with the same arguments.
dash has no `source` (only `.`). It ignores arguments after `. file`. It
stops a script when shift fails. acsh does what bash does there. Those cases
are checked against acsh's own expected output instead.
"""

import shutil
import subprocess

import pytest

DASH = shutil.which("dash")


def both(acsh, args, cwd, stdin=""):
    if DASH is None:
        pytest.skip("dash isn't installed")
    return [
        subprocess.run([shell, *args], input=stdin, capture_output=True, text=True,
                       cwd=cwd, timeout=10)
        for shell in (acsh, DASH)
    ]


def assert_same(acsh, args, cwd, stdin=""):
    mine, theirs = both(acsh, args, cwd, stdin)
    assert mine.stdout == theirs.stdout, f"stderr: {mine.stderr!r}"
    assert mine.returncode == theirs.returncode
    return mine


def write(path, text, mode=0o644):
    path.write_text(text)
    path.chmod(mode)
    return path


# ---- -c ----------------------------------------------------------------------


@pytest.mark.parametrize(
    "args",
    [
        ["-c", "echo $0 $# $1 $2", "name", "one", "two"],
        ["-c", "echo $#"],
        ["-c", "printf '[%s]' \"$@\"", "n", "a", "", "c d"],
        ["-c", "printf '[%s]' \"$*\"", "n", "a", "", "c d"],
        ["-c", "printf '[%s]' $@", "n", "a", "", "c d"],
        ["-c", "printf '[%s]' $*", "n", "a", "", "c d"],
        ["-c", "printf '[%s]' \"x$@y\"", "n", "a", "b"],
        ["-c", "printf '[%s]' \"x$@y\"", "n"],
        ["-c", "printf '[%s]' \"$@\"", "n"],
        ["-c", "IFS=:; printf '[%s]' \"$*\"", "n", "a", "b"],
        ["-c", "IFS=; printf '[%s]' \"$*\" $*", "n", "a", "b"],
        ["-c", "echo ${10} $10", "n", "1", "2", "3", "4", "5", "6", "7", "8", "9", "ten"],
        ["-c", "shift; echo $1 $#; shift 1; echo $1 $#", "n", "a", "b", "c"],
        ["-c", "exit 3; echo no"],
        ["-c", ""],
        ["-c", "echo a\necho b"],
        ["-c", "false"],
        ["-c", "for_real=1; echo $for_real"],
    ],
)
def test_dash_c(acsh, workdir, args):
    assert_same(acsh, args, workdir)


def test_dash_c_needs_a_command(run):
    result = run("-c")
    assert result.returncode == 2
    assert result.stderr.startswith("acsh: -c: needs a command\nusage: acsh")


def test_dash_c_does_not_read_stdin(run):
    result = run("-c", "cat", stdin="for cat\n")
    assert result.stdout == "for cat\n"


# ---- script files ------------------------------------------------------------


def test_script_file(acsh, workdir):
    write(workdir / "s.sh", "echo in script $0 $1 $#\nX=set\necho $X\nexit 4\necho no\n")
    assert_same(acsh, ["s.sh", "x", "y"], workdir)


def test_script_status_is_the_last_commands(acsh, workdir):
    write(workdir / "s.sh", "true\nfalse\n")
    assert_same(acsh, ["s.sh"], workdir)


def test_script_reads_commands_and_its_stdin_apart(acsh, workdir):
    write(workdir / "s.sh", "sh -c 'read line; echo got $line'\ncat\n")
    mine = assert_same(acsh, ["s.sh"], workdir, stdin="first\nrest\n")
    assert mine.stdout == "got first\nrest\n"


def test_script_with_a_syntax_error_stops_there(acsh, workdir):
    write(workdir / "bad.sh", "echo one\n)\necho two\n")
    mine, theirs = both(acsh, ["bad.sh"], workdir)
    assert mine.stdout == theirs.stdout == "one\n"
    assert mine.returncode == theirs.returncode == 2


def test_script_ending_inside_a_quote(acsh, workdir):
    write(workdir / "bad.sh", "echo line1\necho \"a\n")
    result = subprocess.run([acsh, "bad.sh"], capture_output=True, text=True, cwd=workdir)
    assert result.stdout == "line1\n"
    assert result.stderr == "acsh: bad.sh: line 2: unclosed double quote\n"
    assert result.returncode == 2


def test_missing_script(run):
    result = run("nope.sh")
    assert result.stderr == "acsh: nope.sh: No such file or directory\n"
    assert result.returncode == 127


def test_folder_as_a_script(run, workdir):
    (workdir / "adir").mkdir()
    result = run("adir")
    assert result.stderr == "acsh: adir: Is a directory\n"
    assert result.returncode == 126


@pytest.fixture
def acsh_link(acsh, workdir):
    """A path to acsh without spaces. A #! line can't have one. This repo's
    folder may."""
    link = workdir / "acsh"
    link.symlink_to(acsh)
    return link


def test_shebang(acsh_link, workdir):
    script = write(workdir / "hello", f"#!{acsh_link}\necho hello from $0 with $#\n", 0o755)
    result = subprocess.run([str(script), "a"], capture_output=True, text=True, cwd=workdir)
    assert result.stdout == f"hello from {script} with 1\n"


def test_shebang_script_run_from_acsh(run, acsh_link, workdir):
    write(workdir / "hello", f"#!{acsh_link}\necho \"[$1]\"\n", 0o755)
    result = run(stdin="./hello 'a b'\n")
    assert result.stdout == "[a b]\n"


def test_long_script(acsh, workdir):
    lines = "".join(f"echo {i}\n" for i in range(2000))
    write(workdir / "long.sh", lines)
    result = subprocess.run([acsh, "long.sh"], capture_output=True, text=True, cwd=workdir)
    assert result.stdout.split() == [str(i) for i in range(2000)]


# ---- source and . ------------------------------------------------------------


def test_dot_runs_in_this_shell(acsh, workdir):
    write(workdir / "lib.sh", "echo sourced\nX=from_lib\ncd /\n")
    assert_same(acsh, [], workdir, stdin=". ./lib.sh\necho $X\npwd\n")


def test_dot_sees_the_callers_parameters(acsh, workdir):
    write(workdir / "lib.sh", "echo in lib: $# $1\n")
    assert_same(acsh, ["-c", ". ./lib.sh", "n", "top"], workdir)


def test_source_arguments_last_while_it_runs(run, workdir):
    write(workdir / "lib.sh", "echo in lib: $# $1\n")
    result = run("-c", ". ./lib.sh x y; echo back: $# $1", "n", "top")
    assert result.stdout == "in lib: 2 x\nback: 1 top\n"


def test_dot_status(acsh, workdir):
    write(workdir / "empty.sh", "")
    write(workdir / "fails.sh", "true\nfalse\n")
    assert_same(acsh, [], workdir, stdin="false\n. ./empty.sh\necho $?\n. ./fails.sh\necho $?\n")


def test_exit_in_a_sourced_file_leaves_the_shell(acsh, workdir):
    write(workdir / "lib.sh", "echo lib\nexit 5\n")
    assert_same(acsh, [], workdir, stdin=". ./lib.sh\necho no\n")


def test_source_is_the_same_as_dot(run, workdir):
    write(workdir / "lib.sh", "X=from_lib\n")
    result = run(stdin="source ./lib.sh\necho $X\n")
    assert result.stdout == "from_lib\n"


def test_source_looks_in_path_then_here(run, workdir):
    (workdir / "bin").mkdir()
    write(workdir / "bin" / "inpath.sh", "echo from path\n")
    write(workdir / "here.sh", "echo from here\n")
    result = run(stdin=f"PATH={workdir}/bin:$PATH\nsource inpath.sh\nsource here.sh\n")
    assert result.stdout == "from path\nfrom here\n"


def test_source_errors(run, workdir):
    result = run(stdin="source\necho $?\nsource nope.sh\necho $?\n. ./nope.sh\necho $?\n")
    assert result.stderr == (
        "acsh: source: filename argument required\n"
        "acsh: source: nope.sh: not found\n"
        "acsh: .: ./nope.sh: No such file or directory\n"
    )
    assert result.stdout == "2\n1\n1\n"


def test_source_loop_stops(run, workdir):
    write(workdir / "loop.sh", "source ./loop.sh\n")
    result = run(stdin="source ./loop.sh\necho survived $?\n")
    assert "source: ./loop.sh: sourced too deeply (a loop?)\n" in result.stderr
    assert result.stdout == "survived 1\n"


def test_syntax_error_in_a_sourced_file(run, workdir):
    write(workdir / "bad.sh", "echo in bad\n)\necho not reached\n")
    result = run(stdin=". ./bad.sh\necho after $?\n")
    assert result.stdout == "in bad\nafter 2\n"


# ---- shift -------------------------------------------------------------------


def test_shift_errors(run):
    result = run("-c", "shift 5; echo $?; shift x; echo $?; shift 1 2; echo $?; shift; echo $1",
                 "n", "a", "b")
    assert result.stderr == (  # -c with a name: errors say where, like bash
        "acsh: n: line 1: shift: can't shift that many\n"
        "acsh: n: line 1: shift: x: numeric argument required\n"
        "acsh: n: line 1: shift: too many arguments\n"
    )
    assert result.stdout == "1\n1\n1\nb\n"


def test_help_lists_the_new_builtins(run):
    result = run(stdin="help\n")
    for name in [".", "shift", "source"]:
        assert f"\n  {name} " in result.stdout
