#ifndef ACSH_PROMPT_H
#define ACSH_PROMPT_H

#include <stdbool.h>

#include "shell.h"

/*
 * Prints the prompt on two lines, to stderr like bash does, so redirecting
 * the shell's stdout never captures it:
 *
 *     user@host ~/src/acsh  exit 1
 *     acsh$
 *
 * The first line says where you are, and how the last command ended when
 * it failed. The command goes on the second, so a long folder never
 * squeezes it. # instead of $ for root. In color when prompt_colors says so.
 */
void prompt_print(const struct shell *sh);

/* The prompt's second line, where the command is typed: "acsh$ ", or
 * "acsh# " for root. The line editor prints it again when it redraws. */
const char *prompt_tail(void);

/* Colors only on a terminal that has them: not when NO_COLOR is set to
 * anything (no-color.org), and not when TERM is unset or "dumb". */
bool prompt_colors(const char *no_color, const char *term);

/* The folder as the prompt shows it: home becomes ~. Only a whole folder
 * name matches, so with home /home/user, /home/user2 stays /home/user2. Returns a
 * new string. home may be NULL or empty. */
char *prompt_folder(const char *cwd, const char *home);

#endif
