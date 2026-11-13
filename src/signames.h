#ifndef ACSH_SIGNAMES_H
#define ACSH_SIGNAMES_H

/*
 * Signal names for kill: the standard signals, HUP to SYS, by their name
 * without SIG. Any other signal can still be given by number.
 */

/* The signal that text names: TERM, SIGTERM, term or 15. -1 if it isn't
 * a name or a whole number. A number isn't checked any further. */
int signame_parse(const char *text);

/* The name of signal sig without SIG, like "TERM", or NULL if it has none
 * here. */
const char *signame_of(int sig);

/* The highest signal number that has a name, to list them in order. */
int signame_max(void);

#endif
