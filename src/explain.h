#ifndef ACSH_EXPLAIN_H
#define ACSH_EXPLAIN_H

#include <stdbool.h>
#include <stddef.h>

#include "parser.h"
#include "shell.h"

/*
 * Explain mode shows the plumbing. For each line it prints the plan (the
 * parsed line as a small tree), then every system call the shell makes for
 * it as it happens, with a short note, then the status:
 *
 *     plan
 *       pipeline of 2
 *       ├─ ls -l
 *       └─ wc -l  stdout > n.txt
 *     run
 *       pipe() = [3, 4]            what goes into 4 comes out of 3
 *       fork() = 4211              a child for ls
 *       [4211] dup2(4, 1)          stdout goes into the pipe
 *       ...
 *     status 0
 *
 * Turn it on for one line with `explain cmd ...`, or for every line with
 * `acsh --explain` or `set -o explain` (and off with `set +o explain`).
 *
 * The lines go to a copy of stderr made when explain mode starts, so a
 * command's own `2> f` doesn't swallow them. A child reports through a
 * pipe of its own that the shell reads before it goes on, and the pipe is
 * close-on-exec: it reaches end of file the moment exec works. That keeps
 * the lines in the order things happen, even with several children.
 */

/* Starts explain output, if it hasn't started yet. Returns false (with a
 * message) if stderr can't be copied. */
bool explain_start(struct shell *sh);

/* True while the current line is being explained. */
bool explain_on(const struct shell *sh);

/* The plan of a line, and the "run" heading under it. */
void explain_plan(struct shell *sh, const struct list *list);

/* One event: what was called, and a note on what it means. Lines from a
 * child start with its pid. */
void explain_event(struct shell *sh, const char *note, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The last line: "status N". */
void explain_status(struct shell *sh, int status);

/* The words of a command before and after expansion, when they differ. */
void explain_words(struct shell *sh, char *const *typed, size_t ntyped, char *const *argv);

/* exec: the call and the file it will run, found in PATH like execvp. */
void explain_exec(struct shell *sh, char *const *argv);

/* stdin, stdout, stderr, or "fd 7". */
const char *explain_fd_name(int fd, char *buf, size_t size);

#endif
