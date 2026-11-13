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
#include "exec.h"
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
     * shell like that) already leads its group. setpgid says EPERM then. */
    if (getpgrp() != getpid() && setpgid(0, 0) != 0) {
        shell_error(sh, "can't get a process group of its own: %s", strerror(errno));
        return false;
    }
    sh->pgid = getpgrp();

    /* Its own copy of the terminal, at 10 or above and close-on-exec. fd 0
     * can be redirected for a while (`while read l; do ...; done < f`).
     * The terminal has to be at hand all the same. */
    fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
    if (fd < 0) {
        shell_error(sh, "terminal: %s", high_fd_error(errno));
        return false;
    }
    sh->tty_fd = fd;
    tcsetpgrp(fd, sh->pgid);
    sh->have_tty_modes = tcgetattr(fd, &sh->tty_modes) == 0;
    sh->job_control = true;
    if (!signals_watch_children(collect, sh))
        shell_error(sh, "jobs: can't watch for children: %s", high_fd_error(errno));
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
    /* SIGTTOU is ignored. A shell that isn't in front may then do this. */
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
    /* Still with SIGTTOU ignored, as inherited from the shell. A child that
     * isn't in front yet may take the terminal without stopping. */
    if (foreground)
        tcsetpgrp(sh->tty_fd, pgid);
}

void jobs_set_group(struct shell *sh, pid_t pid, pid_t pgid)
{
    if (!sh->job_control)
        return;
    /* The child may have done it already, or even exec'ed (EACCES). Both
     * are fine. The group is right either way. */
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

/* A new entry at the end of the table, numbered one more than the highest
 * in use, as in bash. */
static struct job *new_job(struct shell *sh, pid_t pgid, pid_t *pids, size_t npids,
                           const char *text)
{
    struct job *job;
    int id = 1;

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
    return job;
}

int jobs_add(struct shell *sh, pid_t pgid, pid_t *pids, size_t npids, const char *text)
{
    struct job *job = new_job(sh, pgid, pids, npids, text);

    job->turn = ++sh->job_turns;
    explain_event(sh, "in the background: the shell doesn't wait for it", "job %d", job->id);
    if (sh->interactive)
        fprintf(stderr, "job %d started: %s (pid %ld)\n", job->id, job->text,
                (long)pids[npids - 1]);
    return job->id;
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

struct job *jobs_find_pid(struct shell *sh, pid_t pid)
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
    struct job *job = jobs_find_pid(sh, pid);

    if (job == NULL)
        return; /* not one of ours: a stray child from before */
    for (size_t k = 0; k < job->npids; k++) {
        if (job->pids[k] != pid)
            continue;
        if (WIFSTOPPED(wstatus)) {
            if (job->state == JOB_RUNNING) {
                /* in the background, as one in front is waited for apart */
                job->turn = ++sh->job_turns;
                job->tell_stop = true;
            }
            job->state = JOB_STOPPED;
        } else if (WIFCONTINUED(wstatus)) {
            job->state = JOB_RUNNING;
            job->tell_stop = false;
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

        if (job->state == JOB_STOPPED && job->tell_stop) {
            job->tell_stop = false;
            fprintf(stderr, "job %d stopped: %s\n", job->id, job->text);
        }
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

/* Whether a should be the current job before b: a stopped job first, then
 * the one that stopped or went to the background last. */
static bool comes_before(const struct job *a, const struct job *b)
{
    bool a_stopped = a->state == JOB_STOPPED;
    bool b_stopped = b->state == JOB_STOPPED;

    if (a_stopped != b_stopped)
        return a_stopped;
    return a->turn > b->turn;
}

/* The current job, leaving out skip: %+, or with skip = %+, %-. */
static struct job *current(struct shell *sh, const struct job *skip)
{
    struct job *best = NULL;

    for (size_t i = 0; i < sh->njobs; i++) {
        struct job *job = &sh->jobs[i];

        if (job != skip && (best == NULL || comes_before(job, best)))
            best = job;
    }
    return best;
}

struct job *jobs_find(struct shell *sh, const char *spec, char *err, size_t errlen)
{
    char *end;
    long n;

    if (strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0) {
        struct job *job = current(sh, NULL);

        if (job != NULL)
            return job;
    } else if (strcmp(spec, "%-") == 0) {
        struct job *job = current(sh, current(sh, NULL));

        if (job != NULL)
            return job;
    } else {
        if (spec[0] != '%' || spec[1] == '\0') {
            snprintf(err, errlen, "%s: not a job (jobs are %%1, %%2, ...)", spec);
            return NULL;
        }
        n = strtol(spec + 1, &end, 10);
        if (*end != '\0' || n < 1) {
            snprintf(err, errlen, "%s: not a job (jobs are %%1, %%2, ...)", spec);
            return NULL;
        }
        for (size_t i = 0; i < sh->njobs; i++) {
            if (sh->jobs[i].id == n)
                return &sh->jobs[i];
        }
    }
    snprintf(err, errlen, "%s: no such job", spec);
    return NULL;
}

/* Waits for one process of a job. false after Ctrl+C. At the prompt it
 * sleeps on the self-pipe and collects with jobs_reap. In a script waitpid
 * blocks. There's no pipe there. Ctrl+C ends the script anyway. */
static bool wait_one(struct shell *sh, struct job *job, size_t k)
{
    while (!job->ended[k]) {
        int wstatus;
        pid_t got;

        if (sh->job_control) {
            jobs_reap(sh);
            if (job->ended[k])
                break;
            if (!signals_sleep_until_child())
                return false;
            continue;
        }
        got = waitpid(job->pids[k], &wstatus, 0);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            /* gone already, without us seeing it: count it as ended */
            job->ended[k] = true;
            if (++job->nended == job->npids)
                job->state = JOB_DONE;
            break;
        }
        record(sh, got, wstatus);
    }
    return true;
}

/* Ctrl+C during wait: the jobs get it too, as if they were in front. */
static int interrupted(struct shell *sh, struct job *job)
{
    if (job->pgid > 0)
        kill(-job->pgid, SIGINT);
    sh->interrupted = true;
    return 130;
}

int jobs_wait_pid(struct shell *sh, struct job *job, pid_t pid)
{
    int status = 0;

    for (size_t k = 0; k < job->npids; k++) {
        if (job->pids[k] != pid)
            continue;
        if (!wait_one(sh, job, k))
            return interrupted(sh, job);
        status = exit_status_from_wait(job->wstatus[k]);
    }
    if (job->nended == job->npids)
        remove_job(sh, job);
    return status;
}

int jobs_wait(struct shell *sh, struct job *job)
{
    int status;

    for (size_t k = 0; k < job->npids; k++) {
        if (!wait_one(sh, job, k))
            return interrupted(sh, job);
    }
    status = exit_status_from_wait(job->wstatus[job->npids - 1]);
    remove_job(sh, job);
    return status;
}

int jobs_wait_all(struct shell *sh)
{
    while (sh->njobs > 0) {
        if (jobs_wait(sh, &sh->jobs[0]) == 130 && sh->interrupted) {
            for (size_t i = 0; i < sh->njobs; i++)
                interrupted(sh, &sh->jobs[i]);
            return 130;
        }
    }
    return 0;
}

/* ---- in front: fg, bg and Ctrl+Z ---------------------------------------- */

/* The status for wstatus without printing anything: 128 + N for signal N. */
static int quiet_status(int wstatus)
{
    if (WIFSIGNALED(wstatus))
        return 128 + WTERMSIG(wstatus);
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
}

static void explain_end(struct shell *sh, pid_t pid, int wstatus)
{
    if (!explain_on(sh))
        return;
    if (WIFEXITED(wstatus))
        explain_event(sh, NULL, "waitpid(%ld) = exit %d", (long)pid, WEXITSTATUS(wstatus));
    else if (WIFSIGNALED(wstatus))
        explain_event(sh, strsignal(WTERMSIG(wstatus)), "waitpid(%ld) = killed by signal %d",
                      (long)pid, WTERMSIG(wstatus));
}

/*
 * Waits for each process of the job in front, in pipeline order. explain
 * then shows them in that order. True once all have ended, with the last
 * one's status in *status. False as soon as one stops. Ctrl+Z goes to the
 * whole group. The others stop too. Without job control nothing stops
 * on its own. A script and its commands share one group and stop together.
 */
static bool wait_in_front(struct shell *sh, struct job *job, int *status)
{
    int flags = sh->job_control ? WUNTRACED : 0;
    size_t last = job->npids - 1;

    *status = job->ended[last] ? quiet_status(job->wstatus[last]) : 0;
    for (size_t k = 0; k < job->npids; k++) {
        pid_t pid = job->pids[k];
        int wstatus;
        int s;

        if (job->ended[k])
            continue;
        /* A signal can interrupt waitpid with EINTR. The child is still
         * there. Keep waiting for it. */
        while (waitpid(pid, &wstatus, flags) < 0) {
            if (errno == EINTR)
                continue;
            shell_error(sh, "waitpid: %s", strerror(errno));
            job->ended[k] = true;
            job->nended++;
            if (k == last)
                *status = 1;
            break;
        }
        if (job->ended[k])
            continue;
        if (WIFSTOPPED(wstatus)) {
            explain_event(sh, "Ctrl+Z: the job stops", "waitpid(%ld) = stopped by signal %d",
                          (long)pid, WSTOPSIG(wstatus));
            return false;
        }
        job->ended[k] = true;
        job->wstatus[k] = wstatus;
        job->nended++;
        if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGINT)
            sh->interrupted = true;
        explain_end(sh, pid, wstatus);
        s = exit_status_from_wait(wstatus);
        if (k == last)
            *status = s;
    }
    job->state = JOB_DONE;
    return true;
}

/* The job in front has ended or stopped. The shell takes the terminal back.
 * A stopped job keeps its terminal modes. A stopped editor then comes back
 * as it was. The shell gets its own modes back. */
static int leave_front(struct shell *sh, struct job *job, bool ended, int status)
{
    if (ended) {
        jobs_take_terminal(sh);
        remove_job(sh, job);
        return status;
    }
    job->have_modes = tcgetattr(sh->tty_fd, &job->modes) == 0;
    jobs_take_terminal(sh);
    job->state = JOB_STOPPED;
    job->turn = ++sh->job_turns;
    job->tell_stop = false;
    /* The terminal shows ^Z but no newline. */
    fprintf(stderr, "\njob %d stopped: %s\n", job->id, job->text);
    sh->stopped = true;
    return 0;
}

int jobs_run_in_front(struct shell *sh, pid_t pgid, pid_t *pids, size_t npids,
                      const char *text)
{
    /* It's in the table from the start. A stop only has to keep it there.
     * Nothing else adds jobs while it runs. */
    struct job *job = new_job(sh, pgid, pids, npids, text);
    int status;
    bool ended = wait_in_front(sh, job, &status);

    return leave_front(sh, job, ended, status);
}

/* fg or bg on a job that ended but wasn't reported: report it now. */
static int report_done(struct shell *sh, struct job *job)
{
    char end[64];
    int status = quiet_status(job->wstatus[job->npids - 1]);

    describe_end(job->wstatus[job->npids - 1], end, sizeof end);
    fprintf(stderr, "job %d done: %s %s\n", job->id, job->text, end);
    remove_job(sh, job);
    return status;
}

/* Sends SIGCONT to the whole group: every process of a stopped pipeline
 * goes on. */
static void resume(struct shell *sh, struct job *job)
{
    explain_event(sh, "the job goes on", "kill(%ld, SIGCONT)", (long)-job->pgid);
    kill(-job->pgid, SIGCONT);
    job->state = JOB_RUNNING;
    job->tell_stop = false;
}

int jobs_fg(struct shell *sh, struct job *job)
{
    int status;
    bool ended;

    jobs_reap(sh);
    if (job->state == JOB_DONE)
        return report_done(sh, job);
    printf("%s\n", job->text);
    fflush(stdout);
    if (job->have_modes) {
        explain_event(sh, "the terminal as the job left it", "tcsetattr(%d)", sh->tty_fd);
        tcsetattr(sh->tty_fd, TCSADRAIN, &job->modes);
    }
    jobs_give_terminal(sh, job->pgid);
    if (job->state == JOB_STOPPED)
        resume(sh, job);
    ended = wait_in_front(sh, job, &status);
    return leave_front(sh, job, ended, status);
}

int jobs_bg(struct shell *sh, struct job *job)
{
    jobs_reap(sh);
    if (job->state == JOB_DONE)
        return report_done(sh, job);
    if (job->state == JOB_RUNNING) {
        shell_error(sh, "bg: job %d is already running", job->id);
        return 0;
    }
    /* the notice first. It comes before anything the job prints. */
    fprintf(stderr, "job %d running: %s\n", job->id, job->text);
    job->turn = ++sh->job_turns;
    resume(sh, job);
    return 0;
}

/* ---- leaving: exit, Ctrl+D, disown -------------------------------------- */

bool jobs_may_leave(struct shell *sh)
{
    char *names = NULL;
    size_t len = 0;
    FILE *list;

    if (!sh->job_control)
        return true;
    jobs_reap(sh);
    if (sh->exit_warned != 0 && sh->exit_warned + 1 == sh->commands)
        return true; /* warned by the command just before this one */
    list = open_memstream(&names, &len);
    if (list == NULL)
        return true;
    for (size_t i = 0; i < sh->njobs; i++) {
        if (sh->jobs[i].state == JOB_STOPPED)
            fprintf(list, "%sjob %d (%s)", ftell(list) > 0 ? ", " : "", sh->jobs[i].id,
                    sh->jobs[i].text);
    }
    fclose(list);
    if (len == 0) {
        free(names);
        return true;
    }
    shell_error(sh, "stopped jobs: %s", names);
    shell_error(sh, "exit again to leave; they'll get SIGHUP");
    free(names);
    sh->exit_warned = sh->commands;
    return false;
}

void jobs_hang_up(struct shell *sh)
{
    if (!sh->job_control)
        return;
    jobs_reap(sh);
    for (size_t i = 0; i < sh->njobs; i++) {
        struct job *job = &sh->jobs[i];

        if (job->state == JOB_DONE || job->pgid <= 0)
            continue;
        kill(-job->pgid, SIGHUP);
        if (job->state == JOB_STOPPED)
            kill(-job->pgid, SIGCONT);
    }
}

void jobs_disown(struct shell *sh, struct job *job)
{
    remove_job(sh, job);
}

void jobs_free(struct shell *sh)
{
    for (size_t i = 0; i < sh->njobs; i++)
        job_free(&sh->jobs[i]);
    free(sh->jobs);
    sh->jobs = NULL;
    sh->njobs = sh->jobs_cap = 0;
}
