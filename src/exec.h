#ifndef ACSH_EXEC_H
#define ACSH_EXEC_H

#include "parser.h"
#include "shell.h"

/* Runs a parsed list, pipeline by pipeline, and returns the last exit
 * status. sh->status is updated after each pipeline. In `false; echo $?`
 * the echo sees 1. A pipeline's status is its last command's:
 *   0-255  the command's own exit status
 *   126    found, but it can't be run (permission denied, a directory)
 *   127    not found
 *   128+N  ended by signal N (130 for Ctrl+C, 143 for SIGTERM)
 * The list stops early after `exit`. It also stops after Ctrl+C stopped a
 * command, as in bash. `sleep 10; echo after` prints nothing if you press
 * Ctrl+C. */
int run_list(struct shell *sh, const struct list *list);

/* Turns a status from waitpid into an exit status as above. For a crash,
 * also prints what happened ("Segmentation fault (core dumped)"); Ctrl+C
 * and a closed pipe stay quiet, as in bash. */
int exit_status_from_wait(int wstatus);

#endif
