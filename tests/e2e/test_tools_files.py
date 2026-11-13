"""My own touch, mkdir and ls.

touch and mkdir are checked by what they leave behind. ls is compared with
GNU ls on a folder full of awkward entries. The output must be the same.
Both get LC_ALL=C and TZ=UTC. Sorting and times can't differ for locale
reasons then.
"""

import os
import shlex
import stat
import subprocess
import time

import pexpect
import pytest

from test_tools import gnu, run_tool

UTC = {"LC_ALL": "C", "TZ": "UTC", "PATH": "/usr/bin:/bin"}


def umask():
    mask = os.umask(0)
    os.umask(mask)
    return mask


# ---- touch ------------------------------------------------------------------


def test_touch_creates_an_empty_file(tools, workdir):
    result = run_tool(tools / "touch", "new", cwd=workdir)
    assert result.returncode == 0
    assert (workdir / "new").read_bytes() == b""
    assert stat.S_IMODE(os.stat(workdir / "new").st_mode) == 0o666 & ~umask()


def test_touch_updates_the_times(tools, workdir):
    old = workdir / "old"
    old.write_text("keep me\n")
    os.utime(old, (946684800, 946684800))  # 2000-01-01
    before = time.time()
    run_tool(tools / "touch", "old", cwd=workdir)
    st = os.stat(old)
    assert st.st_mtime >= before - 1
    assert st.st_atime >= before - 1
    assert old.read_text() == "keep me\n"  # contents untouched


def test_touch_c_does_not_create(tools, workdir):
    result = run_tool(tools / "touch", "-c", "ghost", cwd=workdir)
    assert result.returncode == 0
    assert result.stderr == b""
    assert not (workdir / "ghost").exists()


def test_touch_c_still_updates_existing_files(tools, workdir):
    f = workdir / "f"
    f.write_text("")
    os.utime(f, (946684800, 946684800))
    run_tool(tools / "touch", "-c", "f", cwd=workdir)
    assert os.stat(f).st_mtime > 946684800


def test_touch_a_folder(tools, workdir):
    d = workdir / "d"
    d.mkdir()
    os.utime(d, (946684800, 946684800))
    assert run_tool(tools / "touch", "d", cwd=workdir).returncode == 0
    assert os.stat(d).st_mtime > 946684800


def test_touch_several_and_errors(tools, workdir):
    result = run_tool(tools / "touch", "a", "nope/b", "c", cwd=workdir)
    assert result.stderr == b"touch: nope/b: No such file or directory\n"
    assert result.returncode == 1
    assert (workdir / "a").exists() and (workdir / "c").exists()


def test_touch_needs_a_name(tools):
    result = run_tool(tools / "touch")
    assert result.stderr.startswith(b"touch: missing file name\n")
    assert result.returncode == 2


# ---- mkdir ------------------------------------------------------------------


def test_mkdir_makes_a_folder(tools, workdir):
    assert run_tool(tools / "mkdir", "new", cwd=workdir).returncode == 0
    st = os.stat(workdir / "new")
    assert stat.S_ISDIR(st.st_mode)
    assert stat.S_IMODE(st.st_mode) == 0o777 & ~umask()


def test_mkdir_several(tools, workdir):
    run_tool(tools / "mkdir", "a", "b", "c", cwd=workdir)
    assert all((workdir / n).is_dir() for n in "abc")


def test_mkdir_existing_is_an_error(tools, workdir):
    (workdir / "d").mkdir()
    result = run_tool(tools / "mkdir", "d", cwd=workdir)
    assert result.stderr == b"mkdir: d: File exists\n"
    assert result.returncode == 1


def test_mkdir_missing_parent(tools, workdir):
    result = run_tool(tools / "mkdir", "a/b", cwd=workdir)
    assert result.stderr == b"mkdir: a/b: No such file or directory\n"
    assert result.returncode == 1


def test_mkdir_p_makes_the_parents(tools, workdir):
    assert run_tool(tools / "mkdir", "-p", "a/b/c", cwd=workdir).returncode == 0
    assert (workdir / "a" / "b" / "c").is_dir()


def test_mkdir_p_existing_is_fine(tools, workdir):
    (workdir / "a" / "b").mkdir(parents=True)
    result = run_tool(tools / "mkdir", "-p", "a/b", cwd=workdir)
    assert result.returncode == 0
    assert result.stderr == b""


@pytest.mark.parametrize("path", ["x/y/", "x//y", "./x/y", "x/./y/z"])
def test_mkdir_p_odd_paths(tools, workdir, path):
    assert run_tool(tools / "mkdir", "-p", path, cwd=workdir).returncode == 0
    assert (workdir / os.path.normpath(path)).is_dir()


def test_mkdir_p_absolute_path(tools, workdir):
    target = workdir / "abs" / "deep"
    assert run_tool(tools / "mkdir", "-p", target).returncode == 0
    assert target.is_dir()


def test_mkdir_p_with_a_file_in_the_way(tools, workdir):
    (workdir / "a").write_text("")
    result = run_tool(tools / "mkdir", "-p", "a/b", cwd=workdir)
    assert result.stderr == b"mkdir: a/b: Not a directory\n"
    assert result.returncode == 1


def test_mkdir_goes_on_after_an_error(tools, workdir):
    (workdir / "taken").mkdir()
    result = run_tool(tools / "mkdir", "taken", "fresh", cwd=workdir)
    assert result.returncode == 1
    assert (workdir / "fresh").is_dir()


# ---- ls ---------------------------------------------------------------------

NOW = time.time()


@pytest.fixture
def tree(workdir):
    """A folder with the kinds of entries that make ls output tricky."""
    root = workdir / "tree"
    root.mkdir()
    names = ["b.txt", "B.txt", "a10", "a2", "_under", "-dash", "with space", "čš.txt", "zz"]
    for i, name in enumerate(names):
        (root / name).write_bytes(b"x" * (i * 37))
    (root / ".hidden").write_text("secret\n")
    (root / ".config").mkdir()
    (root / "sub").mkdir()
    (root / "sub" / "inner.txt").write_text("in\n")
    (root / "empty_dir").mkdir()
    (root / "big").write_bytes(b"\0" * 1_234_567)
    (root / "run.sh").write_text("#!/bin/sh\n")
    (root / "run.sh").chmod(0o755)
    (root / "setuid").write_text("")
    (root / "setuid").chmod(0o4755)
    (root / "sticky").mkdir()
    (root / "sticky").chmod(0o1777)
    os.link(root / "zz", root / "hardlink")
    os.symlink("b.txt", root / "link")
    os.symlink("nowhere", root / "dangling")
    # Times: recent, old, and in the future (shown with a year, like old ones).
    os.utime(root / "a2", (NOW - 86400, NOW - 86400))
    os.utime(root / "a10", (1577836800, 1577836800))  # 2020-01-01
    os.utime(root / "zz", (NOW + 365 * 86400, NOW + 365 * 86400))
    return root


def mine(tools, *args, cwd, columns=None):
    env = {**UTC, **({"COLUMNS": str(columns)} if columns else {})}
    return run_ls(str(tools / "ls"), args, cwd, env)


def theirs(*args, cwd, columns=None):
    env = {**UTC, **({"COLUMNS": str(columns)} if columns else {})}
    return run_ls(gnu("ls"), args, cwd, env)


def run_ls(path, args, cwd, env):
    return subprocess.run([path, *map(str, args)], capture_output=True, cwd=cwd, timeout=20,
                          env={**os.environ, **env})


@pytest.mark.parametrize("flags", [[], ["-1"], ["-a"], ["-1a"], ["-l"], ["-la"], ["-al"]])
def test_ls_matches_gnu(tools, tree, flags):
    a, b = mine(tools, *flags, cwd=tree), theirs(*flags, cwd=tree)
    assert a.stdout.decode() == b.stdout.decode()
    assert a.returncode == 0


@pytest.mark.parametrize("columns", [1, 12, 20, 40, 80, 200])
@pytest.mark.parametrize("flags", [["-C"], ["-Ca"]])
def test_ls_columns_match_gnu(tools, tree, flags, columns):
    a = mine(tools, *flags, cwd=tree, columns=columns).stdout.decode()
    b = theirs(*flags, cwd=tree, columns=columns).stdout.decode()
    assert a == b.expandtabs(8)  # GNU may pad with tabs. The layout is what counts.


def test_ls_columns_with_many_names(tools, workdir):
    for i in range(137):
        (workdir / f"file{i:03d}{'x' * (i % 7)}").write_text("")
    for columns in (30, 80, 133):
        a = mine(tools, "-C", cwd=workdir, columns=columns).stdout.decode()
        b = theirs("-C", cwd=workdir, columns=columns).stdout.decode()
        assert a == b.expandtabs(8)


@pytest.mark.parametrize(
    "args",
    [
        ["sub"],
        ["b.txt"],
        ["b.txt", "a2", "sub"],
        ["sub", "empty_dir", "zz"],
        ["-l", "b.txt", "link", "sub"],
        ["-l", "link", "dangling"],
        ["link", "dangling"],
        ["-a", "sub", ".config"],
    ],
)
def test_ls_operands_match_gnu(tools, tree, args):
    assert mine(tools, *args, cwd=tree).stdout.decode() == theirs(*args, cwd=tree).stdout.decode()


def test_ls_empty_folder(tools, tree):
    assert mine(tools, "empty_dir", cwd=tree).stdout == b""
    assert mine(tools, "-l", "empty_dir", cwd=tree).stdout == b"total 0\n"


def test_ls_in_a_terminal_uses_columns(tools, tree):
    # A terminal 40 columns wide, with no COLUMNS variable. ls has to ask the
    # terminal for its width. (Quoted: pexpect splits a command with spaces.)
    child = pexpect.spawn(shlex.quote(str(tools / "ls")), cwd=str(tree), dimensions=(24, 40),
                          env={**{k: v for k, v in os.environ.items() if k != "COLUMNS"}, **UTC},
                          encoding="utf-8", timeout=5)
    child.expect(pexpect.EOF)
    shown = child.before.replace("\r\n", "\n")
    expected = mine(tools, "-C", cwd=tree, columns=40).stdout.decode()
    assert shown == expected


def test_ls_missing_operand(tools, tree):
    result = mine(tools, "nope", "b.txt", cwd=tree)
    assert result.stderr == b"ls: nope: No such file or directory\n"
    assert result.stdout == b"b.txt\n"
    assert result.returncode == 1


def test_ls_unreadable_folder(tools, workdir):
    locked = workdir / "locked"
    locked.mkdir()
    locked.chmod(0)
    try:
        result = run_tool(tools / "ls", "locked", cwd=workdir)
        assert result.stderr == b"ls: locked: Permission denied\n"
        assert result.returncode == 1
    finally:
        locked.chmod(0o755)


def test_ls_options(tools):
    result = run_tool(tools / "ls", "-z")
    assert result.stderr == b"ls: unknown option '-z'\nusage: ls [-1aCl] [file ...]\n"
    assert result.returncode == 2


def test_ls_through_acsh(run, tools, tree):
    count = len(mine(tools, cwd=tree).stdout.splitlines())  # before x/ exists
    result = run("--own-tools", stdin=f"cd {tree}\nls | wc -l\nmkdir -p x/y\ntouch x/y/z\nls x/y\n")
    assert result.stdout.split() == [str(count), "z"]
