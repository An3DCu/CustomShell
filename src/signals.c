#include "signals.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t got_sigint;

/* The self-pipe: the SIGCHLD handler writes a byte into [1], and the
 * prompt's pselect sees [0] become readable. -1 until signals_watch_children
 * sets it up, and always -1 in a script. */
static int child_pipe[2] = { -1, -1 };
static void (*child_hook)(void *);
static void *child_hook_arg;

/* Runs only inside pselect(), the one place SIGINT is unblocked. */
static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
}

/* A child ended or stopped. Only write() is used, which is safe in a
 * handler; errno is put back so the code it interrupted doesn't see this
 * handler's result. The write end doesn't block: if the pipe is full,
 * there's a byte waiting already, and that's all the prompt needs. */
static void on_sigchld(int sig)
{
    int saved = errno;

    (void)sig;
    if (child_pipe[1] >= 0)
        (void)!write(child_pipe[1], "c", 1);
    errno = saved;
}

static void set_handler_flags(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa = { 0 };

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    sigaction(sig, &sa, NULL);
}

static void set_handler(int sig, void (*handler)(int))
{
    set_handler_flags(sig, handler, 0); /* no SA_RESTART: pselect must return with EINTR */
}

static void change_sigint_mask(int how)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigprocmask(how, &set, NULL);
}

void signals_init_interactive(void)
{
    set_handler(SIGINT, on_sigint);
    set_handler(SIGQUIT, SIG_IGN);
    change_sigint_mask(SIG_BLOCK);
}

/* Moves fd to 10 or above, close-on-exec, and makes it non-blocking. */
static int move_high(int fd)
{
    int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);

    close(fd);
    if (high >= 0)
        fcntl(high, F_SETFL, fcntl(high, F_GETFL) | O_NONBLOCK);
    return high;
}

bool signals_watch_children(void (*hook)(void *), void *arg)
{
    int fds[2];

    if (pipe(fds) != 0)
        return false;
    child_pipe[0] = move_high(fds[0]);
    child_pipe[1] = move_high(fds[1]);
    if (child_pipe[0] < 0 || child_pipe[1] < 0)
        return false;
    child_hook = hook;
    child_hook_arg = arg;
    /* SA_RESTART: a read() or waitpid() that SIGCHLD interrupts starts
     * again by itself instead of failing with EINTR. pselect never
     * restarts, which is what the prompt wants. */
    set_handler_flags(SIGCHLD, on_sigchld, SA_RESTART);
    return true;
}

/* Empties the self-pipe and lets the hook collect the children. */
static void children_changed(void)
{
    char buf[64];

    while (read(child_pipe[0], buf, sizeof buf) > 0)
        ;
    if (child_hook != NULL)
        child_hook(child_hook_arg);
}

bool signals_wait_for_input(int fd)
{
    sigset_t while_waiting;

    /* The current mask without SIGINT: what pselect uses while it waits. */
    sigprocmask(SIG_BLOCK, NULL, &while_waiting);
    sigdelset(&while_waiting, SIGINT);
    for (;;) {
        fd_set readable;
        int nfds = fd + 1;

        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        if (child_pipe[0] >= 0) {
            FD_SET(child_pipe[0], &readable);
            if (child_pipe[0] >= nfds)
                nfds = child_pipe[0] + 1;
        }
        got_sigint = 0;
        if (pselect(nfds, &readable, NULL, NULL, NULL, &while_waiting) >= 0) {
            if (child_pipe[0] >= 0 && FD_ISSET(child_pipe[0], &readable))
                children_changed();
            if (FD_ISSET(fd, &readable))
                return true;
            continue; /* only a child: keep waiting for the user */
        }
        if (errno != EINTR)
            return false;
        if (got_sigint)
            return false; /* errno is EINTR */
        /* some other signal, like SIGCHLD itself: keep waiting */
    }
}

void signals_drop_pending(void)
{
    sigset_t pending;

    sigpending(&pending);
    if (sigismember(&pending, SIGINT)) {
        /* POSIX: setting a pending signal to SIG_IGN discards it, even
         * while it's blocked. Then the handler goes back. */
        set_handler(SIGINT, SIG_IGN);
        set_handler(SIGINT, on_sigint);
    }
}

bool signals_take_interrupt(void)
{
    sigset_t pending;

    sigpending(&pending);
    if (!sigismember(&pending, SIGINT))
        return false;
    signals_drop_pending();
    return true;
}

void signals_reset_in_child(void)
{
    set_handler(SIGINT, SIG_DFL);
    set_handler(SIGQUIT, SIG_DFL);
    set_handler(SIGTSTP, SIG_DFL);
    set_handler(SIGTTIN, SIG_DFL);
    set_handler(SIGTTOU, SIG_DFL);
    set_handler(SIGCHLD, SIG_DFL);
    change_sigint_mask(SIG_UNBLOCK);
}
