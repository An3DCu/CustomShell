#ifndef ACSH_PATTERN_H
#define ACSH_PATTERN_H

#include <stdbool.h>
#include <stddef.h>

#include "strvec.h"

/*
 * Pathname expansion with my own matcher (not glob(3) or fnmatch(3)):
 *
 *   *        any run of characters, also none
 *   ?        one character
 *   [abc]    one of these; [a-z] a range; [!a] anything but; [[:alpha:]]
 *            a class. ] right after [ or [! is a member, - at either end
 *            is a member, and a [ with no closing ] is just a [.
 *
 * Patterns never match a /, and a name starting with . only matches when
 * the pattern starts with a . too, so * skips hidden files. This is the
 * file name module (not glob.h, which would hide the system's <glob.h>).
 *
 * A pattern is bytes plus, for each byte, whether it may act as one of the
 * characters above. Expansion clears that for quoted bytes, so in "*".c
 * the * is just a *.
 */
struct pattern {
    const char *text;
    const unsigned char *active; /* active[i] != 0: text[i] may be special. NULL: all may. */
    size_t len;
};

/* True if any part of the pattern between slashes has a *, a ? or a
 * complete [...] that may be special. Only those are worth a folder read. */
bool pattern_has_magic(const struct pattern *p);

/* True if name matches all of p. Slashes and leading dots get no special
 * treatment here; that's pattern_expand's job. */
bool pattern_match(const struct pattern *p, const char *name);

/* Adds the paths that match p to out, sorted by byte value like dash, and
 * returns how many there were. Folders that can't be read count as having
 * no matches. */
size_t pattern_expand(const struct pattern *p, struct strvec *out);

#endif
