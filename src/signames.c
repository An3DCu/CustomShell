#include "signames.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <strings.h>

/* The numbers come from signal.h. They're right on any system. The ones
 * after POSIX's own list are Linux names, there only if the system has
 * them. An alias is accepted. It's never listed. */
static const struct {
    const char *name;
    int sig;
    bool alias;
} names[] = {
    { "HUP", SIGHUP, false },
    { "INT", SIGINT, false },
    { "QUIT", SIGQUIT, false },
    { "ILL", SIGILL, false },
    { "TRAP", SIGTRAP, false },
    { "ABRT", SIGABRT, false },
    { "BUS", SIGBUS, false },
    { "FPE", SIGFPE, false },
    { "KILL", SIGKILL, false },
    { "USR1", SIGUSR1, false },
    { "SEGV", SIGSEGV, false },
    { "USR2", SIGUSR2, false },
    { "PIPE", SIGPIPE, false },
    { "ALRM", SIGALRM, false },
    { "TERM", SIGTERM, false },
    { "CHLD", SIGCHLD, false },
    { "CONT", SIGCONT, false },
    { "STOP", SIGSTOP, false },
    { "TSTP", SIGTSTP, false },
    { "TTIN", SIGTTIN, false },
    { "TTOU", SIGTTOU, false },
    { "URG", SIGURG, false },
    { "XCPU", SIGXCPU, false },
    { "XFSZ", SIGXFSZ, false },
    { "VTALRM", SIGVTALRM, false },
    { "PROF", SIGPROF, false },
    { "SYS", SIGSYS, false },
#ifdef SIGSTKFLT
    { "STKFLT", SIGSTKFLT, false },
#endif
#ifdef SIGWINCH
    { "WINCH", SIGWINCH, false },
#endif
#ifdef SIGIO
    { "IO", SIGIO, false },
#endif
#ifdef SIGPOLL
    { "POLL", SIGPOLL, true },
#endif
#ifdef SIGPWR
    { "PWR", SIGPWR, false },
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
