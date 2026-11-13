#include "error.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* The message is put together first and written with one write(). Two
 * children reporting at the same time can't mix up each other's lines. */
static void say(const char *lead, const char *where, int line, const char *fmt, va_list ap)
    __attribute__((format(printf, 4, 0)));

static void say(const char *lead, const char *where, int line, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s", lead);

    if (where != NULL && n >= 0 && (size_t)n < sizeof buf)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s: line %d: ", where, line);
    if (n >= 0 && (size_t)n < sizeof buf)
        n += vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf - 1)
        n = (int)sizeof buf - 2; /* cut off. It's still one line. */
    buf[n++] = '\n';
    fflush(stderr); /* nothing should be waiting. This keeps the order anyway. */
    (void)!write(STDERR_FILENO, buf, (size_t)n);
}

void shell_error(const struct shell *sh, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    say("acsh: ", sh ? sh->where : NULL, sh ? sh->line : 0, fmt, ap);
    va_end(ap);
}

void shell_hint(const struct shell *sh, const char *fmt, ...)
{
    va_list ap;

    if (sh == NULL || !sh->interactive)
        return;
    va_start(ap, fmt);
    say("  hint: ", NULL, 0, fmt, ap);
    va_end(ap);
}

const char *high_fd_error(int err)
{
    static char reason[96];
    struct rlimit lim;

    if ((err == EINVAL || err == EMFILE) && getrlimit(RLIMIT_NOFILE, &lim) == 0
        && lim.rlim_cur != RLIM_INFINITY) {
        snprintf(reason, sizeof reason, "no fd at 10 or above is free (ulimit -n is %llu)",
                 (unsigned long long)lim.rlim_cur);
        return reason;
    }
    return strerror(err);
}
