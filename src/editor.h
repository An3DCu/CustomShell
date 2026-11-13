#ifndef ACSH_EDITOR_H
#define ACSH_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "history.h"

/*
 * The line editor at the prompt, without readline.
 *
 * While a line is typed the terminal is in raw mode. There's no echo and no
 * line buffering. Ctrl+C is a key instead of SIGINT. acsh reads one key at
 * a time and draws the line itself with ANSI codes. Every terminal emulator
 * in use understands them. Raw mode ends before the line is returned.
 * Commands always get the terminal as it was.
 *
 *   Left, Right, Home, End, Ctrl+A, Ctrl+E   move the cursor
 *   Backspace, Delete                         remove a character
 *   Ctrl+K, Ctrl+U                            cut to the end, to the start
 *   Ctrl+W                                    cut the word before the cursor
 *   Ctrl+L                                    clear the screen
 *   Ctrl+C                                    drop the line
 *   Ctrl+D                                    leave on an empty line, else Delete
 *   Up, Down                                  older and newer lines from history
 *   Ctrl+R                                    search back through history
 *   Tab, Shift+Tab                            complete the word (complete.h): one
 *                                             match goes in, several are listed once
 *                                             and Tab and Shift+Tab go through them
 *
 * The cursor moves by byte. A letter like č is two bytes and two steps.
 */

/* Whether the editor can be used with this TERM: set, and not "dumb". */
bool editor_usable(const char *term);

struct editor_config {
    const char *prompt;         /* the prompt's last line, already printed;
                                 * the editor prints it again when it redraws */
    void (*repaint)(void *arg); /* for Ctrl+L: the whole prompt again */
    void *arg;
    const struct history *hist; /* for Up, Down and Ctrl+R, or NULL */
    const char *path;           /* $PATH and $HOME for Tab, or NULL */
    const char *home;
};

/*
 * Reads one line from the terminal fd, like input_read_line: the line with
 * its newline, or -1 with errno 0 for Ctrl+D on an empty line, or -1 with
 * errno EINTR for Ctrl+C.
 */
ssize_t editor_read_line(int fd, const struct editor_config *cfg, char **line, size_t *cap);

#endif
