#ifndef ACSH_EDITOR_H
#define ACSH_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * The line editor at the prompt, without readline.
 *
 * While a line is typed the terminal is in raw mode: no echo, no line
 * buffering, and Ctrl+C is a key instead of SIGINT. acsh reads one key at a
 * time and draws the line itself with ANSI codes, which every terminal
 * emulator in use understands. Raw mode ends before the line is returned,
 * so commands always get the terminal as it was.
 *
 *   Left, Right, Home, End, Ctrl+A, Ctrl+E   move the cursor
 *   Backspace, Delete                         remove a character
 *   Ctrl+K, Ctrl+U                            cut to the end, to the start
 *   Ctrl+W                                    cut the word before the cursor
 *   Ctrl+L                                    clear the screen
 *   Ctrl+C                                    drop the line
 *   Ctrl+D                                    leave on an empty line, else Delete
 *
 * The cursor moves by byte: a letter like č is two bytes and two steps.
 */

/* Whether the editor can be used with this TERM: set, and not "dumb". */
bool editor_usable(const char *term);

/*
 * Reads one line from the terminal fd, like input_read_line: the line with
 * its newline, or -1 with errno 0 for Ctrl+D on an empty line, or -1 with
 * errno EINTR for Ctrl+C. prompt is the last line of the prompt, already
 * printed, which the editor prints again when it redraws. For Ctrl+L,
 * repaint(arg) prints the whole prompt after the screen is cleared.
 */
ssize_t editor_read_line(int fd, const char *prompt, void (*repaint)(void *), void *arg,
                         char **line, size_t *cap);

#endif
