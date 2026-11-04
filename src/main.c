/*
 * acsh: a small Unix shell.
 *
 * For now this only answers --version and --help, so the build and the tests
 * have something real to run. The read, parse and run loop comes next.
 */
#include <stdio.h>
#include <string.h>

#include "version.h"

static void usage(FILE *out)
{
    fputs("usage: acsh [--help | --version]\n", out);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("acsh %s\n", ACSH_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc >= 2) {
        fprintf(stderr, "acsh: unknown option '%s'\n", argv[1]);
    }
    usage(stderr);
    return 2;
}
