#ifndef ACSH_SCRIPT_H
#define ACSH_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#include "shell.h"

/*
 * Where commands come from: the terminal, a pipe, a script file, the text
 * after -c, or a file given to `source`. They all run through the same
 * loop. It reads a line. If the command isn't finished (an open quote, a
 * line ending in | or &&, a \ at the end), it reads the next line too.
 * Then it parses and runs.
 */
struct reader {
    int fd;             /* read from here, one byte at a time; -1 for text */
    const char *text;   /* or from this string (acsh -c) */
    size_t pos;
    bool interactive;   /* prompts, with Ctrl+C dropping the line being typed */
    const char *name;   /* for errors, "build.sh: line 3", or NULL for stdin */
    int lines;          /* how many lines it has given so far */
};

/*
 * Runs every command from r until the end, `exit`, or a syntax error in a
 * script. A script stops there with status 2, as in bash and dash. At the
 * prompt the shell goes on. Returns the status of the last command, or 0
 * if there was none.
 */
int script_run(struct shell *sh, struct reader *r);

/*
 * Opens a script to read. It's close-on-exec and moved to fd 10 or above.
 * No command inherits it. `3< file` in the script can't replace it.
 * Returns the fd, or -1 with errno set (EISDIR for a folder).
 */
int script_open(const char *path);

#endif
