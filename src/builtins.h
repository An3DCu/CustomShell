#ifndef ACSH_BUILTINS_H
#define ACSH_BUILTINS_H

#include "shell.h"

/*
 * Built-ins run inside the shell process instead of a child. cd and exit
 * have to: a child could only change its own folder, or end itself. echo,
 * pwd and help are built in because it's faster and they're tiny.
 */

typedef int builtin_fn(struct shell *sh, int argc, char **argv);

struct builtin {
    const char *name;
    builtin_fn *run;
    const char *usage;   /* for help: "cd [dir | -]" */
    const char *summary; /* for help: one line on what it does */
};

/* The built-in with this name, or NULL. */
const struct builtin *builtin_find(const char *name);

/* Runs it with argv (NULL-terminated, argv[0] is the name) and returns its
 * exit status. Flushes stdout afterwards and reports a failed write, so
 * `echo hi > /dev/full` says so instead of failing quietly. */
int builtin_run(struct shell *sh, const struct builtin *b, char **argv);

#endif
