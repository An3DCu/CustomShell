#include "complete.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "builtins.h"
#include "xalloc.h"

static bool ends_word(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || strchr("|;&<>()", c) != NULL;
}

size_t complete_word_start(const char *text, size_t pos)
{
    size_t start = 0;
    char quote = 0;

    for (size_t i = 0; i < pos; i++) {
        char c = text[i];

        if (quote != 0) {
            if (c == quote)
                quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '\\' && i + 1 < pos) {
            i++; /* an escaped character is part of the word */
        } else if (ends_word(c)) {
            start = i + 1;
        }
    }
    return start;
}

bool complete_is_command(const char *text, size_t start)
{
    size_t i = start;

    while (i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t'))
        i--;
    return i == 0 || strchr("|;&(", text[i - 1]) != NULL;
}

/* The word with its quotes and backslashes taken out: what to look for. */
static char *unquote(const char *word, size_t len)
{
    char *out = xmalloc(len + 1);
    size_t n = 0;
    char quote = 0;

    for (size_t i = 0; i < len; i++) {
        char c = word[i];

        if (quote != 0 && c == quote)
            quote = 0;
        else if (quote == 0 && (c == '\'' || c == '"'))
            quote = c;
        else if (quote == 0 && c == '\\' && i + 1 < len)
            out[n++] = word[++i];
        else
            out[n++] = c;
    }
    out[n] = '\0';
    return out;
}

static bool safe(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || strchr("._-/+:@%,=", c) != NULL;
}

char *complete_quote(const char *name)
{
    size_t len = strlen(name);
    bool plain = len > 0;
    char *out;
    size_t n = 0;

    for (size_t i = 0; i < len && plain; i++)
        plain = safe(name[i]);
    if (plain)
        return xstrdup(name);
    out = xmalloc(len * 4 + 3); /* each ' becomes '\'' */
    out[n++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = name[i];
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
    return out;
}

static char *join(const char *a, const char *b, const char *c)
{
    size_t size = strlen(a) + strlen(b) + strlen(c) + 1;
    char *out = xmalloc(size);

    snprintf(out, size, "%s%s%s", a, b, c);
    return out;
}

static void add(struct matches *m, char *show, char *insert, bool dir)
{
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->items = xreallocarray(m->items, m->cap, sizeof *m->items);
    }
    m->items[m->len++] = (struct match){ .show = show, .insert = insert, .dir = dir };
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* A name in a folder is offered when it starts with what's typed, and a
 * dot file only when what's typed starts with a dot too. */
static bool offered(const char *name, const char *typed)
{
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    if (name[0] == '.' && typed[0] != '.')
        return false;
    return starts_with(name, typed);
}

static void find_commands(const char *typed, const char *path, struct matches *m)
{
    const char *name;
    char *dirs;
    char *dir;
    char *rest;

    for (size_t i = 0; (name = builtin_name(i)) != NULL; i++) {
        if (starts_with(name, typed))
            add(m, xstrdup(name), complete_quote(name), false);
    }
    if (path == NULL)
        return;
    dirs = xstrdup(path);
    for (dir = dirs; dir != NULL; dir = rest) {
        DIR *d;
        struct dirent *ent;

        rest = strchr(dir, ':');
        if (rest != NULL)
            *rest++ = '\0';
        d = opendir(dir[0] ? dir : "."); /* an empty part of PATH is this folder */
        if (d == NULL)
            continue;
        while ((ent = readdir(d)) != NULL) {
            char *full;
            struct stat st;

            if (!offered(ent->d_name, typed))
                continue;
            full = join(dir[0] ? dir : ".", "/", ent->d_name);
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
                add(m, xstrdup(ent->d_name), complete_quote(ent->d_name), false);
            free(full);
        }
        closedir(d);
    }
    free(dirs);
}

static void find_files(const char *typed, const char *home, struct matches *m)
{
    const char *slash = strrchr(typed, '/');
    size_t dir_len = slash ? (size_t)(slash - typed) + 1 : 0;
    char *dir_typed = xstrndup(typed, dir_len); /* "src/", "~/", or "" */
    const char *base = typed + dir_len;
    bool tilde = starts_with(dir_typed, "~/") && home != NULL && home[0] != '\0';
    char *look_in;
    DIR *d;
    struct dirent *ent;

    if (tilde)
        look_in = join(home, dir_typed + 1, "");
    else
        look_in = xstrdup(dir_len ? dir_typed : "./");
    d = opendir(look_in);
    if (d != NULL) {
        while ((ent = readdir(d)) != NULL) {
            char *full;
            char *path;
            char *quoted;
            struct stat st;
            bool dir;

            if (!offered(ent->d_name, base))
                continue;
            full = join(look_in, ent->d_name, "");
            dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            free(full);
            /* ~/ stays outside the quotes, or it wouldn't be expanded */
            path = join(tilde ? dir_typed + 2 : dir_typed, ent->d_name, "");
            quoted = complete_quote(path);
            add(m, join(ent->d_name, dir ? "/" : "", ""),
                join(tilde ? "~/" : "", quoted, dir ? "/" : ""), dir);
            free(quoted);
            free(path);
        }
        closedir(d);
    }
    free(look_in);
    free(dir_typed);
}

static int by_show(const void *a, const void *b)
{
    return strcmp(((const struct match *)a)->show, ((const struct match *)b)->show);
}

void complete_find(const char *word, size_t len, bool command, const char *path,
                   const char *home, struct matches *m)
{
    char *typed = unquote(word, len);
    size_t kept = 0;

    if (command && strchr(typed, '/') == NULL)
        find_commands(typed, path, m);
    else
        find_files(typed, home, m);
    free(typed);
    if (m->len == 0)
        return;
    /* sorted, and a name found twice (a built-in and a program in PATH,
     * or two PATH folders) only once */
    qsort(m->items, m->len, sizeof *m->items, by_show);
    for (size_t i = 0; i < m->len; i++) {
        if (kept > 0 && strcmp(m->items[kept - 1].insert, m->items[i].insert) == 0) {
            free(m->items[i].show);
            free(m->items[i].insert);
            continue;
        }
        m->items[kept++] = m->items[i];
    }
    m->len = kept;
}

void complete_free(struct matches *m)
{
    for (size_t i = 0; i < m->len; i++) {
        free(m->items[i].show);
        free(m->items[i].insert);
    }
    free(m->items);
    *m = (struct matches){ 0 };
}
