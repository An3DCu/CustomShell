#ifndef ACSH_SHELL_H
#define ACSH_SHELL_H

#include <stdbool.h>
#include <sys/types.h>

#include "strvec.h"
#include "vars.h"

/* What the shell remembers between commands. */
struct shell {
    bool interactive; /* stdin and stderr are a terminal: show a prompt */
    int status;       /* exit status of the last command: $? */
    bool exiting;     /* set by the exit built-in */
    bool interrupted; /* Ctrl+C stopped the last command */
    pid_t pid;        /* the shell's own pid: $$, the same in every child */
    const char *name; /* $0: how acsh was started, or the script's name */
    struct strvec params; /* $1, $2, ...: the script's arguments */
    int source_depth; /* how many `source` calls are running, to stop a loop */
    struct vars vars;
};

#endif
