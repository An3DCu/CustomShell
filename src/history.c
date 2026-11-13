#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xalloc.h"

#define DEFAULT_SIZE 1000

size_t history_size(const char *text)
{
    char *end;
    long n;

    if (text == NULL || text[0] < '0' || text[0] > '9')
        return DEFAULT_SIZE;
    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || n < 0)
        return DEFAULT_SIZE;
    return (size_t)n;
}

/* Drops the oldest entries until at most h->max are left. */
static void trim(struct history *h)
{
    size_t extra;

    if (h->len <= h->max)
        return;
    extra = h->len - h->max;
    for (size_t i = 0; i < extra; i++)
        free(h->lines[i]);
    memmove(h->lines, h->lines + extra, h->max * sizeof *h->lines);
    h->len = h->max;
}

static void push(struct history *h, const char *line)
{
    if (h->len == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->lines = xreallocarray(h->lines, h->cap, sizeof *h->lines);
    }
    h->lines[h->len++] = xstrdup(line);
}

/* Writes the whole list to the file again, readable only by its owner. */
static void rewrite(const struct history *h)
{
    int fd = open(h->file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f;

    if (fd < 0)
        return;
    f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        return;
    }
    for (size_t i = 0; i < h->len; i++)
        fprintf(f, "%s\n", h->lines[i]);
    fclose(f);
}

void history_init(struct history *h, const char *file, size_t max)
{
    FILE *f = NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    size_t entries = 0;
    int fd;

    *h = (struct history){ .max = max, .file = file && file[0] ? xstrdup(file) : NULL };
    if (h->file == NULL || (fd = open(h->file, O_RDONLY | O_CLOEXEC)) < 0)
        return;
    if ((f = fdopen(fd, "r")) == NULL) {
        close(fd);
        return;
    }
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        push(h, line);
        entries++;
        trim(h);
    }
    free(line);
    fclose(f);
    if (entries > h->len)
        rewrite(h); /* the file had more than $HISTSIZE entries */
}

void history_settings(struct history *h, const char *file, size_t max)
{
    if (file == NULL || file[0] == '\0') {
        free(h->file);
        h->file = NULL;
    } else if (h->file == NULL || strcmp(h->file, file) != 0) {
        free(h->file);
        h->file = xstrdup(file);
    }
    h->max = max;
    trim(h);
}

char *history_file(const char *histfile, const char *home)
{
    size_t size;
    char *path;

    if (histfile != NULL)
        return histfile[0] ? xstrdup(histfile) : NULL;
    if (home == NULL || home[0] == '\0')
        return NULL;
    size = strlen(home) + sizeof "/.acsh_history";
    path = xmalloc(size);
    snprintf(path, size, "%s/.acsh_history", home);
    return path;
}

static bool only_blanks(const char *s)
{
    return s[strspn(s, " \t")] == '\0';
}

void history_add(struct history *h, const char *line)
{
    int fd;

    if (h->max == 0 || only_blanks(line))
        return;
    if (h->len > 0 && strcmp(h->lines[h->len - 1], line) == 0)
        return;
    push(h, line);
    trim(h);
    if (h->file == NULL)
        return;
    fd = open(h->file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return; /* no history file is no reason to stop the shell */
    /* One write for the line and its newline, so two windows adding at the
     * same time can't mix their lines. */
    {
        size_t len = strlen(line);
        char *buf = xmalloc(len + 1);

        /* buf is for write(). It ends in the newline and has no 0. */
        /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
        memcpy(buf, line, len);
        buf[len] = '\n';
        (void)!write(fd, buf, len + 1);
        free(buf);
    }
    close(fd);
}

void history_clear(struct history *h)
{
    for (size_t i = 0; i < h->len; i++)
        free(h->lines[i]);
    h->len = 0;
    if (h->file != NULL)
        rewrite(h);
}

void history_free(struct history *h)
{
    for (size_t i = 0; i < h->len; i++)
        free(h->lines[i]);
    free(h->lines);
    free(h->file);
    *h = (struct history){ 0 };
}
