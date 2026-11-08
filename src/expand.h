#ifndef ACSH_EXPAND_H
#define ACSH_EXPAND_H

#include <stdbool.h>
#include <stddef.h>

#include "shell.h"
#include "strvec.h"

/*
 * Expansion turns a word as typed into what the command gets, in the order
 * POSIX gives:
 *
 *   1. tilde        ~ and ~/x become $HOME, ~name becomes that user's home
 *   2. parameters   $X, ${X}, $?, $$, $!, $#, $0, $1..., $@, $*
 *   3. splitting    the result of an unquoted $X is cut into words on IFS
 *   4. file names   a word with an unquoted *, ? or [...] becomes the
 *                   names that match it, sorted (see pattern.h)
 *   5. quotes       removed; they only decided what 1 to 4 may touch
 *
 * Everything works on the word as the lexer left it, quotes included, so
 * the text of a variable is never read as shell syntax: after X='a; b',
 * `echo $X` prints "a;" and "b", and runs nothing.
 *
 * $(...) and backquotes aren't supported yet and are reported as errors
 * instead of being printed as they are.
 */

/* Expands a command's words and adds the resulting fields to out. A word
 * can become several fields ($X with X="a b") or none ($X with X empty).
 * When the command is `export`, arguments like NAME=value are expanded as
 * assignments instead, so `export X=$Y` keeps Y's spaces. Returns false and
 * writes the reason into err ("${A: missing '}'") on an error. */
bool expand_command(struct shell *sh, char *const *words, size_t nwords,
                    struct strvec *out, char *err, size_t errlen);

/* The same for words that aren't a command, like the ones after `for x
 * in`: no special case for export. */
bool expand_words(struct shell *sh, char *const *words, size_t nwords,
                  struct strvec *out, char *err, size_t errlen);

/* Expands one word into one string, with no splitting and no file names:
 * for redirection targets, so `> $F` with F="a b" writes to the file "a b",
 * and `> *.log` to a file called *.log, as in dash.
 * Returns a new string, or NULL with err set. */
char *expand_one(struct shell *sh, const char *word, char *err, size_t errlen);

/* Expands the value part of an assignment (after the =). Like expand_one,
 * and a ~ right after a : expands too, so PATH=~/bin:~/tools works. */
char *expand_assignment(struct shell *sh, const char *value, char *err, size_t errlen);

#endif
