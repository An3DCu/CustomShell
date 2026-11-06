#include "pattern.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "xalloc.h"

static bool is_active(const struct pattern *p, size_t i)
{
    return p->active == NULL || p->active[i] != 0;
}

static bool is_special(const struct pattern *p, size_t i, char c)
{
    return p->text[i] == c && is_active(p, i);
}

/* ---- matching one name -------------------------------------------------- */

static bool class_matches(const char *name, size_t n, unsigned char c)
{
    static const struct {
        const char *name;
        int (*test)(int);
    } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank },
        { "cntrl", iscntrl }, { "digit", isdigit }, { "graph", isgraph },
        { "lower", islower }, { "print", isprint }, { "punct", ispunct },
        { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
    };

    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        if (strlen(classes[i].name) == n && memcmp(classes[i].name, name, n) == 0)
            return classes[i].test(c) != 0;
    }
    return false; /* [[:nope:]] matches nothing, as in dash */
}

/*
 * The bracket expression starting at p->text[start], which is a [. Returns
 * -1 if it never closes (then the [ is an ordinary character). Otherwise
 * sets *end to just past the ] and returns 1 if c is in the set, 0 if not.
 */
static int match_bracket(const struct pattern *p, size_t start, unsigned char c, size_t *end)
{
    size_t k = start + 1;
    size_t first;
    bool negate = false;
    bool found = false;

    if (k < p->len && is_special(p, k, '!')) {
        negate = true;
        k++;
    }
    first = k;
    for (;;) {
        unsigned char lo;

        if (k >= p->len)
            return -1;
        if (k > first && is_special(p, k, ']'))
            break;
        /* [:name:] */
        if (is_special(p, k, '[') && k + 1 < p->len && is_special(p, k + 1, ':')) {
            size_t m = k + 2;

            while (m + 1 < p->len && !(is_special(p, m, ':') && is_special(p, m + 1, ']')))
                m++;
            if (m + 1 < p->len) {
                if (class_matches(p->text + k + 2, m - k - 2, c))
                    found = true;
                k = m + 2;
                continue;
            }
        }
        lo = (unsigned char)p->text[k];
        /* a-z, but a - before the closing ] is just a - */
        if (k + 2 < p->len && is_special(p, k + 1, '-') && !is_special(p, k + 2, ']')) {
            unsigned char hi = (unsigned char)p->text[k + 2];

            if (lo <= c && c <= hi)
                found = true;
            k += 3;
            continue;
        }
        if (lo == c)
            found = true;
        k++;
    }
    *end = k + 1;
    return found != negate;
}

/*
 * The usual wildcard match with one saved * to go back to. When something
 * fails after a *, the * takes one more character and the rest is tried
 * again. Only the last * ever needs to be retried, so this is linear in
 * practice and never recursive.
 */
bool pattern_match(const struct pattern *p, const char *name)
{
    size_t pi = 0;
    const char *s = name;
    size_t star = 0;
    const char *star_s = NULL;

    while (*s != '\0') {
        if (pi < p->len) {
            char c = p->text[pi];

            if (is_special(p, pi, '*')) {
                star = pi++;
                star_s = s;
                continue;
            }
            if (is_special(p, pi, '?')) {
                pi++;
                s++;
                continue;
            }
            if (is_special(p, pi, '[')) {
                size_t end;
                int r = match_bracket(p, pi, (unsigned char)*s, &end);

                if (r == 1) {
                    pi = end;
                    s++;
                    continue;
                }
                if (r == 0)
                    goto retry;
                /* r == -1: a [ that never closes is just a [ */
            }
            if (c == *s) {
                pi++;
                s++;
                continue;
            }
        }
retry:
        if (star_s == NULL)
            return false;
        pi = star + 1;
        s = ++star_s;
    }
    while (pi < p->len && is_special(p, pi, '*'))
        pi++;
    return pi == p->len;
}

/* ---- walking the folders ------------------------------------------------ */

static size_t component_end(const struct pattern *p, size_t start)
{
    size_t end = start;

    while (end < p->len && p->text[end] != '/')
        end++;
    return end;
}

static struct pattern component(const struct pattern *p, size_t start, size_t end)
{
    return (struct pattern){
        .text = p->text + start,
        .active = p->active ? p->active + start : NULL,
        .len = end - start,
    };
}

static bool component_has_magic(const struct pattern *c)
{
    for (size_t i = 0; i < c->len; i++) {
        size_t end;

        if (is_special(c, i, '*') || is_special(c, i, '?'))
            return true;
        if (is_special(c, i, '[') && match_bracket(c, i, 0, &end) >= 0)
            return true;
    }
    return false;
}

bool pattern_has_magic(const struct pattern *p)
{
    for (size_t start = 0; start <= p->len;) {
        size_t end = component_end(p, start);
        struct pattern c = component(p, start, end);

        if (component_has_magic(&c))
            return true;
        start = end + 1;
    }
    return false;
}

/* prefix + the n bytes at name + tail, as a new string. */
static char *join(const char *prefix, const char *name, size_t n, const char *tail)
{
    size_t plen = strlen(prefix), tlen = strlen(tail);
    char *s = xmalloc(plen + n + tlen + 1);

    memcpy(s, prefix, plen);
    memcpy(s + plen, name, n);
    memcpy(s + plen + n, tail, tlen + 1);
    return s;
}

/*
 * Matches the part of p from start on, inside the folder prefix ("" for
 * the current folder, otherwise ending in /). One part between slashes at
 * a time: a part without magic is taken as it is, a part with magic is
 * matched against every name in the folder. The last part decides what's
 * added: a match from readdir exists already, a part taken as it is is
 * checked with lstat, so a pattern ending in /f1 only finds the folders
 * that really have an f1.
 */
static void expand_from(const struct pattern *p, size_t start, const char *prefix,
                        struct strvec *out)
{
    size_t end = component_end(p, start);
    bool last = end == p->len;
    struct pattern c = component(p, start, end);
    const struct dirent *e;
    DIR *dir;

    if (!component_has_magic(&c)) {
        char *next = join(prefix, c.text, c.len, last ? "" : "/");
        struct stat st;

        if (!last)
            expand_from(p, end + 1, next, out);
        else if (lstat(next, &st) == 0) {
            strvec_push(out, next);
            return;
        }
        free(next);
        return;
    }
    dir = opendir(prefix[0] != '\0' ? prefix : ".");
    if (dir == NULL)
        return;
    while ((e = readdir(dir)) != NULL) {
        char *next;

        if (e->d_name[0] == '.' && c.text[0] != '.')
            continue; /* hidden: only a pattern starting with . finds it */
        if (!pattern_match(&c, e->d_name))
            continue;
        next = join(prefix, e->d_name, strlen(e->d_name), last ? "" : "/");
        if (last) {
            strvec_push(out, next);
        } else {
            expand_from(p, end + 1, next, out);
            free(next);
        }
    }
    closedir(dir);
}

static int by_bytes(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t pattern_expand(const struct pattern *p, struct strvec *out)
{
    struct strvec found = { 0 };
    size_t n;

    expand_from(p, 0, "", &found);
    n = found.len;
    if (n > 1)
        qsort(found.items, n, sizeof *found.items, by_bytes);
    for (size_t i = 0; i < n; i++)
        strvec_push(out, found.items[i]); /* out takes over the strings */
    free(found.items);
    return n;
}
