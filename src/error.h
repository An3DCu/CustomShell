#ifndef ACSH_ERROR_H
#define ACSH_ERROR_H

#include "shell.h"

/*
 * Every error the shell reports while it runs commands goes through here:
 *
 *     acsh: nope: command not found                       at the prompt
 *     acsh: build.sh: line 3: nope: command not found     in a script
 *
 * In a script file, -c text or a sourced file, the place goes in front, as
 * bash does, so a long script tells you where to look. Reading commands
 * from the terminal or a pipe, the message stays short.
 */
void shell_error(const struct shell *sh, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * A hint under an error, for a mistake that's easy to make:
 *
 *     acsh: X: command not found
 *       hint: no spaces around = in an assignment: X=1
 *
 * Only in an interactive shell: a script's output shouldn't change because
 * a person might be reading it.
 */
void shell_hint(const struct shell *sh, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Why moving an fd to 10 or above failed (errno err from fcntl F_DUPFD).
 * The shell keeps its own fds there, away from the 0 to 9 people use, and
 * a low `ulimit -n` can leave no room: then it says so, with the limit,
 * instead of "Invalid argument". Otherwise strerror(err).
 */
const char *high_fd_error(int err);

#endif
