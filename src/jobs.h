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

/*
 * The jobs table: what runs in the background.
 *
 * The shell asks for children that ended with waitpid(-1, ..., WNOHANG)
 * at points where nothing runs in front. At the prompt that happens the
 * moment one ends: the SIGCHLD handler only writes a byte into a self-pipe,
 * and the prompt waits on the terminal and that pipe together (see
 * signals.h). In a script it happens before each command. Either way the
 * notices come just before the next prompt, as in bash.
 */
enum job_state {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE,
};

struct job {
    int id;             /* the n in %n */
    pid_t pgid;         /* its process group, or 0 without job control */
    pid_t *pids;        /* every process in it, in pipeline order */
    int *wstatus;       /* what waitpid said for each, once it ended */
    bool *ended;
    size_t npids;
    size_t nended;
    char *text;         /* the command as typed, on one line */
    enum job_state state;
};

/* Adds a job for pids (the job takes over the array). In an interactive
 * shell it says `job 1 started: sleep 30 (pid 4211)`. Returns its number. */
int jobs_add(struct shell *sh, pid_t pgid, pid_t *pids, size_t npids, const char *text);

/* Collects every child that ended or stopped since the last call, without
 * waiting. */
void jobs_reap(struct shell *sh);

/* In an interactive shell: `job 1 done: sleep 30 (exit 0)` for each job
 * that ended, and takes it off the list. */
void jobs_notify(struct shell *sh);

/* `jobs`: one line per job. Finished ones are shown once more, then
 * forgotten. */
void jobs_list(struct shell *sh);

/* Forgets every job, at exit. */
void jobs_free(struct shell *sh);

#endif
