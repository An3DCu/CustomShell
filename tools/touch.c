/*
 * touch: set a file's access and change times to now, creating it if needed.
 *
 *     touch [-c] file ...
 *
 * -c only touches files that already exist and never creates one. A new
 * file is empty, with mode 0666 before the umask (so usually rw-r--r--).
 * A folder can be touched too.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tool.h"

static void usage(FILE *to)
{
    fputs("usage: touch [-c] file ...\n", to);
}

/* Returns false after an error (reported). */
static bool touch(const char *path, bool no_create)
{
    int fd;

    /* NULL times mean "now", for both access and modification. */
    if (utimensat(AT_FDCWD, path, NULL, 0) == 0)
        return true;
    if (errno != ENOENT) {
        tool_error(path, errno);
        return false;
    }
    if (no_create)
        return true; /* -c: a missing file isn't an error */
    /* A new file gets the current time by itself. */
    fd = open(path, O_WRONLY | O_CREAT | O_NOCTTY, 0666);
    if (fd < 0) {
        tool_error(path, errno);
        return false;
    }
    close(fd);
    return true;
}

int main(int argc, char **argv)
{
    bool no_create = false;
    int opt;
    int status = 0;

    tool_init("touch");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    opterr = 0;
    while ((opt = getopt(argc, argv, "c")) != -1) {
        if (opt == 'c') {
            no_create = true;
        } else {
            tool_message("unknown option '-%c'", optopt);
            usage(stderr);
            return 2;
        }
    }
    if (optind == argc) {
        tool_message("missing file name");
        usage(stderr);
        return 2;
    }
    for (int i = optind; i < argc; i++) {
        if (!touch(argv[i], no_create))
            status = 1;
    }
    return status;
}
