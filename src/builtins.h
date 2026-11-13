#ifndef ACSH_BUILTINS_H
#define ACSH_BUILTINS_H

#include "shell.h"

/*
 * Built-ins run inside the shell process instead of a child. cd and exit
 * have to. A child could only change its own folder, or end itself. echo,
 * pwd and help are built in because it's faster and they're tiny.
 */

typedef int builtin_fn(struct shell *sh, int argc, char **argv);

struct builtin {
    const char *name;
    builtin_fn *run;
    const char *usage;   /* for help: "cd [dir | -]" */
    const char *summary; /* for help: one line on what it does */
    /* POSIX "special" built-ins (exit, export, unset). An assignment in
     * front of one stays. `X=2 export X` exports X=2. For the others,
     * `X=2 cd` only changes X while cd runs. */
    bool special;
};

/* The built-in with this name, or NULL. */
const struct builtin *builtin_find(const char *name);

/* The name of the i-th built-in, or NULL past the last one. Tab uses it. */
const char *builtin_name(size_t i);

/* Runs it with argv (NULL-terminated, argv[0] is the name) and returns its
 * exit status. Flushes stdout afterwards and reports a failed write.
 * `echo hi > /dev/full` then says so instead of failing quietly. */
int builtin_run(struct shell *sh, const struct builtin *b, char **argv);

#endif
