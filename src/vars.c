#include "vars.h"

#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

static bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

size_t var_name_len(const char *s)
{
    size_t n = 0;

    if (!is_name_start(s[0]))
        return 0;
    while (is_name_char(s[n]))
        n++;
    return n;
}

bool var_name_valid(const char *s)
{
    size_t n = var_name_len(s);

    return n > 0 && s[n] == '\0';
}

/* A linear search. A shell has a few dozen variables. A list in the order
 * they were made is easy to check in a debugger. */
static struct var *find(const struct vars *v, const char *name)
{
    for (size_t i = 0; i < v->len; i++) {
        if (strcmp(v->items[i].name, name) == 0)
            return &v->items[i];
    }
    return NULL;
}

static struct var *add(struct vars *v, char *name, char *value, bool exported)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->items = xreallocarray(v->items, v->cap, sizeof *v->items);
    }
    v->items[v->len] = (struct var){ .name = name, .value = value, .exported = exported };
    return &v->items[v->len++];
}

void vars_init(struct vars *v, char **env)
{
    *v = (struct vars){ 0 };
    for (size_t i = 0; env != NULL && env[i] != NULL; i++) {
        const char *eq = strchr(env[i], '=');
        char *name;

        if (eq == NULL || eq == env[i])
            continue; /* not NAME=value, nothing to keep */
        name = xstrndup(env[i], (size_t)(eq - env[i]));
        if (find(v, name) != NULL) {
            free(name); /* a second copy in environ. The first one wins, as in getenv. */
            continue;
        }
        add(v, name, xstrdup(eq + 1), true);
    }
}

void vars_free(struct vars *v)
{
    for (size_t i = 0; i < v->len; i++) {
        free(v->items[i].name);
        free(v->items[i].value);
    }
    free(v->items);
    *v = (struct vars){ 0 };
}

const char *vars_get(const struct vars *v, const char *name)
{
    const struct var *var = find(v, name);

    return var ? var->value : NULL;
}

void vars_set(struct vars *v, const char *name, const char *value)
{
    struct var *var = find(v, name);
    char *copy;

    if (var == NULL) {
        add(v, xstrdup(name), xstrdup(value), false);
        return;
    }
    copy = xstrdup(value); /* before the free. value may point into the old one. */
    free(var->value);
    var->value = copy;
}

void vars_export(struct vars *v, const char *name)
{
    struct var *var = find(v, name);

    if (var == NULL)
        var = add(v, xstrdup(name), NULL, false);
    var->exported = true;
}

void vars_unset(struct vars *v, const char *name)
{
    struct var *var = find(v, name);
    size_t i;

    if (var == NULL)
        return;
    free(var->name);
    free(var->value);
    /* Keep the order. The rest moves down. The last one doesn't fill the gap. */
    i = (size_t)(var - v->items);
    memmove(&v->items[i], &v->items[i + 1], (v->len - i - 1) * sizeof *v->items);
    v->len--;
}

bool vars_exists(const struct vars *v, const char *name)
{
    return find(v, name) != NULL;
}

bool vars_is_exported(const struct vars *v, const char *name)
{
    const struct var *var = find(v, name);

    return var != NULL && var->exported;
}

char **vars_environ(const struct vars *v)
{
    char **env = xcalloc(v->len + 1, sizeof *env);
    size_t n = 0;

    for (size_t i = 0; i < v->len; i++) {
        const struct var *var = &v->items[i];
        size_t nlen, vlen;

        if (!var->exported || var->value == NULL)
            continue;
        nlen = strlen(var->name);
        vlen = strlen(var->value);
        env[n] = xmalloc(nlen + vlen + 2);
        memcpy(env[n], var->name, nlen);
        env[n][nlen] = '=';
        memcpy(env[n] + nlen + 1, var->value, vlen + 1);
        n++;
    }
    env[n] = NULL;
    return env;
}

void vars_free_environ(char **env)
{
    for (size_t i = 0; env[i] != NULL; i++)
        free(env[i]);
    free(env);
}

static int by_name(const void *a, const void *b)
{
    const struct var *const *x = a;
    const struct var *const *y = b;

    return strcmp((*x)->name, (*y)->name);
}

const struct var **vars_sorted(const struct vars *v, size_t *count)
{
    const struct var **list = xcalloc(v->len + 1, sizeof *list);

    for (size_t i = 0; i < v->len; i++)
        list[i] = &v->items[i];
    if (v->len > 1)
        qsort(list, v->len, sizeof *list, by_name);
    *count = v->len;
    return list;
}
