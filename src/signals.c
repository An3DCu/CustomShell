#include "signals.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/select.h>

static volatile sig_atomic_t got_sigint;

/* Runs only inside pselect(), the one place SIGINT is unblocked. */
static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
}

static void set_handler(int sig, void (*handler)(int))
{
    struct sigaction sa = { 0 };

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: pselect must return with EINTR */
    sigaction(sig, &sa, NULL);
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

bool signals_wait_for_input(int fd)
{
    sigset_t while_waiting;

    /* The current mask without SIGINT: what pselect uses while it waits. */
    sigprocmask(SIG_BLOCK, NULL, &while_waiting);
    sigdelset(&while_waiting, SIGINT);
    for (;;) {
        fd_set readable;

        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        got_sigint = 0;
        if (pselect(fd + 1, &readable, NULL, NULL, NULL, &while_waiting) >= 0)
            return true;
        if (errno != EINTR)
            return false;
        if (got_sigint)
            return false; /* errno is EINTR */
        /* some other signal: keep waiting */
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
    change_sigint_mask(SIG_UNBLOCK);
}
