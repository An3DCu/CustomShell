#include "jobs.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "error.h"
#include "explain.h"
#include "signals.h"
#include "xalloc.h"

static void ignore(int sig)
{
    struct sigaction sa = { 0 };

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void collect(void *sh)
{
    jobs_reap(sh);
}

bool jobs_init(struct shell *sh)
{
    pid_t pgid;
    int fd;

    if (!isatty(STDIN_FILENO))
        return false;
    /* Started in the background (`acsh &` from another shell): stop until
     * someone brings this group to the front, as bash and dash do. */
    while (tcgetpgrp(STDIN_FILENO) != (pgid = getpgrp()))
        kill(-pgid, SIGTTIN);

    ignore(SIGTSTP);
    ignore(SIGTTIN);
    ignore(SIGTTOU);

    /* A group of its own. A session leader (a terminal emulator starts its
     * shell like that) already leads its group, and setpgid says EPERM. */
    if (getpgrp() != getpid() && setpgid(0, 0) != 0) {
        shell_error(sh, "can't get a process group of its own: %s", strerror(errno));
        return false;
    }
    sh->pgid = getpgrp();

    /* Its own copy of the terminal, at 10 or above and close-on-exec: fd 0
     * can be redirected for a while (`while read l; do ...; done < f`),
     * and the terminal has to be at hand all the same. */
    fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
    if (fd < 0) {
        shell_error(sh, "terminal: %s", strerror(errno));
        return false;
    }
    sh->tty_fd = fd;
    tcsetpgrp(fd, sh->pgid);
    sh->have_tty_modes = tcgetattr(fd, &sh->tty_modes) == 0;
    sh->job_control = true;
    if (!signals_watch_children(collect, sh))
        shell_error(sh, "jobs: can't watch for children: %s", strerror(errno));
    return true;
}

void jobs_give_terminal(struct shell *sh, pid_t pgid)
{
    if (!sh->job_control)
        return;
    explain_event(sh, "the pipeline's group gets the terminal: Ctrl+C goes to it",
                  "tcsetpgrp(%d, %ld)", sh->tty_fd, (long)pgid);
    tcsetpgrp(sh->tty_fd, pgid);
}

void jobs_take_terminal(struct shell *sh)
{
    if (!sh->job_control)
        return;
    explain_event(sh, "the shell takes the terminal back", "tcsetpgrp(%d, %ld)", sh->tty_fd,
                  (long)sh->pgid);
    /* SIGTTOU is ignored, so a shell that isn't in front may do this. */
    tcsetpgrp(sh->tty_fd, sh->pgid);
    if (sh->have_tty_modes)
        tcsetattr(sh->tty_fd, TCSADRAIN, &sh->tty_modes);
}

void jobs_join_group(struct shell *sh, pid_t pgid, bool foreground)
{
    pid_t me = getpid();

    if (!sh->job_control)
        return;
    if (pgid == 0)
        pgid = me;
    explain_event(sh, pgid == me ? "a new process group, led by this child"
                                 : "joins the pipeline's group",
                  "setpgid(0, %ld)", (long)pgid);
    setpgid(0, pgid);
    /* Still with SIGTTOU ignored, as inherited from the shell: a child
     * that isn't in front yet may take the terminal without stopping. */
    if (foreground)
        tcsetpgrp(sh->tty_fd, pgid);
}

void jobs_set_group(struct shell *sh, pid_t pid, pid_t pgid)
{
    if (!sh->job_control)
        return;
    /* The child may have done it already, or even exec'ed (EACCES): both
     * are fine, the group is right either way. */
    if (setpgid(pid, pgid) != 0 && errno != EACCES && errno != ESRCH)
        shell_error(sh, "setpgid: %s", strerror(errno));
}

/* ---- the jobs table ------------------------------------------------------- */

/* The text on one line: newlines and runs of blanks become one space. */
static char *one_line(const char *text)
{
    char *out = xmalloc(strlen(text) + 1);
    size_t n = 0;

    for (const char *p = text; *p != '\0'; p++) {
        bool blank = *p == ' ' || *p == '\t' || *p == '\n';

        if (blank && (n == 0 || out[n - 1] == ' '))
            continue;
        out[n++] = blank ? ' ' : *p;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    return out;
}

int jobs_add(struct shell *sh, pid_t pgid, pid_t *pids, size_t npids, const char *text)
{
    struct job *job;
    int id = 1;

    /* One more than the highest number in use, as in bash. */
    for (size_t i = 0; i < sh->njobs; i++) {
        if (sh->jobs[i].id >= id)
            id = sh->jobs[i].id + 1;
    }
    if (sh->njobs == sh->jobs_cap) {
        sh->jobs_cap = sh->jobs_cap ? sh->jobs_cap * 2 : 8;
        sh->jobs = xreallocarray(sh->jobs, sh->jobs_cap, sizeof *sh->jobs);
    }
    job = &sh->jobs[sh->njobs++];
    *job = (struct job){
        .id = id,
        .pgid = pgid,
        .pids = pids,
        .wstatus = xcalloc(npids, sizeof *job->wstatus),
        .ended = xcalloc(npids, sizeof *job->ended),
        .npids = npids,
        .text = one_line(text),
        .state = JOB_RUNNING,
    };
    explain_event(sh, "in the background: the shell doesn't wait for it", "job %d", id);
    if (sh->interactive)
        fprintf(stderr, "job %d started: %s (pid %ld)\n", id, job->text, (long)pids[npids - 1]);
    return id;
}

static void job_free(struct job *job)
{
    free(job->pids);
    free(job->wstatus);
    free(job->ended);
    free(job->text);
}

static void remove_job(struct shell *sh, struct job *job)
{
    size_t i = (size_t)(job - sh->jobs);

    job_free(job);
    memmove(&sh->jobs[i], &sh->jobs[i + 1], (sh->njobs - i - 1) * sizeof *sh->jobs);
    sh->njobs--;
}

/* The job that has pid in it, or NULL. */
static struct job *find_pid(struct shell *sh, pid_t pid)
{
    for (size_t i = 0; i < sh->njobs; i++) {
        for (size_t k = 0; k < sh->jobs[i].npids; k++) {
            if (sh->jobs[i].pids[k] == pid)
                return &sh->jobs[i];
        }
    }
    return NULL;
}

/* What waitpid said about pid, written into its job. */
static void record(struct shell *sh, pid_t pid, int wstatus)
{
    struct job *job = find_pid(sh, pid);

    if (job == NULL)
        return; /* not one of ours: a stray child from before */
    for (size_t k = 0; k < job->npids; k++) {
        if (job->pids[k] != pid)
            continue;
        if (WIFSTOPPED(wstatus)) {
            job->state = JOB_STOPPED;
        } else if (WIFCONTINUED(wstatus)) {
            job->state = JOB_RUNNING;
        } else if (!job->ended[k]) {
            job->ended[k] = true;
            job->wstatus[k] = wstatus;
            if (++job->nended == job->npids)
                job->state = JOB_DONE;
        }
    }
}

void jobs_reap(struct shell *sh)
{
    pid_t pid;
    int wstatus;

    if (sh->njobs == 0)
        return;
    while ((pid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        record(sh, pid, wstatus);
}

/* "(exit 0)", or "(Terminated)" when a signal ended it. */
static void describe_end(int wstatus, char *buf, size_t size)
{
    if (WIFSIGNALED(wstatus))
        snprintf(buf, size, "(%s)", strsignal(WTERMSIG(wstatus)));
    else
        snprintf(buf, size, "(exit %d)", WEXITSTATUS(wstatus));
}


void jobs_notify(struct shell *sh)
{
    for (size_t i = 0; i < sh->njobs;) {
        struct job *job = &sh->jobs[i];
        char end[64];

        if (job->state != JOB_DONE) {
            i++;
            continue;
        }
        describe_end(job->wstatus[job->npids - 1], end, sizeof end);
        fprintf(stderr, "job %d done: %s %s\n", job->id, job->text, end);
        remove_job(sh, job); /* the next one moves to i */
    }
}

void jobs_list(struct shell *sh)
{
    static const char *const names[] = { "running", "stopped", "done" };

    jobs_reap(sh);
    for (size_t i = 0; i < sh->njobs;) {
        struct job *job = &sh->jobs[i];
        char end[64] = "";

        if (job->state == JOB_DONE) {
            end[0] = ' ';
            describe_end(job->wstatus[job->npids - 1], end + 1, sizeof end - 1);
        }
        printf("job %d  %-8s %s%s\n", job->id, names[job->state], job->text, end);
        if (job->state == JOB_DONE && sh->interactive)
            remove_job(sh, job);
        else
            i++;
    }
}

void jobs_free(struct shell *sh)
{
    for (size_t i = 0; i < sh->njobs; i++)
        job_free(&sh->jobs[i]);
    free(sh->jobs);
    sh->jobs = NULL;
    sh->njobs = sh->jobs_cap = 0;
}
