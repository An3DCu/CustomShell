#include "prompt.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        strcpy(out + 1, cwd + n);
        return out;
    }
    return xstrdup(cwd);
}

void prompt_print(const char *home)
{
    const struct passwd *pw = getpwuid(geteuid());
    const char *user = pw ? pw->pw_name : getenv("USER");
    char host[256];
    char *cwd = xgetcwd();
    char *folder = prompt_folder(cwd ? cwd : "?", home);

    if (gethostname(host, sizeof host) != 0)
        strcpy(host, "?");
    host[sizeof host - 1] = '\0';  /* gethostname may not end it if cut */
    host[strcspn(host, ".")] = '\0'; /* laptop.local -> laptop, like bash */

    fprintf(stderr, "%s@%s:%s%s ", user ? user : "?", host, folder,
            geteuid() == 0 ? "#" : "$");
    free(folder);
    free(cwd);
}
