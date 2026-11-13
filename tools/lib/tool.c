#include "tool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *name = "tool";

void tool_init(const char *tool)
{
    name = tool;
}

const char *tool_name(void)
{
    return name;
}

void tool_error(const char *what, int err)
{
    fprintf(stderr, "%s: %s: %s\n", name, what, strerror(err));
}

void tool_message(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int tool_open_input(const char *path)
{
    if (strcmp(path, "-") == 0)
        return STDIN_FILENO;
    return open(path, O_RDONLY);
}

void tool_close_input(int fd)
{
    if (fd != STDIN_FILENO)
        close(fd);
}

ssize_t tool_read(int fd, void *buf, size_t len)
{
    ssize_t n;

    do {
        n = read(fd, buf, len);
    } while (n < 0 && errno == EINTR);
    return n;
}

bool tool_write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    /* write() may take only part of it (a pipe that's nearly full, a
     * signal). Keep going from where it stopped. */
    while (len > 0) {
        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool tool_parse_count(const char *s, unsigned long long *out)
{
    unsigned long long value = 0;

    if (*s == '\0')
        return false;
    for (; *s != '\0'; s++) {
        unsigned d;

        if (*s < '0' || *s > '9')
            return false;
        d = (unsigned)(*s - '0');
        if (value > (~0ULL - d) / 10)
            return false; /* too big */
        value = value * 10 + d;
    }
    *out = value;
    return true;
}
