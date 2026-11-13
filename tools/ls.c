/*
 * ls: list folders.
 *
 *     ls [-1aCl] [file ...]
 *
 *   -a  also show names starting with a dot, . and .. included
 *   -l  long format: type and permissions, links, owner, group, size, time
 *   -1  one name per line (the default when the output isn't a terminal)
 *   -C  names in columns (the default in a terminal)
 *
 * Names are sorted byte by byte, the way GNU ls sorts in the C locale. The
 * column layout follows GNU's rule: as many columns as fit the width, filled
 * top to bottom, two spaces between them. The width comes from COLUMNS if
 * it's set, otherwise from the terminal, otherwise 80.
 *
 * Several operands: files are listed first, then each folder under a
 * "name:" heading. Limits: names are printed as they are (GNU quotes odd
 * names in a terminal), and widths count bytes, so UTF-8 names with
 * letters like č take more room than they need.
 */

/* The sticky bit (S_ISVTX) isn't in base POSIX but in its X/Open part,
 * XSI. Version 700 includes everything POSIX.1-2008 has. */
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "tool.h"

enum layout { LAYOUT_AUTO, LAYOUT_ONE_PER_LINE, LAYOUT_COLUMNS };

static bool show_all;
static bool long_format;
static enum layout layout = LAYOUT_AUTO;
static int status;

struct entry {
    char *name;   /* as shown: the name in a folder, or the operand as typed */
    char *target; /* where a symbolic link points, for -l */
    struct stat st;
    bool have_stat;
};

struct list {
    struct entry *items;
    size_t len;
    size_t cap;
};

static void *xalloc(size_t size)
{
    void *p = malloc(size ? size : 1);

    if (p == NULL) {
        tool_error("memory", ENOMEM);
        exit(1);
    }
    return p;
}

static char *xdup(const char *s)
{
    size_t n = strlen(s) + 1;

    return memcpy(xalloc(n), s, n);
}

static struct entry *push(struct list *list, const char *name)
{
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 32;
        list->items = realloc(list->items, list->cap * sizeof *list->items);
        if (list->items == NULL) {
            tool_error("memory", ENOMEM);
            exit(1);
        }
    }
    list->items[list->len] = (struct entry){ .name = xdup(name) };
    return &list->items[list->len++];
}

static void free_list(struct list *list)
{
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].name);
        free(list->items[i].target);
    }
    free(list->items);
    *list = (struct list){ 0 };
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

/* Where a symbolic link points, as a new string, or NULL. */
static char *read_link(const char *path, off_t size_hint)
{
    size_t size = size_hint > 0 ? (size_t)size_hint + 1 : 256;

    for (;;) {
        char *buf = xalloc(size);
        ssize_t n = readlink(path, buf, size);

        if (n < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)n < size) {
            buf[n] = '\0';
            return buf;
        }
        free(buf);
        size *= 2;
    }
}

/* Fills in st (and the link target) for -l. path is how to reach it. */
static void look_up(struct entry *e, const char *path)
{
    if (lstat(path, &e->st) != 0) {
        tool_error(path, errno);
        status = 1;
        return;
    }
    e->have_stat = true;
    if (S_ISLNK(e->st.st_mode))
        e->target = read_link(path, e->st.st_size);
}

/* ---- long format --------------------------------------------------------- */

/* drwxr-xr-x, with s/S for setuid and setgid and t/T for sticky, as ls -l. */
static void mode_string(mode_t m, char out[11])
{
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c'
           : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_ISUID) ? ((m & S_IXUSR) ? 's' : 'S') : ((m & S_IXUSR) ? 'x' : '-');
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_ISGID) ? ((m & S_IXGRP) ? 's' : 'S') : ((m & S_IXGRP) ? 'x' : '-');
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_ISVTX) ? ((m & S_IXOTH) ? 't' : 'T') : ((m & S_IXOTH) ? 'x' : '-');
    out[10] = '\0';
}

static bool later(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec > b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

/* "Nov  6 14:03" for the last six months, "Nov  6  2025" for anything
 * older or in the future, like GNU ls. The clock is read once, to the
 * nanosecond. A file that looks newer than that may just have been changed
 * since, so the clock is read again before the file counts as the future,
 * as GNU ls does. time(), which only has whole seconds from a clock that
 * lags behind file times, called a file made a moment ago the future. */
static void time_string(const struct timespec *t, char *out, size_t size)
{
    static struct timespec now;
    const time_t six_months = 31556952 / 2; /* half a Gregorian year */
    struct tm tm;

    if (now.tv_sec == 0 || later(t, &now))
        clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&t->tv_sec, &tm);
    strftime(out, size, (t->tv_sec > now.tv_sec - six_months && !later(t, &now))
                            ? "%b %e %H:%M" : "%b %e  %Y", &tm);
}

static void owner_name(uid_t uid, char *out, size_t size)
{
    const struct passwd *pw = getpwuid(uid);

    if (pw != NULL)
        snprintf(out, size, "%s", pw->pw_name);
    else
        snprintf(out, size, "%ju", (uintmax_t)uid);
}

static void group_name(gid_t gid, char *out, size_t size)
{
    const struct group *gr = getgrgid(gid);

    if (gr != NULL)
        snprintf(out, size, "%s", gr->gr_name);
    else
        snprintf(out, size, "%ju", (uintmax_t)gid);
}

/* The size column: bytes, or "major, minor" for a device. */
static void size_string(const struct stat *st, char *out, size_t size)
{
    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
        snprintf(out, size, "%u, %u", major(st->st_rdev), minor(st->st_rdev));
    else
        snprintf(out, size, "%jd", (intmax_t)st->st_size);
}

static int width_of(const char *s)
{
    return (int)strlen(s);
}

struct widths {
    int links, owner, group, size;
};

/* Widens w to fit every entry of list, and adds up their blocks. */
static void measure(const struct list *list, struct widths *w, unsigned long long *blocks)
{
    char buf[64];

    for (size_t i = 0; i < list->len; i++) {
        const struct entry *e = &list->items[i];

        if (!e->have_stat)
            continue;
        snprintf(buf, sizeof buf, "%ju", (uintmax_t)e->st.st_nlink);
        if (width_of(buf) > w->links)
            w->links = width_of(buf);
        owner_name(e->st.st_uid, buf, sizeof buf);
        if (width_of(buf) > w->owner)
            w->owner = width_of(buf);
        group_name(e->st.st_gid, buf, sizeof buf);
        if (width_of(buf) > w->group)
            w->group = width_of(buf);
        size_string(&e->st, buf, sizeof buf);
        if (width_of(buf) > w->size)
            w->size = width_of(buf);
        *blocks += (unsigned long long)e->st.st_blocks;
    }
}

/*
 * also_measure: entries that don't print here but still count for the
 * column widths. GNU ls measures every operand, folders included, before
 * it moves the folders out to list them on their own; matching that keeps
 * `ls -l file folder` byte for byte the same.
 */
static void print_long(const struct list *list, const struct list *also_measure,
                       bool with_total)
{
    struct widths w = { 1, 1, 1, 1 };
    unsigned long long blocks = 0, unused = 0;
    int w_links, w_owner, w_group, w_size;

    measure(list, &w, &blocks);
    if (also_measure != NULL)
        measure(also_measure, &w, &unused);
    w_links = w.links;
    w_owner = w.owner;
    w_group = w.group;
    w_size = w.size;
    /* st_blocks counts 512-byte blocks; ls shows 1024-byte ones, rounded up. */
    if (with_total)
        printf("total %llu\n", (blocks + 1) / 2);

    for (size_t i = 0; i < list->len; i++) {
        const struct entry *e = &list->items[i];
        char mode[11], owner[64], group[64], size[64], when[64];

        if (!e->have_stat)
            continue;
        mode_string(e->st.st_mode, mode);
        owner_name(e->st.st_uid, owner, sizeof owner);
        group_name(e->st.st_gid, group, sizeof group);
        size_string(&e->st, size, sizeof size);
        time_string(&e->st.st_mtim, when, sizeof when);
        printf("%s %*ju %-*s %-*s %*s %s %s", mode, w_links, (uintmax_t)e->st.st_nlink,
               w_owner, owner, w_group, group, w_size, size, when, e->name);
        if (e->target != NULL)
            printf(" -> %s", e->target);
        putchar('\n');
    }
}

/* ---- names only ---------------------------------------------------------- */

static size_t line_width(void)
{
    const char *columns = getenv("COLUMNS");
    struct winsize ws;

    if (columns != NULL) {
        unsigned long long n;

        if (tool_parse_count(columns, &n) && n > 0 && n < 100000)
            return (size_t)n;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/*
 * GNU's rule: try the most columns first. With c columns there are
 * ceil(n / c) rows, and name i goes in column i / rows. Every column is as
 * wide as its longest name plus 2, except the last. The first c whose total
 * is less than the width wins.
 */
static void print_columns(const struct list *list)
{
    size_t n = list->len;
    size_t width = line_width();
    size_t max_cols = width / 3 > 0 ? width / 3 : 1; /* a name of 1 + 2 spaces */
    size_t cols, rows;
    size_t *col_width;

    if (n == 0)
        return;
    if (max_cols > n)
        max_cols = n;
    col_width = xalloc(max_cols * sizeof *col_width);
    for (cols = max_cols; cols > 1; cols--) {
        size_t total = 0;

        rows = (n + cols - 1) / cols;
        memset(col_width, 0, cols * sizeof *col_width);
        for (size_t i = 0; i < n; i++) {
            size_t c = i / rows;
            size_t len = strlen(list->items[i].name) + (c == cols - 1 ? 0 : 2);

            if (len > col_width[c])
                col_width[c] = len;
        }
        for (size_t c = 0; c < cols; c++)
            total += col_width[c];
        if (total < width)
            break;
    }
    if (cols == 1)
        col_width[0] = 0; /* one column: no padding at all */
    rows = (n + cols - 1) / cols;

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0, i = r; i < n; c++, i += rows) {
            const char *name = list->items[i].name;

            fputs(name, stdout);
            if (i + rows < n) /* another name follows on this row */
                printf("%*s", (int)(col_width[c] - strlen(name)), "");
        }
        putchar('\n');
    }
    free(col_width);
}

static void print_list(const struct list *list, const struct list *also_measure,
                       bool is_folder_listing)
{
    if (long_format) {
        print_long(list, also_measure, is_folder_listing);
    } else if (layout == LAYOUT_COLUMNS) {
        print_columns(list);
    } else {
        for (size_t i = 0; i < list->len; i++)
            puts(list->items[i].name);
    }
}

/* ---- folders --------------------------------------------------------------- */

static void list_folder(const char *path)
{
    struct list list = { 0 };
    DIR *dir = opendir(path);
    const struct dirent *de;

    if (dir == NULL) {
        tool_error(path, errno);
        status = 1;
        return;
    }
    for (errno = 0; (de = readdir(dir)) != NULL; errno = 0) {
        if (!show_all && de->d_name[0] == '.')
            continue;
        push(&list, de->d_name);
    }
    if (errno != 0) {
        tool_error(path, errno);
        status = 1;
    }
    closedir(dir);

    if (long_format) {
        for (size_t i = 0; i < list.len; i++) {
            char *full = xalloc(strlen(path) + strlen(list.items[i].name) + 2);

            sprintf(full, "%s/%s", path, list.items[i].name);
            look_up(&list.items[i], full);
            free(full);
        }
    }
    if (list.len > 1)
        qsort(list.items, list.len, sizeof *list.items, by_name);
    print_list(&list, NULL, true);
    free_list(&list);
}

static void usage(FILE *to)
{
    fputs("usage: ls [-1aCl] [file ...]\n", to);
}

int main(int argc, char **argv)
{
    struct list files = { 0 };
    struct list folders = { 0 };
    bool printed = false;
    int opt;

    tool_init("ls");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    opterr = 0;
    while ((opt = getopt(argc, argv, "1aCl")) != -1) {
        switch (opt) {
        case '1': layout = LAYOUT_ONE_PER_LINE; break;
        case 'a': show_all = true; break;
        case 'C': layout = LAYOUT_COLUMNS; break;
        case 'l': long_format = true; break;
        default:
            tool_message("unknown option '-%c'", optopt);
            usage(stderr);
            return 2;
        }
    }
    if (layout == LAYOUT_AUTO)
        layout = isatty(STDOUT_FILENO) ? LAYOUT_COLUMNS : LAYOUT_ONE_PER_LINE;

    if (optind == argc) {
        list_folder(".");
        return fflush(stdout) == 0 ? status : 1;
    }

    /* Sort the operands into files and folders. A symbolic link to a folder
     * counts as the folder, except with -l, which shows the link itself. */
    for (int i = optind; i < argc; i++) {
        struct stat st;
        int rc = long_format ? lstat(argv[i], &st) : stat(argv[i], &st);

        if (rc != 0 && !long_format && errno == ENOENT && lstat(argv[i], &st) == 0)
            rc = 0; /* a link that points nowhere: list the link */
        if (rc != 0) {
            tool_error(argv[i], errno);
            status = 1;
            continue;
        }
        /* With -l, folders are looked up too: they count for the widths of
         * the files' columns (see print_long). */
        struct entry *e = push(S_ISDIR(st.st_mode) ? &folders : &files, argv[i]);

        if (long_format)
            look_up(e, argv[i]);
    }
    if (files.len > 1)
        qsort(files.items, files.len, sizeof *files.items, by_name);
    if (folders.len > 1)
        qsort(folders.items, folders.len, sizeof *folders.items, by_name);

    if (files.len > 0) {
        print_list(&files, &folders, false);
        printed = true;
    }
    for (size_t i = 0; i < folders.len; i++) {
        /* Headings only when there's more than one thing to list. */
        if (argc - optind > 1)
            printf("%s%s:\n", printed ? "\n" : "", folders.items[i].name);
        list_folder(folders.items[i].name);
        printed = true;
    }
    free_list(&files);
    free_list(&folders);
    if (fflush(stdout) != 0) {
        tool_error("write error", errno);
        return 1;
    }
    return status;
}
