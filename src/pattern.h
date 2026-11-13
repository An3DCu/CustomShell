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
 *   [abc]    one of these. [a-z] is a range, [!a] anything but a, and
 *            [[:alpha:]] a class. ] right after [ or [! is a member. So is
 *            - at either end. A [ with no closing ] is a plain [.
 *
 * Patterns never match a /. A name starting with . only matches when the
 * pattern starts with a . too. * skips hidden files that way. The module
 * is called pattern because a glob.h would hide the system's <glob.h>.
 *
 * A pattern is bytes plus, for each byte, whether it may act as one of the
 * characters above. Expansion clears that for quoted bytes. In "*".c the *
 * is a plain *.
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
 * treatment here. pattern_expand handles them. */
bool pattern_match(const struct pattern *p, const char *name);

/* Adds the paths that match p to out, sorted by byte value like dash.
 * Returns how many there were. Folders that can't be read count as having
 * no matches. */
size_t pattern_expand(const struct pattern *p, struct strvec *out);

#endif
