"""My own cat, head and wc, checked against GNU coreutils.

Every tool runs on the same awkward inputs as the GNU tool, with LC_ALL=C so
GNU counts bytes, not characters. cat and head must give exactly the same
bytes. For wc only the numbers are compared: the column spacing is my own.

Ubuntu 25.10 and later run uutils (a Rust rewrite) as `cat`, and keep GNU
as `gnucat`; older systems have GNU as `cat`. gnu() finds the GNU one either
way and refuses to compare against anything else.
"""

import functools
import os
import random
import shlex
import shutil
import subprocess

import pytest

SYSTEM = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}


@functools.cache
def gnu(name):
    """The GNU coreutils version of a tool, by its gnu-prefixed name or its own."""
    for candidate in (f"gnu{name}", name):
        path = shutil.which(candidate, path=SYSTEM["PATH"])
        if path is None:
            continue
        version = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
        if "GNU coreutils" in version:
            return path
    pytest.fail(f"no GNU coreutils {name} to compare with")

INPUTS = {
    "empty": b"",
    "one_line": b"one line\n",
    "no_final_newline": b"no newline at the end",
    "blank_lines": b"a\nb\n\n\nc",
    "nul_bytes": b"ab\0cd\nsecond\0line\n\0",
    "binary": random.Random(42).randbytes(300_000),
    "many_lines": b"".join(b"line %d\n" % i for i in range(2000)),
    "long_line": b"x" * 200_000,
    "whitespace": b"  a\tb\v c\f d\r\n e  \n\t\n",
    "crlf": b"x\r\ny\r\n",
    "utf8": "čšž ćđ\n日本語 テキスト\n".encode(),
}


@pytest.fixture
def files(workdir):
    """Writes every input into the test's folder and returns name -> path."""
    paths = {}
    for name, data in INPUTS.items():
        path = workdir / name
        path.write_bytes(data)
        paths[name] = path
    return paths


def run_tool(path, *args, stdin=b"", cwd=None):
    return subprocess.run([str(path), *map(str, args)], input=stdin, capture_output=True,
                          cwd=cwd, timeout=20, env={**os.environ, **SYSTEM})


def system(name, *args, stdin=b"", cwd=None):
    """Runs the GNU version of the tool.

    wc runs with POSIXLY_CORRECT: by default GNU wc also splits words at byte
    0xA0 (a no-break space), which POSIX doesn't. Mine follows POSIX.
    """
    env = {**os.environ, **SYSTEM}
    if name == "wc":
        env["POSIXLY_CORRECT"] = "1"
    return subprocess.run([gnu(name), *map(str, args)], input=stdin, capture_output=True,
                          cwd=cwd, timeout=20, env=env)


# ---- cat --------------------------------------------------------------------


@pytest.mark.parametrize("name", INPUTS)
@pytest.mark.parametrize("flags", [[], ["-n"]])
def test_cat_matches_the_system(tools, files, name, flags):
    mine = run_tool(tools / "cat", *flags, files[name])
    theirs = system("cat", *flags, files[name])
    assert mine.stdout == theirs.stdout
    assert mine.returncode == 0
    assert mine.stderr == b""


def test_cat_several_files_and_numbering_goes_on(tools, files):
    names = [files["blank_lines"], files["one_line"], files["no_final_newline"], files["crlf"]]
    for flags in ([], ["-n"]):
        assert run_tool(tools / "cat", *flags, *names).stdout == system("cat", *flags, *names).stdout


def test_cat_reads_stdin(tools):
    assert run_tool(tools / "cat", stdin=b"from stdin\n").stdout == b"from stdin\n"
    assert run_tool(tools / "cat", "-", stdin=b"dash\n").stdout == b"dash\n"


def test_cat_mixes_files_and_stdin(tools, files):
    result = run_tool(tools / "cat", files["one_line"], "-", files["one_line"], stdin=b"middle\n")
    assert result.stdout == b"one line\nmiddle\none line\n"


def test_cat_numbered_lines_come_out_right_away(tools):
    # -n collects output, but must not hold a line back waiting for more.
    proc = subprocess.Popen([str(tools / "cat"), "-n"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        proc.stdin.write(b"typed\n")
        proc.stdin.flush()
        assert proc.stdout.readline() == b"     1\ttyped\n"
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)


def test_cat_missing_file_goes_on(tools, files):
    result = run_tool(tools / "cat", "missing", files["one_line"], cwd=files["one_line"].parent)
    assert result.stdout == b"one line\n"
    assert result.stderr == b"cat: missing: No such file or directory\n"
    assert result.returncode == 1


def test_cat_folder(tools, workdir):
    (workdir / "sub").mkdir()
    result = run_tool(tools / "cat", "sub", cwd=workdir)
    assert result.stderr == b"cat: sub: Is a directory\n"
    assert result.returncode == 1


def test_cat_write_error(tools, files):
    with open("/dev/full", "wb") as full:
        result = subprocess.run([str(tools / "cat"), str(files["binary"])], stdout=full,
                                stderr=subprocess.PIPE, timeout=10)
    assert result.stderr == b"cat: write error: No space left on device\n"
    assert result.returncode == 1


def test_cat_options(tools):
    result = run_tool(tools / "cat", "-x")
    assert result.stderr == b"cat: unknown option '-x'\nusage: cat [-n] [file ...]\n"
    assert result.returncode == 2
    assert run_tool(tools / "cat", "--help").stdout == b"usage: cat [-n] [file ...]\n"


# ---- head -------------------------------------------------------------------


@pytest.mark.parametrize("name", INPUTS)
@pytest.mark.parametrize("flags", [[], ["-n", "0"], ["-n", "1"], ["-n", "3"], ["-n", "100000"]])
def test_head_matches_the_system(tools, files, name, flags):
    mine = run_tool(tools / "head", *flags, files[name])
    theirs = system("head", *flags, files[name])
    assert mine.stdout == theirs.stdout
    assert mine.returncode == 0


def test_head_several_files_get_headers(tools, files):
    names = [files["many_lines"], files["one_line"], files["empty"]]
    for flags in ([], ["-n", "2"]):
        assert run_tool(tools / "head", *flags, *names).stdout == system("head", *flags, *names).stdout


def test_head_stdin_header(tools, files):
    args = ["-n", "1", files["one_line"], "-"]
    assert run_tool(tools / "head", *args, stdin=b"in\n").stdout == system("head", *args, stdin=b"in\n").stdout


def test_head_option_written_together(tools, files):
    assert run_tool(tools / "head", "-n2", files["many_lines"]).stdout == b"line 0\nline 1\n"


def test_head_stops_reading_early(run, tools):
    # yes never ends by itself. It only stops when head exits and the pipe
    # loses its reader, so this would hang if head read on to the end.
    result = run(stdin=f"yes | {shlex.quote(str(tools / 'head'))} -n 3\n", timeout=10)
    assert result.stdout == "y\ny\ny\n"
    assert result.returncode == 0


def test_head_bad_counts(tools):
    for count in ["x", "-1", "3x", ""]:
        result = run_tool(tools / "head", "-n", count)
        assert result.stderr == f"head: invalid number of lines: '{count}'\n".encode()
        assert result.returncode == 2
    result = run_tool(tools / "head", "-n")
    assert result.stderr.startswith(b"head: -n needs a number\n")
    assert result.returncode == 2


def test_head_missing_file(tools, files):
    result = run_tool(tools / "head", "-n", "1", "missing", files["one_line"],
                      cwd=files["one_line"].parent)
    assert b"one line\n" in result.stdout
    assert result.stderr == b"head: missing: No such file or directory\n"
    assert result.returncode == 1


# ---- wc ---------------------------------------------------------------------


@pytest.mark.parametrize("name", INPUTS)
@pytest.mark.parametrize("flags", [[], ["-l"], ["-w"], ["-c"], ["-lw"], ["-cl"]])
def test_wc_counts_match_the_system(tools, files, name, flags):
    mine = run_tool(tools / "wc", *flags, files[name])
    theirs = system("wc", *flags, files[name])
    assert mine.stdout.split() == theirs.stdout.split()
    assert mine.returncode == 0


def test_wc_several_files_and_total(tools, files):
    names = [files["many_lines"], files["utf8"], files["empty"]]
    mine = run_tool(tools / "wc", *names)
    assert mine.stdout.split() == system("wc", *names).stdout.split()
    assert mine.stdout.splitlines()[-1].endswith(b" total")


def test_wc_columns_line_up(tools, files):
    lines = run_tool(tools / "wc", files["many_lines"], files["one_line"]).stdout.splitlines()
    starts = {line.index(b"/") for line in lines[:2]}  # where the file name begins
    assert len(starts) == 1


def test_wc_words_split_only_at_whitespace(tools):
    # Space, tab, newline, \v, \f and \r split words; nothing else does. GNU
    # also splits at 0xA0 unless POSIXLY_CORRECT is set.
    assert run_tool(tools / "wc", "-w", stdin=b"a b\tc\nd\ve\ff\rg").stdout == b"7\n"
    assert run_tool(tools / "wc", "-w", stdin=b"a\xa0a a\0a a\x7fa").stdout == b"3\n"


def test_wc_reads_stdin(tools):
    result = run_tool(tools / "wc", "-l", stdin=b"a\nb\nc\n")
    assert result.stdout == b"3\n"


def test_wc_missing_file(tools, files):
    result = run_tool(tools / "wc", "-l", "missing", files["one_line"], cwd=files["one_line"].parent)
    assert result.stderr == b"wc: missing: No such file or directory\n"
    assert result.stdout.split()[-2:] == [b"1", b"total"]
    assert result.returncode == 1


def test_wc_options(tools):
    result = run_tool(tools / "wc", "-m")
    assert result.stderr.startswith(b"wc: unknown option '-m'\n")
    assert result.returncode == 2


# ---- together in acsh -------------------------------------------------------


def test_tools_in_a_pipeline(run, tools, files):
    t = shlex.quote(str(tools))
    result = run(stdin=f"{t}/cat {files['many_lines']} | {t}/head -n 50 | {t}/wc -l\n")
    assert result.stdout == "50\n"


def test_own_tools_option_puts_them_first(run, tools):
    result = run("--own-tools", stdin="cat --help\nprintenv PATH\n")
    lines = result.stdout.splitlines()
    assert lines[0] == "usage: cat [-n] [file ...]"
    assert lines[1].startswith(f"{tools}:")


def test_without_the_option_the_system_tools_run(run):
    result = run(stdin="cat --help\n")
    assert result.returncode == 0
    assert result.stdout != ""
    assert not result.stdout.startswith("usage: cat [-n]")  # the system's help, not mine


def test_own_tools_without_a_tools_folder(acsh, workdir):
    lonely = workdir / "acsh"
    lonely.write_bytes(open(acsh, "rb").read())
    lonely.chmod(0o755)
    result = subprocess.run([str(lonely), "--own-tools"], input="", capture_output=True, text=True,
                            timeout=10)
    assert result.stderr == f"acsh: --own-tools: no tools folder at {workdir}/tools\n"
    assert result.returncode == 2
