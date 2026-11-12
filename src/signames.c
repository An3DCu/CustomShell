#include "signames.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <strings.h>

/* The numbers come from signal.h, so they're right on any system. The
 * ones after POSIX's own list are Linux names, there only if the system
 * has them. An alias is accepted but never listed. */
static const struct {
    int sig;
    const char *name;
    bool alias;
} names[] = {
    { SIGHUP, "HUP", false },
    { SIGINT, "INT", false },
    { SIGQUIT, "QUIT", false },
    { SIGILL, "ILL", false },
    { SIGTRAP, "TRAP", false },
    { SIGABRT, "ABRT", false },
    { SIGBUS, "BUS", false },
    { SIGFPE, "FPE", false },
    { SIGKILL, "KILL", false },
    { SIGUSR1, "USR1", false },
    { SIGSEGV, "SEGV", false },
    { SIGUSR2, "USR2", false },
    { SIGPIPE, "PIPE", false },
    { SIGALRM, "ALRM", false },
    { SIGTERM, "TERM", false },
    { SIGCHLD, "CHLD", false },
    { SIGCONT, "CONT", false },
    { SIGSTOP, "STOP", false },
    { SIGTSTP, "TSTP", false },
    { SIGTTIN, "TTIN", false },
    { SIGTTOU, "TTOU", false },
    { SIGURG, "URG", false },
    { SIGXCPU, "XCPU", false },
    { SIGXFSZ, "XFSZ", false },
    { SIGVTALRM, "VTALRM", false },
    { SIGPROF, "PROF", false },
    { SIGSYS, "SYS", false },
#ifdef SIGSTKFLT
    { SIGSTKFLT, "STKFLT", false },
#endif
#ifdef SIGWINCH
    { SIGWINCH, "WINCH", false },
#endif
#ifdef SIGIO
    { SIGIO, "IO", false },
#endif
#ifdef SIGPOLL
    { SIGPOLL, "POLL", true },
#endif
#ifdef SIGPWR
    { SIGPWR, "PWR", false },
#endif
};

#define NNAMES (sizeof names / sizeof names[0])

int signame_parse(const char *text)
{
    if (text[0] >= '0' && text[0] <= '9') {
        char *end;
        long n;

        errno = 0;
        n = strtol(text, &end, 10);
        if (errno != 0 || *end != '\0' || n > INT_MAX)
            return -1;
        return (int)n;
    }
    if (strncasecmp(text, "SIG", 3) == 0)
        text += 3;
    for (size_t i = 0; i < NNAMES; i++) {
        if (strcasecmp(text, names[i].name) == 0)
            return names[i].sig;
    }
    return -1;
}

const char *signame_of(int sig)
{
    for (size_t i = 0; i < NNAMES; i++) {
        if (names[i].sig == sig && !names[i].alias)
            return names[i].name;
    }
    return NULL;
}

int signame_max(void)
{
    int max = 0;

    for (size_t i = 0; i < NNAMES; i++) {
        if (names[i].sig > max)
            max = names[i].sig;
    }
    return max;
}
