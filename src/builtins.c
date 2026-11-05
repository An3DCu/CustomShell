#include "builtins.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xalloc.h"

/* ---- cd ----------------------------------------------------------------- */

static int builtin_cd(struct shell *sh, int argc, char **argv)
{
    const char *dir;
    bool print = false;
    char *before, *after;

    (void)sh;
    if (argc > 2) {
        fputs("acsh: cd: too many arguments\n", stderr);
        return 1;
    }
    if (argc == 1) {
        dir = getenv("HOME");
        if (dir == NULL || dir[0] == '\0') {
            fputs("acsh: cd: HOME not set\n", stderr);
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        dir = getenv("OLDPWD");
        if (dir == NULL || dir[0] == '\0') {
            fputs("acsh: cd: OLDPWD not set\n", stderr);
            return 1;
        }
        print = true; /* like bash: say where "-" went */
    } else {
        dir = argv[1];
    }

    before = xgetcwd(); /* NULL if the current folder was deleted */
    if (chdir(dir) != 0) {
        fprintf(stderr, "acsh: cd: %s: %s\n", dir, strerror(errno));
        free(before);
        return 1;
    }
    /* dir may point into the environment, which setenv can change, so it
     * isn't used after this. */
    after = xgetcwd();
    if (before != NULL)
        setenv("OLDPWD", before, 1);
    if (after != NULL) {
        setenv("PWD", after, 1);
        if (print)
            printf("%s\n", after);
    }
    free(before);
    free(after);
    return 0;
}

/* ---- pwd ---------------------------------------------------------------- */

static int builtin_pwd(struct shell *sh, int argc, char **argv)
{
    char *cwd = xgetcwd();

    (void)sh;
    (void)argc;
    (void)argv;
    if (cwd == NULL) {
        fprintf(stderr, "acsh: pwd: %s\n", strerror(errno));
        return 1;
    }
    printf("%s\n", cwd);
    free(cwd);
    return 0;
}

/* ---- echo --------------------------------------------------------------- */

/* Like bash: an argument is an option only if it's "-" followed by nothing
 * but n, e and E. So -nE is an option, while -x, - and -- are printed. */
static bool is_echo_option(const char *arg)
{
    if (arg[0] != '-' || arg[1] == '\0')
        return false;
    for (const char *p = arg + 1; *p != '\0'; p++) {
        if (*p != 'n' && *p != 'e' && *p != 'E')
            return false;
    }
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Prints s with echo -e escapes. Returns false at \c, which means "stop
 * printing, no newline either". */
static bool print_escaped(const char *s)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        int value;

        if (s[i] != '\\' || s[i + 1] == '\0') {
            putchar(s[i]);
            continue;
        }
        i++;
        switch (s[i]) {
        case 'a':  putchar('\a'); break;
        case 'b':  putchar('\b'); break;
        case 'c':  return false;
        case 'e':
        case 'E':  putchar('\033'); break;
        case 'f':  putchar('\f'); break;
        case 'n':  putchar('\n'); break;
        case 'r':  putchar('\r'); break;
        case 't':  putchar('\t'); break;
        case 'v':  putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0': /* \0nnn: up to three octal digits */
            value = 0;
            for (int d = 0; d < 3 && s[i + 1] >= '0' && s[i + 1] <= '7'; d++)
                value = value * 8 + (s[++i] - '0');
            putchar(value & 0xff);
            break;
        case 'x': /* \xHH: one or two hex digits */
            if (hex_value(s[i + 1]) < 0) {
                fputs("\\x", stdout);
                break;
            }
            value = hex_value(s[++i]);
            if (hex_value(s[i + 1]) >= 0)
                value = value * 16 + hex_value(s[++i]);
            putchar(value);
            break;
        default: /* not an escape: print it as written */
            putchar('\\');
            putchar(s[i]);
            break;
        }
    }
    return true;
}

static int builtin_echo(struct shell *sh, int argc, char **argv)
{
    bool newline = true;
    bool escapes = false;
    int i = 1;

    (void)sh;
    for (; i < argc && is_echo_option(argv[i]); i++) {
        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            if (*p == 'n')
                newline = false;
            else if (*p == 'e')
                escapes = true;
            else
                escapes = false;
        }
    }
    for (; i < argc; i++) {
        if (escapes) {
            if (!print_escaped(argv[i]))
                return 0;
        } else {
            fputs(argv[i], stdout);
        }
        if (i + 1 < argc)
            putchar(' ');
    }
    if (newline)
        putchar('\n');
    return 0;
}

/* ---- exit --------------------------------------------------------------- */

/* A whole decimal number with an optional sign, and nothing else. */
static bool parse_number(const char *s, long *out)
{
    char *end;

    if (s[0] == '\0')
        return false;
    errno = 0;
    *out = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0')
        return false;
    /* strtol also skips leading blanks; a number here starts right away. */
    return s[0] == '-' || s[0] == '+' || (s[0] >= '0' && s[0] <= '9');
}

static int builtin_exit(struct shell *sh, int argc, char **argv)
{
    int status = sh->status;
    long n;

    if (argc > 2) {
        fputs("acsh: exit: too many arguments\n", stderr);
        return 1; /* like bash: an error, but the shell stays */
    }
    if (argc == 2) {
        if (parse_number(argv[1], &n)) {
            /* An exit status is one byte: 256 is 0, -1 is 255. */
            status = (int)((unsigned long)n & 0xff);
        } else {
            fprintf(stderr, "acsh: exit: %s: numeric argument required\n", argv[1]);
            status = 2;
        }
    }
    sh->exiting = true;
    return status;
}

/* ---- help --------------------------------------------------------------- */

static int builtin_help(struct shell *sh, int argc, char **argv);

static const struct builtin builtins[] = {
    { "cd",   builtin_cd,   "cd [dir | -]",
      "Go to dir. With no dir, go home; with -, go back to the last folder." },
    { "echo", builtin_echo, "echo [-neE] [word ...]",
      "Print the words. -n: no newline at the end. -e: read \\n, \\t and other escapes." },
    { "exit", builtin_exit, "exit [n]",
      "Leave acsh with status n, or with the last command's status." },
    { "help", builtin_help, "help [name]",
      "List the built-ins, or show one." },
    { "pwd",  builtin_pwd,  "pwd",
      "Print the current folder." },
};

#define NBUILTINS (sizeof builtins / sizeof builtins[0])

static void print_help_line(const struct builtin *b)
{
    printf("  %-24s %s\n", b->usage, b->summary);
}

static int builtin_help(struct shell *sh, int argc, char **argv)
{
    int status = 0;

    (void)sh;
    if (argc == 1) {
        puts("acsh built-ins:");
        for (size_t i = 0; i < NBUILTINS; i++)
            print_help_line(&builtins[i]);
        puts("Anything else runs as a program from PATH.");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const struct builtin *b = builtin_find(argv[i]);

        if (b != NULL) {
            print_help_line(b);
        } else {
            fprintf(stderr, "acsh: help: no help for '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

/* ---- lookup and running ------------------------------------------------- */

const struct builtin *builtin_find(const char *name)
{
    for (size_t i = 0; i < NBUILTINS; i++) {
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];
    }
    return NULL;
}

int builtin_run(struct shell *sh, const struct builtin *b, char **argv)
{
    int argc = 0;
    int status;

    while (argv[argc] != NULL)
        argc++;
    clearerr(stdout);
    status = b->run(sh, argc, argv);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "acsh: %s: write error: %s\n", b->name, strerror(errno));
        clearerr(stdout);
        if (status == 0)
            status = 1;
    }
    return status;
}
