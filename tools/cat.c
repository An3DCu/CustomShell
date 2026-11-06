/*
 * cat: print files one after another.
 *
 *     cat [-n] [file ...]
 *
 * With no file, or "-", it reads stdin. -n numbers every line, the way GNU
 * cat does ("%6d" and a tab), counting on across files. Reads and writes
 * raw bytes, so a file with a 0 byte in it comes out whole. (The old acsh
 * read lines into C strings and stopped at the first 0.)
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tool.h"

#define BUF_SIZE (64 * 1024)

static bool number_lines;
static unsigned long long line_number;
static bool at_line_start = true;

/* Output for -n, collected so a numbered file isn't one write per line. */
static char out[BUF_SIZE];
static size_t out_len;

static bool flush_out(void)
{
    bool ok = tool_write_all(STDOUT_FILENO, out, out_len);

    out_len = 0;
    return ok;
}

static bool put(const char *p, size_t n)
{
    while (n > 0) {
        size_t room = sizeof out - out_len;
        size_t chunk = n < room ? n : room;

        memcpy(out + out_len, p, chunk);
        out_len += chunk;
        p += chunk;
        n -= chunk;
        if (out_len == sizeof out && !flush_out())
            return false;
    }
    return true;
}

/* Copies buf to the output, with a line number before every line. */
static bool put_numbered(const char *buf, size_t n)
{
    while (n > 0) {
        const char *newline;
        size_t len;

        if (at_line_start) {
            char prefix[32];
            int k = snprintf(prefix, sizeof prefix, "%6llu\t", ++line_number);

            if (!put(prefix, (size_t)k))
                return false;
            at_line_start = false;
        }
        newline = memchr(buf, '\n', n);
        len = newline ? (size_t)(newline - buf) + 1 : n;
        if (!put(buf, len))
            return false;
        if (newline)
            at_line_start = true;
        buf += len;
        n -= len;
    }
    return true;
}

/* Returns 0 when done, 1 after a read error (reported, go on with the next
 * file), -1 after a write error (stop: nobody is reading any more). */
static int cat_fd(int fd, const char *name)
{
    static char buf[BUF_SIZE];

    for (;;) {
        ssize_t n = tool_read(fd, buf, sizeof buf);
        bool ok;

        if (n < 0) {
            tool_error(name, errno);
            return 1;
        }
        if (n == 0)
            return 0;
        /* Flush after every read, so typing into `cat -n` shows each line
         * right away instead of waiting for 64 KB. */
        if (number_lines)
            ok = put_numbered(buf, (size_t)n) && flush_out();
        else
            ok = tool_write_all(STDOUT_FILENO, buf, (size_t)n);
        if (!ok)
            return -1;
    }
}

static void usage(FILE *to)
{
    fputs("usage: cat [-n] [file ...]\n", to);
}

int main(int argc, char **argv)
{
    static char *stdin_only[] = { "-", NULL };
    char **files;
    int opt;
    int status = 0;

    tool_init("cat");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    opterr = 0; /* my own message instead of getopt's */
    while ((opt = getopt(argc, argv, "n")) != -1) {
        if (opt == 'n') {
            number_lines = true;
        } else {
            tool_message("unknown option '-%c'", optopt);
            usage(stderr);
            return 2;
        }
    }
    files = optind < argc ? argv + optind : stdin_only;

    for (; *files != NULL; files++) {
        int fd = tool_open_input(*files);
        int result;

        if (fd < 0) {
            tool_error(*files, errno);
            status = 1;
            continue;
        }
        result = cat_fd(fd, *files);
        tool_close_input(fd);
        if (result < 0) {
            tool_error("write error", errno);
            return 1;
        }
        if (result > 0)
            status = 1;
    }
    if (!flush_out()) {
        tool_error("write error", errno);
        return 1;
    }
    return status;
}
