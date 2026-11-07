"""Text that comes from data is never run as shell code.

Values of variables, file names that a pattern finds, lines that read
reads, and arguments to a script are all just text by the time a command
gets them. Each test plants something that would create a file if a shell
ever parsed it again, and checks that the file isn't there.
"""

import pytest

TRAPS = [
    "a; touch pwned1",
    "$(touch pwned2)",
    "`touch pwned3`",
    "| touch pwned4",
    "x && touch pwned5",
    "> pwned6",
    "'; touch pwned7; '",
]


def no_pwned(folder):
    left = sorted(p.name for p in folder.iterdir() if p.name.startswith("pwned"))
    assert left == []


def test_variable_values(run, workdir):
    lines = "".join(f"X='{t}'\necho $X\necho \"$X\"\n$X\n" for t in TRAPS if "'" not in t)
    lines += "X=\"'; touch pwned7; '\"\necho $X\n"
    run(stdin=lines)
    no_pwned(workdir)


def test_file_names_from_a_pattern(run, workdir):
    data = workdir / "data"
    data.mkdir()
    for t in TRAPS:
        (data / t.replace("/", "_")).touch()
    result = run(stdin="cd data\necho *\nfor f in *; do echo \"$f\"; done\nls *\nX=*\necho $X\n")
    assert result.returncode == 0
    no_pwned(workdir)
    no_pwned(data)
    for t in TRAPS:
        assert t in result.stdout


def test_lines_from_read(run, workdir):
    script = "while read line; do echo \"$line\"; echo $line; done\n"
    result = run(stdin=script + "".join(t + "\n" for t in TRAPS))
    no_pwned(workdir)
    for t in TRAPS:
        # once from "$line", once from $line: split into words, and echo
        # puts them back with the same single spaces
        assert result.stdout.count(t + "\n") == 2


@pytest.mark.parametrize("trap", TRAPS)
def test_script_arguments(run, workdir, trap):
    run("-c", "echo \"$1\"; echo $1; for a; do echo $a; done; echo \"$@\"", "name", trap)
    no_pwned(workdir)


def test_explained_values(run, workdir):
    run(stdin="X='a; touch pwned1'\nexplain echo $X\n")
    no_pwned(workdir)
