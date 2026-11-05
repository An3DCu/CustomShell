#ifndef ACSH_EXEC_H
#define ACSH_EXEC_H

#include "parser.h"
#include "shell.h"

/* Runs a parsed pipeline and returns its exit status:
 *   0-255  the command's own exit status
 *   126    found, but it can't be run (permission denied, a directory)
 *   127    not found
 *   128+N  ended by signal N (130 for Ctrl+C, 143 for SIGTERM) */
int run_pipeline(struct shell *sh, const struct pipeline *pl);

/* Turns a status from waitpid into an exit status as above. For a crash,
 * also prints what happened ("Segmentation fault (core dumped)"); Ctrl+C
 * and a closed pipe stay quiet, as in bash. */
int exit_status_from_wait(int wstatus);

#endif
