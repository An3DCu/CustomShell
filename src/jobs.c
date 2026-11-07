#include "jobs.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "error.h"
#include "explain.h"

static void ignore(int sig)
{
    struct sigaction sa = { 0 };

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
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
