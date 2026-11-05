#ifndef ACSH_SHELL_H
#define ACSH_SHELL_H

#include <stdbool.h>

/* What the shell remembers between commands. */
struct shell {
    bool interactive; /* stdin and stderr are a terminal: show a prompt */
    int status;       /* exit status of the last command, later $? */
    bool exiting;     /* set by the exit built-in */
};

#endif
