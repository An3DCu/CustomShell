#ifndef ACSH_SHELL_H
#define ACSH_SHELL_H

#include <stdbool.h>
#include <sys/types.h>
#include <termios.h>

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
    int loop_depth;   /* how many while, until and for loops are running */
    int breaks;       /* break n: how many loops still have to end */
    int continues;    /* continue n: the same, the last one goes on instead */
    bool explain_all; /* set -o explain or --explain: explain every line */
    bool explaining;  /* the line running now is being explained */
    int explain_fd;   /* the copy of stderr explain mode writes to; 0 until it starts */
    int explain_out;  /* where explain lines go now: explain_fd, or a child's report pipe */
    const char *where; /* the script running now, for errors; NULL at the prompt or from a pipe */
    int line;         /* the line in it of the command running now */
    int unit_line;    /* the line where the command being run starts; commands count from it */
    bool job_control; /* interactive, on a terminal: pipelines get process groups (jobs.h) */
    pid_t pgid;       /* the shell's own process group */
    int tty_fd;       /* the shell's copy of the terminal, at 10 or above */
    struct termios tty_modes; /* the terminal as the shell wants it, to put back after a job */
    bool have_tty_modes;
    struct vars vars;
};

#endif
