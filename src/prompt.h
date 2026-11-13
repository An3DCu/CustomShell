#ifndef ACSH_PROMPT_H
#define ACSH_PROMPT_H

#include <stdbool.h>

#include "shell.h"

/*
 * Prints the prompt on two lines, to stderr as in bash. Redirecting the
 * shell's stdout never captures it:
 *
 *     user@host ~/src/acsh  exit 1
 *     acsh$
 *
 * The first line says where you are. When the last command failed, it
 * also says how it ended. The command goes on the second line. A long
 * folder never squeezes it there. # instead of $ for root. In color when
 * prompt_colors says so.
 */
void prompt_print(const struct shell *sh);

/* The prompt's second line, where the command is typed: "acsh$ ", or
 * "acsh# " for root. The line editor prints it again when it redraws. */
const char *prompt_tail(void);

/* Colors only on a terminal that has them. None when NO_COLOR is set to
 * anything (no-color.org). None when TERM is unset or "dumb". */
bool prompt_colors(const char *no_color, const char *term);

/* The folder as the prompt shows it, with home as ~. Only a whole folder
 * name matches. With home /home/user, /home/user2 stays /home/user2.
 * Returns a new string. home may be NULL or empty. */
char *prompt_folder(const char *cwd, const char *home);

#endif
