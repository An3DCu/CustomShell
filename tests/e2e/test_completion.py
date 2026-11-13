"""Tab completion, in a pseudo-terminal with TERM=xterm.

Each test completes a word, presses Enter, and checks what the command got:
printf prints each argument between = and =.
"""

import re

import pytest

PROMPT = r"acsh\$ "
TAB, BACKTAB = "\t", "\x1b[Z"
SHOW = "printf '=%s=\\n' "


@pytest.fixture
def folder(workdir):
    """A folder with names to complete, and a bin/ with a program in it."""
    for name in ["notes.txt", "notes-old.txt", "my notes.txt", ".hidden"]:
        (workdir / name).write_text("")
    (workdir / "notebook").mkdir()
    (workdir / "run.sh").write_text("#!/bin/sh\necho =script=\n")
    (workdir / "run.sh").chmod(0o755)
    (workdir / "bin").mkdir()
    (workdir / "bin" / "acshtestprog").write_text("#!/bin/sh\necho =ran=\n")
    (workdir / "bin" / "acshtestprog").chmod(0o755)
    (workdir / "home" / "Documents").mkdir(parents=True)
    (workdir / "many").mkdir()
    for i in range(120):
        (workdir / "many" / f"f{i:03}").write_text("")
    return workdir


@pytest.fixture
def editor(terminal, folder):
    shell = terminal(env={"TERM": "xterm", "PATH": f"{folder}/bin:/usr/bin:/bin",
                          "HOME": str(folder / "home")})
    shell.expect(PROMPT)
    return shell


def run_keys(shell, keys):
    """Types keys, presses Enter, and returns the first =...= line printed."""
    shell.send(keys + "\r")
    shell.expect(r"\r\n(=[^\r\n]*=)\r\n")
    return shell.match.group(1)


@pytest.mark.parametrize(
    "keys, printed",
    [
        (SHOW + "my" + TAB, "=my notes.txt="),           # quoted, then a space
        (SHOW + "noteb" + TAB, "=notebook/="),           # a folder gets a /
        (SHOW + "noteb" + TAB + "x", "=notebook/x="),    # and no space after it
        (SHOW + ".hi" + TAB, "=.hidden="),               # a dot file only after a dot
        (SHOW + "zz" + TAB + "q", "=zzq="),              # no match: nothing happens
        (SHOW + "~/Doc" + TAB, "=HOME/Documents/="),     # ~/ looks in HOME
        (SHOW + "many/f" + TAB, "=many/f000="),          # the first match goes in
    ],
)
def test_files(editor, folder, keys, printed):
    assert run_keys(editor, keys) == printed.replace("HOME", str(folder / "home"))


def test_several_matches_are_listed_once_and_tab_goes_through_them(editor):
    # sorted: notebook/, notes-old.txt, notes.txt. Tab shows the first,
    # Tab and Tab go to the third, Shift+Tab back to the second.
    editor.send(SHOW + "note" + TAB + TAB + TAB + BACKTAB + "\r")
    editor.expect(r"\r\n=notes-old\.txt=\r\n")
    # everything acsh drew before the result: one row of three columns
    rows = re.findall(r"notebook/ +notes-old\.txt +notes\.txt", editor.before)
    assert len(rows) == 1


def test_another_key_keeps_the_match_with_a_space(editor):
    editor.send(SHOW + "note" + TAB + TAB + "x\r")
    editor.expect_exact("=notes-old.txt=\r\n=x=\r\n")


def test_a_long_list_stops_at_100(editor):
    editor.send(SHOW + "many/f" + TAB)
    editor.expect_exact("and 20 more")


@pytest.mark.parametrize(
    "keys, printed",
    [
        ("acshtestp" + TAB, "=ran="),               # a program in PATH
        ("echo hi | acshtestp" + TAB, "=ran="),     # where a command starts after |
        ("true && acshtestp" + TAB, "=ran="),       # and after &&
        ("./ru" + TAB, "=script="),                 # a word with a / is a file
    ],
)
def test_commands(editor, keys, printed):
    assert run_keys(editor, keys) == printed


def test_a_built_in_completes_too(editor):
    editor.send("disow" + TAB + "\r")
    editor.expect_exact("acsh: disown: no current job")
