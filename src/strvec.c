#include "strvec.h"

#include <stdlib.h>

#include "xalloc.h"

static void reserve(struct strvec *v, size_t extra)
{
    if (v->len + extra < v->cap)
        return; /* < and not <=: there must be room for the NULL as well */
    v->cap = v->cap ? v->cap * 2 : 8;
    while (v->len + extra >= v->cap)
        v->cap *= 2;
    v->items = xreallocarray(v->items, v->cap, sizeof *v->items);
}

void strvec_push(struct strvec *v, char *s)
{
    reserve(v, 1);
    v->items[v->len++] = s;
    v->items[v->len] = NULL;
}

char **strvec_items(struct strvec *v)
{
    if (v->items == NULL) {
        reserve(v, 0);
        v->items[0] = NULL;
    }
    return v->items;
}

void strvec_free(struct strvec *v)
{
    for (size_t i = 0; i < v->len; i++)
        free(v->items[i]);
    free(v->items);
    *v = (struct strvec){ 0 };
}
