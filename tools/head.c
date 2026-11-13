/*
 * head: print the first lines of files.
 *
 *     head [-n count] [file ...]
 *
 * Ten lines unless -n says otherwise. With more than one file, each gets a
 * "==> name <==" header, as in GNU head. With no file, or "-", it reads
 * stdin.
 *
 * It stops reading as soon as it has its lines. In `yes | head -n 3` that's
 * what ends yes. head exits. The pipe has no reader left. yes gets SIGPIPE
 * on its next write.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tool.h"

/* Returns 0 when done, 1 after a read error (reported), -1 after a write
 * error. */
static int head_fd(int fd, const char *name, unsigned long long lines)
{
    static char buf[64 * 1024];

    while (lines > 0) {
        ssize_t n = tool_read(fd, buf, sizeof buf);
        size_t len = 0;

        if (n < 0) {
            tool_error(name, errno);
            return 1;
        }
        if (n == 0)
            return 0;
        /* Take whole lines from this chunk until the count runs out. */
        while (len < (size_t)n && lines > 0) {
            const char *newline = memchr(buf + len, '\n', (size_t)n - len);

            if (newline == NULL) {
                len = (size_t)n; /* the rest is part of a line that goes on */
                break;
            }
            len = (size_t)(newline - buf) + 1;
            lines--;
        }
        if (!tool_write_all(STDOUT_FILENO, buf, len))
            return -1;
    }
    return 0;
}

static void usage(FILE *to)
{
    fputs("usage: head [-n count] [file ...]\n", to);
}

int main(int argc, char **argv)
{
    static char *stdin_only[] = { "-", NULL };
    unsigned long long lines = 10;
    char **files;
    int nfiles;
    int opt;
    int status = 0;

    tool_init("head");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    opterr = 0;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        if (opt == 'n') {
            if (!tool_parse_count(optarg, &lines)) {
                tool_message("invalid number of lines: '%s'", optarg);
                return 2;
            }
        } else if (optopt == 'n') {
            tool_message("-n needs a number");
            usage(stderr);
            return 2;
        } else {
            tool_message("unknown option '-%c'", optopt);
            usage(stderr);
            return 2;
        }
    }
    files = optind < argc ? argv + optind : stdin_only;
    nfiles = optind < argc ? argc - optind : 1;

    for (int i = 0; i < nfiles; i++) {
        const char *name = files[i];
        int fd = tool_open_input(name);
        int result;

        if (fd < 0) {
            tool_error(name, errno);
            status = 1;
            continue;
        }
        if (nfiles > 1) {
            char header[4096];
            int k = snprintf(header, sizeof header, "%s==> %s <==\n", i > 0 ? "\n" : "",
                             strcmp(name, "-") == 0 ? "standard input" : name);

            if (k < 0 || (size_t)k >= sizeof header)
                k = (int)sizeof header - 1;
            if (!tool_write_all(STDOUT_FILENO, header, (size_t)k)) {
                tool_error("write error", errno);
                tool_close_input(fd);
                return 1;
            }
        }
        result = head_fd(fd, name, lines);
        tool_close_input(fd);
        if (result < 0) {
            tool_error("write error", errno);
            return 1;
        }
        if (result > 0)
            status = 1;
    }
    return status;
}
