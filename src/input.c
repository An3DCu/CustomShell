#include "input.h"

#include <errno.h>
#include <unistd.h>

#include "error.h"
#include "signals.h"
#include "xalloc.h"

static bool zero_seen;

void input_warn_zero(const struct shell *sh)
{
    static bool warned;

    if (zero_seen && !warned) {
        shell_error(sh, "ignored a zero byte in the input");
        warned = true;
    }
    zero_seen = false;
}

ssize_t input_read_line(int fd, char **line, size_t *cap, bool interruptible)
{
    size_t len = 0;

    for (;;) {
        char c;
        ssize_t n;

        if (interruptible && !signals_wait_for_input(fd))
            return -1; /* errno is EINTR for Ctrl+C */
        n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue; /* a signal the shell doesn't act on */
            return -1;
        }
        if (n == 0)
            break;
        if (c == '\0') {
            /* C strings end at a zero byte. It would cut the line short.
             * It's dropped, as in bash and dash. input_warn_zero says so. */
            zero_seen = true;
            continue;
        }
        if (len + 2 > *cap) { /* room for c and the final 0 */
            *cap = *cap ? *cap * 2 : 128;
            *line = xreallocarray(*line, *cap, 1);
        }
        (*line)[len++] = c;
        if (c == '\n')
            break;
    }
    if (len == 0) {
        errno = 0;
        return -1;
    }
    (*line)[len] = '\0';
    return (ssize_t)len;
}
