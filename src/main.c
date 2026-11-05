/*
 * acsh: a small Unix shell.
 *
 * The main loop: show a prompt (only on a terminal), read a line, parse it,
 * run it, remember its exit status, and repeat until exit or end of input.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "exec.h"
#include "input.h"
#include "parser.h"
#include "prompt.h"
#include "shell.h"
#include "version.h"

static void usage(FILE *out)
{
    fputs("usage: acsh [--help | --version]\n", out);
}

static void run_line(struct shell *sh, const char *line)
{
    struct pipeline *pl;
    char err[256];

    switch (parse_line(line, &pl, err, sizeof err)) {
    case PARSE_EMPTY:
        return; /* an empty line keeps the last status, as in bash */
    case PARSE_INCOMPLETE:
    case PARSE_SYNTAX:
        fprintf(stderr, "acsh: %s\n", err);
        sh->status = 2;
        return;
    case PARSE_OK:
        sh->status = run_pipeline(sh, pl);
        pipeline_free(pl);
        return;
    }
}

static int shell_loop(struct shell *sh)
{
    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        if (sh->interactive)
            prompt_print();
        if (input_read_line(STDIN_FILENO, &line, &cap) < 0) {
            if (errno != 0)
                fprintf(stderr, "acsh: reading input: %s\n", strerror(errno));
            else if (sh->interactive)
                fputc('\n', stderr); /* Ctrl+D: end the prompt's line */
            break;
        }
        run_line(sh, line);
        if (sh->exiting)
            break;
    }
    free(line);
    return sh->status;
}

int main(int argc, char **argv)
{
    struct shell sh = { 0 };

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
        usage(stderr);
        return 2;
    }

    /* POSIX: a shell is interactive when stdin and stderr are terminals. */
    sh.interactive = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    return shell_loop(&sh);
}
