#ifndef ACSH_XALLOC_H
#define ACSH_XALLOC_H

#include <stddef.h>

/*
 * Memory allocation that never returns NULL. If the system is out of memory,
 * acsh prints "acsh: out of memory" and exits with status 1, the way bash and
 * dash do. Callers don't need to check, so the rest of the code stays readable.
 */
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);

/* Resizes p to hold count items of size bytes each, refusing a count * size
 * that would overflow instead of quietly allocating too little. */
void *xreallocarray(void *p, size_t count, size_t size);

char *xstrdup(const char *s);

/* Copies the first n bytes of s and adds a 0. s must have at least n bytes. */
char *xstrndup(const char *s, size_t n);

/* The current folder as a new string, however long it is. Returns NULL with
 * errno set if it can't be read (for example, the folder was deleted). */
char *xgetcwd(void);

#endif
