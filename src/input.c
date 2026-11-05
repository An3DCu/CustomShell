#include "input.h"

#include <errno.h>
#include <unistd.h>

#include "signals.h"
#include "xalloc.h"

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
