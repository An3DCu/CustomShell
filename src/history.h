#ifndef ACSH_HISTORY_H
#define ACSH_HISTORY_H

#include <stddef.h>

/*
 * The lines typed at the prompt, oldest first, for Up and Down, Ctrl+R and
 * the history built-in.
 *
 * Every line is its own entry, the "> " lines of a longer command too. No
 * entry has a newline in it. The file has one entry per line. A line goes
 * into the file as soon as it's entered (O_APPEND). A killed acsh loses
 * nothing. Another window starting later sees the line.
 *
 * The file is $HISTFILE, or ~/.acsh_history. With no HOME, or HISTFILE set
 * to nothing, there's no file. The history then only lives in memory.
 * $HISTSIZE (1000 if unset or not a number) is how many entries are kept.
 * 0 keeps none.
 */
struct history {
    char **lines;
    size_t len;
    size_t cap;
    size_t max;   /* $HISTSIZE */
    char *file;   /* NULL for none */
};

/* Reads the file, keeps the last max entries, and cuts the file back to
 * those. file may be NULL. */
void history_init(struct history *h, const char *file, size_t max);

/* Adds line (without its newline) to the list and to the file, unless it's
 * empty, only blanks, or the same as the entry before it. */
void history_add(struct history *h, const char *line);

/* Applies $HISTFILE and $HISTSIZE again, in case they were set since. A
 * new file is used from the next line on. A smaller size drops the oldest
 * entries. */
void history_settings(struct history *h, const char *file, size_t max);

/* The history file for $HISTFILE and $HISTSIZE's HOME: HISTFILE if it's set
 * (NULL if it's set to nothing), else HOME/.acsh_history, else NULL. A new
 * string. */
char *history_file(const char *histfile, const char *home);

/* history -c: empties the list and the file. */
void history_clear(struct history *h);

void history_free(struct history *h);

/* $HISTSIZE as a number of entries: 1000 for NULL or anything that isn't a
 * whole number from 0 up. */
size_t history_size(const char *text);

#endif
