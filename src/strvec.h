#ifndef ACSH_STRVEC_H
#define ACSH_STRVEC_H

#include <stddef.h>

/*
 * A growing list of strings that is always NULL-terminated. items can go
 * straight to execvp as argv. The list owns its strings.
 */
struct strvec {
    char **items;
    size_t len;
    size_t cap;
};

/* Adds s to the end. The list takes ownership of it. */
void strvec_push(struct strvec *v, char *s);

/* items, never NULL. An empty list is { NULL }. */
char **strvec_items(struct strvec *v);

/* Frees the first n strings (n <= len) and moves the rest down. */
void strvec_remove_front(struct strvec *v, size_t n);

/* Frees every string and the list, and leaves it empty. */
void strvec_free(struct strvec *v);

#endif
