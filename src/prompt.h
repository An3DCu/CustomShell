#ifndef ACSH_PROMPT_H
#define ACSH_PROMPT_H

/* Prints the prompt, user@host:folder$ (# for root), to stderr like bash
 * does, so redirecting the shell's stdout never captures it. */
void prompt_print(void);

/* The folder as the prompt shows it: home becomes ~. Only a whole folder
 * name matches, so with home /home/user, /home/user2 stays /home/user2. Returns a
 * new string. home may be NULL or empty. */
char *prompt_folder(const char *cwd, const char *home);

#endif
