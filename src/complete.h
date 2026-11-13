#ifndef ACSH_COMPLETE_H
#define ACSH_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Tab completion: what the word before the cursor can become.
 *
 * A word where a command starts (the first one, or the first after |, ;,
 * &&, || or &) completes from the built-ins and the programs in $PATH. Any
 * other word, or one with a / in it like ./run.sh, completes from file
 * names. A name that starts with a dot only comes up when the word does.
 */

struct match {
    char *show;   /* as listed: the name, a folder with / after it */
    char *insert; /* as put in the line: the whole word, quoted if needed */
    bool dir;
};

struct matches {
    struct match *items;
    size_t len;
    size_t cap;
};

/* Where the word that ends at pos starts. Blanks and | ; & < > ( ) end a
 * word, unless they're inside quotes. */
size_t complete_word_start(const char *text, size_t pos);

/* Whether the word that starts at start is where a command goes. */
bool complete_is_command(const char *text, size_t start);

/*
 * Fills m with what the word (len bytes, as typed, quotes and all) can
 * become, sorted, without repeats. command: the word is where a command
 * goes. path and home are $PATH and $HOME, either may be NULL. A word that
 * starts with ~/ looks in home, and the ~/ stays in what's inserted.
 */
void complete_find(const char *word, size_t len, bool command, const char *path,
                   const char *home, struct matches *m);

void complete_free(struct matches *m);

/* name as it can go into a command line: as it is when it's only letters,
 * digits and . _ - / + : @ % , =, else in single quotes ('it'\''s' for a
 * quote inside). A new string. */
char *complete_quote(const char *name);

#endif
