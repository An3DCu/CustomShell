#ifndef ACSH_JOBS_H
#define ACSH_JOBS_H

#include <stdbool.h>
#include <sys/types.h>

#include "shell.h"

/*
 * Job control: which processes own the terminal.
 *
 * The terminal sends Ctrl+C, Ctrl+\ and Ctrl+Z to one process group, its
 * foreground group, and only that group may read from it. So an
 * interactive shell puts every pipeline into a process group of its own,
 * gives that group the terminal while it runs, and takes the terminal back
 * when it's done. Ctrl+C then reaches the pipeline and not the shell, and a
 * pipeline can never read what you type for the shell.
 *
 *     shell    pgid 100  (the terminal is here at the prompt)
 *     ls | wc  pgid 4211 (ls is 4211, wc 4212; the terminal is here while they run)
 *
 * The group is set with setpgid() in the child and in the shell both:
 * whichever runs first, the group exists before the shell hands it the
 * terminal or a second child joins it.
 *
 * Only an interactive shell does any of this. A script's commands stay in
 * the script's own group, as in every other shell.
 */

/*
 * Starts job control for an interactive shell: waits until the shell is
 * in the foreground (if it was started in the background, it stops itself
 * until it's brought to the front), moves it into a group of its own,
 * takes the terminal and remembers its modes, and ignores the signals that
 * stop a job (SIGTSTP, SIGTTIN, SIGTTOU), which the commands get back.
 * Returns false and leaves job control off if stdin isn't the terminal.
 */
bool jobs_init(struct shell *sh);

/* Hands the terminal to group pgid. */
void jobs_give_terminal(struct shell *sh, pid_t pgid);

/* Takes the terminal back for the shell and puts the shell's terminal
 * modes back, in case the command changed them (a crashed editor can
 * leave the terminal raw). */
void jobs_take_terminal(struct shell *sh);

/* In a child just forked for a pipeline: joins group pgid (0 means start a
 * new one with this child's pid) and, in the foreground, takes the
 * terminal for it. */
void jobs_join_group(struct shell *sh, pid_t pgid, bool foreground);

/* In the shell, right after forking pid: the same setpgid, from this side. */
void jobs_set_group(struct shell *sh, pid_t pid, pid_t pgid);

#endif
