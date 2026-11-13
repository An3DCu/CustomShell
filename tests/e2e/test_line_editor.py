"""The line editor at the prompt, in a pseudo-terminal with TERM=xterm.

Each key test types a line, edits it, presses Enter and checks what the
command printed. The output is wrapped in = =, so it can't be mistaken for
the line as the editor draws it.

The terminal's modes are read from the test's side of the pseudo-terminal:
on Linux, tcgetattr on the master gives the modes acsh's side has.
"""

import os
import re
import signal
import termios
import time

import pexpect
import pytest

PROMPT = r"acsh\$ "
LEFT, RIGHT, HOME, END, DELETE = "\x1b[D", "\x1b[C", "\x1b[H", "\x1b[F", "\x1b[3~"
BACKSPACE = "\x7f"


@pytest.fixture
def editor(terminal):
    shell = terminal(env={"TERM": "xterm", "PATH": "/usr/bin:/bin"})
    shell.expect(PROMPT)
    return shell


def run_keys(shell, keys):
    """Types keys, presses Enter, and returns the line the command printed."""
    shell.send(keys + "\r")
    shell.expect(r"\r\n(=[^\r\n]*=)\r\n")
    return shell.match.group(1)


def raw(shell):
    """True while acsh has the terminal in raw mode: no echo, no lines."""
    lflag = termios.tcgetattr(shell.child_fd)[3]
    return not lflag & (termios.ICANON | termios.ECHO)


def wait_for(check, what):
    for _ in range(200):
        if check():
            return
        time.sleep(0.01)
    raise AssertionError(what)


@pytest.mark.parametrize(
    "keys",
    [
        "echo =abc=",                             # plain typing
        "echo =ac=" + LEFT * 2 + "b",             # Left
        "echo =ac=" + LEFT * 3 + RIGHT + "b",     # Right
        "cho =abc=" + HOME + "e",                 # Home
        "echo =ab" + HOME + END + "c=",           # End
        "cho =abc=" + "\x01" + "e",               # Ctrl+A
        "echo =ab" + "\x01" + "\x05" + "c=",      # Ctrl+E
        "echo =abcX" + BACKSPACE + "=",           # Backspace
        "echo =aXbc=" + LEFT * 4 + DELETE,        # Delete
        "echo =aXbc=" + LEFT * 4 + "\x04",        # Ctrl+D in a line with text
        "echo =abc= junk" + LEFT * 5 + "\x0b",    # Ctrl+K
        "junk" + "\x15" + "echo =abc=",           # Ctrl+U
        "echo =abc= junk" + "\x17",               # Ctrl+W
        "echo =ab" + "\x1b[15~" + "\x02" + "c=",  # F5 and Ctrl+B do nothing
        "\x15\x17" + "echo =abc=",                # Ctrl+U and Ctrl+W on an empty line
        "echo =ab" + HOME + BACKSPACE + END + DELETE + "c=",  # nothing to remove
    ],
)
def test_editing_keys(editor, keys):
    assert run_keys(editor, keys) == "=abc="


def test_utf8_goes_in_and_runs(editor):
    assert run_keys(editor, "echo =čćš=") == "=čćš="


def test_a_long_line_wraps_and_can_be_edited(editor):
    # 160 characters on an 80-column terminal: three rows with the prompt.
    long = "y" * 150
    assert run_keys(editor, f"cho ={long}=" + HOME + "e") == f"={long}="
    assert run_keys(editor, f"echo ={long}X=" + LEFT * 76 + "\x01" + "\x05" + LEFT
                    + BACKSPACE) == f"={long}="


class Screen:
    """Just enough of an 80-column terminal to replay what the editor
    writes: text with wrapping, \\r, \\n, and ESC [ n A, C, J and H."""

    COLS, ROWS = 80, 24

    def __init__(self):
        self.cells = [[" "] * self.COLS for _ in range(self.ROWS)]
        self.r = self.c = 0
        self.at_edge = False  # a real terminal waits there before it wraps

    def newline(self):
        self.at_edge = False
        if self.r == self.ROWS - 1:
            self.cells = self.cells[1:] + [[" "] * self.COLS]
        else:
            self.r += 1

    def feed(self, data):
        for m in re.finditer(r"\x1b\[([0-9;]*)([A-Za-z])|(.)", data, re.S):
            code, char = m.group(2), m.group(3)
            n = int(m.group(1) or 1) if m.group(1) != "2" else 2
            if code == "A":
                self.r = max(0, self.r - n)
            elif code == "C":
                self.c = min(self.COLS - 1, self.c + n)
            elif code == "H":
                self.r = self.c = 0
            elif code == "J":
                for row in range(0 if n == 2 else self.r, self.ROWS):
                    first = self.c if row == self.r and n != 2 else 0
                    self.cells[row][first:] = [" "] * (self.COLS - first)
            elif char == "\r":
                self.c = 0
            elif char == "\n":
                self.newline()
            elif char is not None and char >= " ":
                if self.at_edge:
                    self.c = 0
                    self.newline()
                self.cells[self.r][self.c] = char
                self.at_edge = self.c == self.COLS - 1
                self.c += 0 if self.at_edge else 1
                continue
            self.at_edge = False


@pytest.mark.parametrize(
    "length, keys, cursor",
    [
        (74, "", 74),                 # ends right at the edge: 6 + 74 = 80
        (154, "", 154),               # exactly two rows
        (160, LEFT * 90, 70),         # back across a row boundary
        (200, HOME, 0),               # to the start from the third row
    ],
)
def test_a_wrapped_line_is_drawn_right(terminal, length, keys, cursor):
    shell = terminal(env={"TERM": "xterm", "NO_COLOR": "1", "PATH": "/usr/bin:/bin"})
    shell.expect(PROMPT)
    screen = Screen()
    screen.feed(shell.before + shell.after)
    top = screen.r
    for key in ["x" * length, keys]:
        shell.send(key)
        while True:  # read as it comes, or acsh would block on a full pty
            try:
                screen.feed(shell.read_nonblocking(100000, timeout=0.2))
            except pexpect.TIMEOUT:
                break
    shown = "".join("".join(row) for row in screen.cells[top:]).rstrip()
    assert shown == "acsh$ " + "x" * length
    assert (screen.r - top) * Screen.COLS + screen.c == len("acsh$ ") + cursor


def test_a_command_over_two_lines(editor):
    editor.send("echo '=a\r")
    editor.expect("> ")
    editor.send("b" + LEFT + "x" + "\x05" + "='\r")
    editor.expect(r"=a\r\nxb=\r\n")


def test_ctrl_l_clears_the_screen(editor):
    editor.send("echo =ab" + "\x0c")
    editor.expect_exact("\x1b[H\x1b[2J")
    editor.expect(r"\r\n" + PROMPT)  # the whole prompt again, after its first line
    assert run_keys(editor, "c=") == "=abc="


def test_ctrl_c_drops_the_line(editor):
    editor.send("echo =dropped=" + "\x03")
    editor.expect_exact("^C\r\n")
    editor.expect(r"exit 130.*" + PROMPT)
    assert run_keys(editor, "echo =$?=") == "=130="


def test_ctrl_d_on_an_empty_line_leaves(editor):
    editor.send("\x04")
    editor.expect(pexpect.EOF)


def test_raw_mode_only_while_a_line_is_typed(editor):
    wait_for(lambda: raw(editor), "raw mode at the prompt")
    editor.send("sleep 1\r")
    wait_for(lambda: not raw(editor), "the terminal back while a command runs")
    editor.expect(r"\r\n" + PROMPT)  # the real prompt, not a redraw
    wait_for(lambda: raw(editor), "raw mode at the next prompt")


@pytest.mark.parametrize("how", ["exit", "SIGTERM", "SIGHUP"])
def test_the_terminal_is_put_back_when_acsh_ends(editor, how):
    wait_for(lambda: raw(editor), "raw mode at the prompt")
    if how == "exit":
        editor.send("exit\r")
    else:
        editor.send("half a line")
        time.sleep(0.2)
        os.kill(editor.pid, getattr(signal, how))
    editor.expect(pexpect.EOF)
    assert not raw(editor)


@pytest.mark.parametrize("term", ["dumb", None])
def test_no_editor_without_a_terminal_it_can_draw_on(terminal, term):
    shell = terminal(env={"TERM": term} if term else {}, unset=() if term else ("TERM",))
    shell.expect(PROMPT)
    time.sleep(0.2)
    assert not raw(shell)  # the terminal's own line reading
