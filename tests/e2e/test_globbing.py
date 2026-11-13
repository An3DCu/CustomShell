"""File name patterns, checked against dash on a folder of awkward names.

Both shells run the same line in the same folder (nothing here writes to
it). Their output and exit status have to be the same. printf '[%s]'
shows exactly where one argument ends and the next begins.
"""

import os
import shutil
import subprocess

import pytest

DASH = shutil.which("dash")

NAMES = [
    "a", "b", "B", "Z", "_x", "a b", "a-1", "abc", "aXc", "x*y", "x?", "[ab]",
    "q]", "-n", "ä", "é", ".hidden", ".h2",
]


@pytest.fixture
def awkward(workdir):
    """A folder with odd names, hidden files, folders and links."""
    for name in NAMES:
        (workdir / name).touch()
    (workdir / "dir" / "sub").mkdir(parents=True)
    (workdir / "dir" / ".inner").mkdir()
    (workdir / ".hdir").mkdir()
    for name in ["dir/f1", "dir/f2", "dir/sub/g", "dir/.inner/h"]:
        (workdir / name).touch()
    os.symlink("dir", workdir / "link")
    os.symlink("nowhere", workdir / "dangling")
    return workdir


@pytest.fixture
def same_as_dash(acsh, awkward):
    if DASH is None:
        pytest.skip("dash isn't installed")

    def _check(script):
        results = [
            subprocess.run(
                [shell], input=script, capture_output=True, text=True, cwd=awkward, timeout=10
            )
            for shell in (acsh, DASH)
        ]
        mine, theirs = results
        assert mine.stdout == theirs.stdout, f"stderr: {mine.stderr!r}"
        assert mine.returncode == theirs.returncode
        return mine

    return _check


@pytest.mark.parametrize(
    "words",
    [
        # every name, sorted by byte value: B and Z before a, ä and é last
        "*",
        ".*",
        "?",
        "a*",
        "*c",
        "a?c",
        "x*",
        # brackets
        "[ab]",
        "[!a]",
        "[^a]",
        "[a-]",
        "[z-a]",
        "[]a]",
        "[!]a]",
        "[[:upper:]]",
        "[[:alpha:]]*",
        "[[:alpha:]-]",
        "[[:nope:]]",
        "[a",
        "a[b",
        # quoting turns a pattern character back into a plain one
        "x\\*y",
        "x\"*\"y",
        "\"[ab]\"",
        "\\[ab\\]",
        "[\\!a]",
        "[a\\]]",
        "q\\]",
        "\"a b\"*",
        # folders and links
        "*/",
        "*/*",
        "dir/*",
        "dir/.*",
        "*/f1",
        "*/nope",
        "d*/s*/g",
        "dir//f*",
        "./a*",
        "dang*",
        "link/*",
        # hidden names need a dot in the pattern
        ".h*",
        "[.]h*",
        "?hidden",
        # no match: the word stays as typed
        "nomatch*",
        "\"nomatch\"*",
        "a[/]b",
        # several patterns in one line
        "a* b* nomatch* B",
    ],
)
def test_patterns(same_as_dash, words):
    same_as_dash(f"printf '[%s]' {words}\necho\n")


def test_absolute_patterns(same_as_dash):
    same_as_dash("printf '[%s]' /e*c //e*c /etc/host?ame\necho\n")


def test_patterns_from_variables(same_as_dash):
    same_as_dash(
        "X='*'\nprintf '[%s]' $X \"$X\"\necho\n"
        "X='a b'\nprintf '[%s]' $X*\necho\n"
        "X='[ab]'\nprintf '[%s]' $X\necho\n"
    )


def test_no_patterns_in_assignments(same_as_dash):
    same_as_dash("X=a*\necho $X\necho \"$X\"\nexport Y=a*\nprintf '[%s]' \"$Y\"\necho\n")


def test_no_patterns_in_redirections(same_as_dash, awkward):
    same_as_dash("echo hi > *.nope\ncat '*.nope'\n")


def test_a_command_from_a_pattern(same_as_dash, awkward):
    (awkward / "dir" / "sub" / "g").write_text("#!/bin/sh\necho ran g\n")
    (awkward / "dir" / "sub" / "g").chmod(0o755)
    same_as_dash("dir/s?b/g\n")


def test_unreadable_folder_is_no_match(acsh, workdir, run):
    folder = workdir / "closed"
    folder.mkdir()
    (folder / "f").touch()
    folder.chmod(0)
    try:
        result = run(stdin="printf '[%s]' closed/*\n")
    finally:
        folder.chmod(0o755)
    assert result.stdout == "[closed/*]"


def test_many_files(run, workdir):
    for i in range(2000):
        (workdir / f"f{i:04}").touch()
    result = run(stdin="printf '%s\\n' f* | wc -l\necho f0* | wc -w\n")
    assert result.stdout.split() == ["2000", "1000"]


def test_bracket_test_command_still_works(run, workdir):
    (workdir / "x").touch()
    result = run(stdin="[ -f x ]\necho $?\n[ -f nope ]\necho $?\n")
    assert result.stdout == "0\n1\n"
