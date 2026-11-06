#ifndef ACSH_STRVEC_H
#define ACSH_STRVEC_H

#include <stddef.h>

/*
 * A growing list of strings that is always NULL-terminated, so items can
 * go straight to execvp as argv. The list owns its strings.
 */
struct strvec {
    char **items;
    size_t len;
    size_t cap;
};

/* Adds s to the end; the list takes ownership of it. */
void strvec_push(struct strvec *v, char *s);

/* items, never NULL: an empty list is just { NULL }. */
char **strvec_items(struct strvec *v);

/* Frees every string and the list, and leaves it empty. */
void strvec_free(struct strvec *v);

#endif
