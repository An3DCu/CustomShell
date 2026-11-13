/*
 * wc: count lines, words and bytes.
 *
 *     wc [-lwc] [file ...]
 *
 * No options means all three. The columns always come in the order lines,
 * words, bytes, whatever order the options were given in. With more than one
 * file there's a "total" line. With no file, or "-", it reads stdin.
 *
 * A line is a newline character. A last line without one isn't counted
 * (POSIX says so, and GNU wc agrees). A word is a run of bytes that aren't
 * space, tab, newline, \v, \f or \r. Bytes are counted. Characters aren't.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tool.h"

struct counts {
    unsigned long long lines, words, bytes;
};

static bool show_lines, show_words, show_bytes;

static bool is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Returns false after a read error (reported). */
static bool count_fd(int fd, const char *name, struct counts *c)
{
    static char buf[64 * 1024];
    bool in_word = false;

    for (;;) {
        ssize_t n = tool_read(fd, buf, sizeof buf);

        if (n < 0) {
            tool_error(name, errno);
            return false;
        }
        if (n == 0)
            return true;
        c->bytes += (unsigned long long)n;
        for (ssize_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];

            if (ch == '\n')
                c->lines++;
            if (is_space(ch)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                c->words++;
            }
        }
    }
}

static int digits(unsigned long long n)
{
    int d = 1;

    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static void print_row(const struct counts *c, int width, const char *name)
{
    const char *sep = "";

    if (show_lines) {
        printf("%*llu", width, c->lines);
        sep = " ";
    }
    if (show_words) {
        printf("%s%*llu", sep, width, c->words);
        sep = " ";
    }
    if (show_bytes)
        printf("%s%*llu", sep, width, c->bytes);
    if (name != NULL)
        printf(" %s", name);
    putchar('\n');
}

static void usage(FILE *to)
{
    fputs("usage: wc [-lwc] [file ...]\n", to);
}

int main(int argc, char **argv)
{
    struct counts *rows;
    struct counts total = { 0 };
    bool *ok;
    int nfiles;
    int opt;
    int status = 0;
    int width = 1;

    tool_init("wc");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    opterr = 0;
    while ((opt = getopt(argc, argv, "lwc")) != -1) {
        switch (opt) {
        case 'l': show_lines = true; break;
        case 'w': show_words = true; break;
        case 'c': show_bytes = true; break;
        default:
            tool_message("unknown option '-%c'", optopt);
            usage(stderr);
            return 2;
        }
    }
    if (!show_lines && !show_words && !show_bytes)
        show_lines = show_words = show_bytes = true;

    nfiles = argc - optind;
    rows = calloc(nfiles > 0 ? (size_t)nfiles : 1, sizeof *rows);
    ok = calloc(nfiles > 0 ? (size_t)nfiles : 1, sizeof *ok);
    if (rows == NULL || ok == NULL) {
        free(rows);
        free(ok);
        tool_error("memory", ENOMEM);
        return 1;
    }

    /* Count everything first. The columns are as wide as the biggest
     * number, total included. They then line up. */
    for (int i = 0; i < (nfiles > 0 ? nfiles : 1); i++) {
        const char *name = nfiles > 0 ? argv[optind + i] : "-";
        int fd = tool_open_input(name);

        if (fd < 0) {
            tool_error(name, errno);
            status = 1;
            continue;
        }
        ok[i] = count_fd(fd, name, &rows[i]);
        tool_close_input(fd);
        if (!ok[i])
            status = 1;
        total.lines += rows[i].lines;
        total.words += rows[i].words;
        total.bytes += rows[i].bytes;
    }
    if (show_lines && digits(total.lines) > width)
        width = digits(total.lines);
    if (show_words && digits(total.words) > width)
        width = digits(total.words);
    if (show_bytes && digits(total.bytes) > width)
        width = digits(total.bytes);

    if (nfiles == 0) {
        if (ok[0])
            print_row(&rows[0], width, NULL);
    } else {
        for (int i = 0; i < nfiles; i++) {
            if (ok[i])
                print_row(&rows[i], width, argv[optind + i]);
        }
        if (nfiles > 1)
            print_row(&total, width, "total");
    }
    free(rows);
    free(ok);
    if (fflush(stdout) != 0) {
        tool_error("write error", errno);
        return 1;
    }
    return status;
}
