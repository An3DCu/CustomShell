#include "xalloc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void out_of_memory(void)
{
    fputs("acsh: out of memory\n", stderr);
    exit(1);
}

void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);

    if (p == NULL)
        out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);

    if (p == NULL)
        out_of_memory();
    return p;
}

void *xreallocarray(void *p, size_t count, size_t size)
{
    size_t bytes;
    void *grown;

    if (size != 0 && count > SIZE_MAX / size)
        out_of_memory();
    bytes = count * size;
    grown = realloc(p, bytes ? bytes : 1);
    if (grown == NULL)
        out_of_memory();
    return grown;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *copy = xmalloc(n + 1);

    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

char *xgetcwd(void)
{
    /* getcwd needs a buffer big enough. PATH_MAX doesn't limit paths on
     * Linux. Start small and double until it fits. */
    for (size_t size = 256;; size *= 2) {
        char *buf = xmalloc(size);
        int err;

        if (getcwd(buf, size) != NULL)
            return buf;
        err = errno;
        free(buf);
        if (err != ERANGE) {
            errno = err;
            return NULL;
        }
    }
}

void *xgrow(void *p, size_t len, size_t size)
{
    /* The room is always the next power of two from len up. It's full
     * exactly when len is 0 or a power of two. */
    if (len != 0 && (len & (len - 1)) != 0)
        return p;
    return xreallocarray(p, len ? len * 2 : 1, size);
}
