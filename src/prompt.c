#include "prompt.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vars.h"
#include "xalloc.h"

char *prompt_folder(const char *cwd, const char *home)
{
    size_t n;

    if (home == NULL || home[0] == '\0')
        return xstrdup(cwd);
    n = strlen(home);
    while (n > 1 && home[n - 1] == '/') /* HOME=/home/user/ means /home/user */
        n--;
    if (strncmp(cwd, home, n) != 0)
        return xstrdup(cwd);
    if (cwd[n] == '\0') {
        return xstrdup("~");
    }
    if (cwd[n] == '/' && n > 1) {
        char *out = xmalloc(strlen(cwd + n) + 2);

        out[0] = '~';
        memcpy(out + 1, cwd + n, strlen(cwd + n) + 1);
        return out;
    }
    return xstrdup(cwd);
}

bool prompt_colors(const char *no_color, const char *term)
{
    if (no_color != NULL && no_color[0] != '\0')
        return false;
    return term != NULL && term[0] != '\0' && strcmp(term, "dumb") != 0;
}

/* ANSI colors: bold green, bold blue, red, and back to normal. */
#define GREEN "\033[1;32m"
#define BLUE  "\033[1;34m"
#define RED   "\033[31m"
#define RESET "\033[0m"

void prompt_print(const struct shell *sh)
{
    const struct passwd *pw = getpwuid(geteuid());
    const char *user = pw ? pw->pw_name : vars_get(&sh->vars, "USER");
    bool color = prompt_colors(vars_get(&sh->vars, "NO_COLOR"), vars_get(&sh->vars, "TERM"));
    char host[256];
    char *dot;
    char status[32] = "";
    char *cwd = xgetcwd();
    char *folder = prompt_folder(cwd ? cwd : "?", vars_get(&sh->vars, "HOME"));

    if (gethostname(host, sizeof host) != 0)
        memcpy(host, "?", sizeof "?");
    host[sizeof host - 1] = '\0'; /* gethostname may not end it if cut */
    dot = strchr(host, '.');
    if (dot != NULL)
        *dot = '\0'; /* laptop.local -> laptop, like bash */
    if (sh->status != 0)
        snprintf(status, sizeof status, "  %sexit %d%s", color ? RED : "", sh->status,
                 color ? RESET : "");

    fprintf(stderr, "%s%s@%s%s %s%s%s%s\n%s",
            color ? GREEN : "", user ? user : "?", host, color ? RESET : "",
            color ? BLUE : "", folder, color ? RESET : "", status, prompt_tail());
    free(folder);
    free(cwd);
}

const char *prompt_tail(void)
{
    return geteuid() == 0 ? "acsh# " : "acsh$ ";
}
