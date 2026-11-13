/*
 * mkdir: make folders.
 *
 *     mkdir [-p] dir ...
 *
 * New folders get mode 0777 before the umask (so usually rwxr-xr-x).
 * -p also makes every missing parent. With -p, a folder that already
 * exists isn't an error.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tool.h"

static void usage(FILE *to)
{
    fputs("usage: mkdir [-p] dir ...\n", to);
}

static bool is_folder(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir, where "already a folder" counts as success. */
static bool make_one(const char *path, bool exists_ok)
{
    if (mkdir(path, 0777) == 0)
        return true;
    if (errno == EEXIST && exists_ok && is_folder(path))
        return true;
    tool_error(path, errno);
    return false;
}

/* -p: make every folder on the way, then the folder itself. For a/b/c that
 * means a, then a/b, then a/b/c. */
static bool make_with_parents(const char *path)
{
    size_t len = strlen(path);
    char *prefix = malloc(len + 1);
    bool ok = true;

    if (prefix == NULL) {
        tool_error(path, ENOMEM);
        return false;
    }
    memcpy(prefix, path, len + 1);
    /* Start at 1. A leading / is the root. The root always exists. */
    for (size_t i = 1; i < len; i++) {
        if (prefix[i] != '/' || prefix[i - 1] == '/')
            continue; /* not a separator, or the second / of a//b */
        prefix[i] = '\0';
        /* The result doesn't matter here. If this parent already exists,
         * fine. If it can't be made (a file in the way, no permission), the
         * next step fails too. That step reports it with the full name. */
        (void)mkdir(prefix, 0777);
        prefix[i] = '/';
    }
    ok = make_one(path, true);
    free(prefix);
    return ok;
}

int main(int argc, char **argv)
{
    bool parents = false;
    int opt;
    int status = 0;

    tool_init("mkdir");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    opterr = 0;
    while ((opt = getopt(argc, argv, "p")) != -1) {
        if (opt == 'p') {
            parents = true;
        } else {
            tool_message("unknown option '-%c'", optopt);
            usage(stderr);
            return 2;
        }
    }
    if (optind == argc) {
        tool_message("missing folder name");
        usage(stderr);
        return 2;
    }
    for (int i = optind; i < argc; i++) {
        bool ok = parents ? make_with_parents(argv[i]) : make_one(argv[i], false);

        if (!ok)
            status = 1;
    }
    return status;
}
