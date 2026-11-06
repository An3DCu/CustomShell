#include "builtins.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "error.h"
#include "explain.h"
#include "input.h"
#include "script.h"
#include "strvec.h"
#include "vars.h"
#include "xalloc.h"

/* ---- cd ----------------------------------------------------------------- */

/* PWD and OLDPWD go to children too, as in bash. */
static void set_exported(struct shell *sh, const char *name, const char *value)
{
    vars_set(&sh->vars, name, value);
    vars_export(&sh->vars, name);
}

static int builtin_cd(struct shell *sh, int argc, char **argv)
{
    const char *dir;
    bool print = false;
    char *before, *after;

    if (argc > 2) {
        shell_error(sh, "cd: too many arguments");
        return 1;
    }
    if (argc == 1) {
        dir = vars_get(&sh->vars, "HOME");
        if (dir == NULL || dir[0] == '\0') {
            shell_error(sh, "cd: HOME not set");
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        dir = vars_get(&sh->vars, "OLDPWD");
        if (dir == NULL || dir[0] == '\0') {
            shell_error(sh, "cd: OLDPWD not set");
            return 1;
        }
        print = true; /* like bash: say where "-" went */
    } else {
        dir = argv[1];
    }

    before = xgetcwd(); /* NULL if the current folder was deleted */
    if (chdir(dir) != 0) {
        shell_error(sh, "cd: %s: %s", dir, strerror(errno));
        free(before);
        return 1;
    }
    /* dir may point into the variable table, which vars_set changes, so it
     * isn't used after this. */
    after = xgetcwd();
    if (before != NULL)
        set_exported(sh, "OLDPWD", before);
    if (after != NULL) {
        set_exported(sh, "PWD", after);
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
        shell_error(sh, "pwd: %s", strerror(errno));
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
        shell_error(sh, "exit: too many arguments");
        return 1; /* like bash: an error, but the shell stays */
    }
    if (argc == 2) {
        if (parse_number(argv[1], &n)) {
            /* An exit status is one byte: 256 is 0, -1 is 255. */
            status = (int)((unsigned long)n & 0xff);
        } else {
            shell_error(sh, "exit: %s: numeric argument required", argv[1]);
            status = 2;
        }
    }
    sh->exiting = true;
    return status;
}

/* ---- export and unset --------------------------------------------------- */

/* Prints s in single quotes so the shell can read it back. A ' inside
 * closes the quotes, adds "'" and opens them again, as dash prints it:
 * it's -> 'it'"'"'s'. */
static void print_quoted(const char *s)
{
    putchar('\'');
    for (; *s != '\0'; s++) {
        if (*s == '\'')
            fputs("'\"'\"'", stdout);
        else
            putchar(*s);
    }
    putchar('\'');
}

/* export -p, in the form dash uses: export NAME='value', sorted by name. */
static void print_exports(const struct shell *sh)
{
    size_t n;
    const struct var **list = vars_sorted(&sh->vars, &n);

    for (size_t i = 0; i < n; i++) {
        if (!list[i]->exported || !var_name_valid(list[i]->name))
            continue;
        printf("export %s", list[i]->name);
        if (list[i]->value != NULL) {
            putchar('=');
            print_quoted(list[i]->value);
        }
        putchar('\n');
    }
    free(list);
}

static int builtin_export(struct shell *sh, int argc, char **argv)
{
    int status = 0;
    int i = 1;

    if (i < argc && strcmp(argv[i], "-p") == 0)
        i++;
    if (i == argc) {
        print_exports(sh);
        return 0;
    }
    for (; i < argc; i++) {
        size_t n = var_name_len(argv[i]);

        if (n == 0 || (argv[i][n] != '\0' && argv[i][n] != '=')) {
            shell_error(sh, "export: %s: bad variable name", argv[i]);
            status = 1;
            continue;
        }
        if (argv[i][n] == '=') {
            char *name = xstrndup(argv[i], n);

            vars_set(&sh->vars, name, argv[i] + n + 1);
            vars_export(&sh->vars, name);
            free(name);
        } else {
            vars_export(&sh->vars, argv[i]);
        }
    }
    return status;
}

static int builtin_unset(struct shell *sh, int argc, char **argv)
{
    int status = 0;
    int i = 1;

    if (i < argc && strcmp(argv[i], "-v") == 0)
        i++;
    if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        shell_error(sh, "unset: %s: invalid option", argv[i]);
        return 2;
    }
    for (; i < argc; i++) {
        if (!var_name_valid(argv[i])) {
            shell_error(sh, "unset: %s: bad variable name", argv[i]);
            status = 1;
            continue;
        }
        vars_unset(&sh->vars, argv[i]);
    }
    return status;
}

/* ---- shift -------------------------------------------------------------- */

static int builtin_shift(struct shell *sh, int argc, char **argv)
{
    long n = 1;

    if (argc > 2) {
        shell_error(sh, "shift: too many arguments");
        return 1;
    }
    if (argc == 2 && !parse_number(argv[1], &n)) {
        shell_error(sh, "shift: %s: numeric argument required", argv[1]);
        return 1;
    }
    if (n < 0 || (unsigned long)n > sh->params.len) {
        shell_error(sh, "shift: can't shift that many");
        return 1;
    }
    strvec_remove_front(&sh->params, (size_t)n);
    return 0;
}

/* ---- true, false and : -------------------------------------------------- */

/* Built in so `while true` and `while :` don't start a program every round. */
static int builtin_true(struct shell *sh, int argc, char **argv)
{
    (void)sh;
    (void)argc;
    (void)argv;
    return 0;
}

static int builtin_false(struct shell *sh, int argc, char **argv)
{
    (void)sh;
    (void)argc;
    (void)argv;
    return 1;
}

/* ---- break and continue ------------------------------------------------- */

/* break [n] and continue [n] only set a count; the loops act on it (see
 * loop_ends in exec.c). n bigger than the number of loops means all of
 * them, and outside a loop there's nothing to do, as in dash. */
static int loop_jump(struct shell *sh, int argc, char **argv, int *count)
{
    long n = 1;

    if (argc > 2) {
        shell_error(sh, "%s: too many arguments", argv[0]);
        return 1;
    }
    if (argc == 2 && (!parse_number(argv[1], &n) || n < 1)) {
        shell_error(sh, "%s: %s: loop count out of range", argv[0], argv[1]);
        return 1;
    }
    if (sh->loop_depth == 0)
        return 0;
    *count = n > sh->loop_depth ? sh->loop_depth : (int)n;
    return 0;
}

static int builtin_break(struct shell *sh, int argc, char **argv)
{
    return loop_jump(sh, argc, argv, &sh->breaks);
}

static int builtin_continue(struct shell *sh, int argc, char **argv)
{
    return loop_jump(sh, argc, argv, &sh->continues);
}

/* ---- read --------------------------------------------------------------- */

/* A line as read builds it: each byte, and whether a backslash escaped it
 * (an escaped byte never separates fields). */
struct read_buf {
    char *text;
    bool *escaped;
    size_t len;
    size_t cap;
};

static void read_buf_add(struct read_buf *b, char c, bool escaped)
{
    if (b->len + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->text = xreallocarray(b->text, b->cap, 1);
        b->escaped = xreallocarray(b->escaped, b->cap, sizeof *b->escaped);
    }
    b->text[b->len] = c;
    b->escaped[b->len] = escaped;
    b->len++;
}

/*
 * Reads one line from stdin into b, one byte at a time like the shell, so
 * the rest stays for the next command. Without -r, a backslash keeps the
 * next byte from separating fields, and a backslash at the end of the line
 * joins the next one. Returns 0 for a full line, 1 at the end of input
 * (b holds whatever came before it), or 130 after Ctrl+C.
 */
static int read_line_into(struct shell *sh, struct read_buf *b, bool raw)
{
    char *line = NULL;
    size_t cap = 0;
    int status = 0;

    for (;;) {
        ssize_t n = input_read_line(STDIN_FILENO, &line, &cap, sh->interactive);
        bool joined = false;

        if (n < 0) {
            if (errno == EINTR) {
                sh->interrupted = true;
                status = 130;
            } else {
                status = 1;
            }
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (line[i] == '\n')
                break;
            if (!raw && line[i] == '\\') {
                if (line[i + 1] == '\n') {
                    joined = true;
                    break;
                }
                if (i + 1 < n)
                    read_buf_add(b, line[++i], true);
                continue;
            }
            read_buf_add(b, line[i], false);
        }
        if (joined)
            continue;
        if (line[n - 1] != '\n')
            status = 1; /* the input ended without a newline */
        break;
    }
    free(line);
    return status;
}

static bool in_ifs(const char *ifs, char c)
{
    return strchr(ifs, c) != NULL;
}

static bool is_ifs_space(const char *ifs, char c)
{
    return (c == ' ' || c == '\t' || c == '\n') && in_ifs(ifs, c);
}

/* Steps over one field separator at *pos: IFS spaces, at most one other
 * IFS character, and IFS spaces again. */
static void skip_separator(const struct read_buf *b, const char *ifs, size_t *pos)
{
    while (*pos < b->len && !b->escaped[*pos] && is_ifs_space(ifs, b->text[*pos]))
        (*pos)++;
    if (*pos < b->len && !b->escaped[*pos] && in_ifs(ifs, b->text[*pos]))
        (*pos)++;
    while (*pos < b->len && !b->escaped[*pos] && is_ifs_space(ifs, b->text[*pos]))
        (*pos)++;
}

static bool is_separator(const struct read_buf *b, const char *ifs, size_t i)
{
    return !b->escaped[i] && in_ifs(ifs, b->text[i]);
}

/*
 * Splits b over the names, POSIX style: each name but the last gets one
 * field, and the last gets the rest of the line, without IFS spaces at
 * either end. If that rest is a single field plus one separator, the
 * separator goes too: with IFS=:, "a:b:" gives y=b but "a:b::" gives b::.
 */
static void assign_fields(struct shell *sh, const struct read_buf *b, char **names, int n)
{
    const char *ifs = vars_get(&sh->vars, "IFS");
    size_t pos = 0;

    if (ifs == NULL)
        ifs = " \t\n";
    for (int k = 0; k < n; k++) {
        size_t start, end;
        char *value;

        while (pos < b->len && !b->escaped[pos] && is_ifs_space(ifs, b->text[pos]))
            pos++;
        start = pos;
        if (k + 1 < n) {
            while (pos < b->len && !is_separator(b, ifs, pos))
                pos++;
            end = pos;
            skip_separator(b, ifs, &pos);
        } else {
            size_t q = start;

            end = b->len;
            while (end > start && !b->escaped[end - 1] && is_ifs_space(ifs, b->text[end - 1]))
                end--;
            while (q < end && !is_separator(b, ifs, q))
                q++;
            if (q < end) {
                size_t after = q;

                skip_separator(b, ifs, &after);
                if (after >= end)
                    end = q;
            }
        }
        value = xstrndup(b->text ? b->text + start : "", end - start);
        vars_set(&sh->vars, names[k], value);
        free(value);
    }
}

/* read [-r] [name ...]: one line from stdin into the names. With no name,
 * the whole line goes into REPLY, as in bash. */
static int builtin_read(struct shell *sh, int argc, char **argv)
{
    struct read_buf b = { 0 };
    const char *reply = "REPLY";
    bool raw = false;
    int i = 1;
    int status;

    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-r") != 0) {
            shell_error(sh, "read: %s: invalid option", argv[i]);
            return 2;
        }
        raw = true;
    }
    for (int k = i; k < argc; k++) {
        if (!var_name_valid(argv[k])) {
            shell_error(sh, "read: %s: bad variable name", argv[k]);
            return 2;
        }
    }
    fflush(stdout); /* a prompt printed with echo -n shows before we wait */
    status = read_line_into(sh, &b, raw);
    if (status != 130) {
        if (i < argc) {
            assign_fields(sh, &b, argv + i, argc - i);
        } else {
            char *line = xstrndup(b.text ? b.text : "", b.len);

            vars_set(&sh->vars, reply, line);
            free(line);
        }
    }
    free(b.text);
    free(b.escaped);
    return status;
}

/* ---- source and . -------------------------------------------------------- */

/* A loop of files sourcing each other stops here instead of running out of
 * file descriptors. */
#define MAX_SOURCE_DEPTH 100

static bool is_readable_file(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, R_OK) == 0;
}

/* A name with a / is used as it is. Otherwise it's looked for in PATH, as
 * POSIX says, and then in the current folder, as bash does, so
 * `source setup.sh` works. Returns a new string, or NULL. */
static char *find_sourced(const struct shell *sh, const char *name)
{
    const char *path = vars_get(&sh->vars, "PATH");

    if (strchr(name, '/') != NULL)
        return xstrdup(name);
    while (path != NULL) {
        const char *colon = strchr(path, ':');
        size_t dlen = colon ? (size_t)(colon - path) : strlen(path);
        char *candidate = xmalloc(dlen + strlen(name) + 3);

        if (dlen == 0) /* an empty entry means the current folder */
            sprintf(candidate, "./%s", name);
        else
            sprintf(candidate, "%.*s/%s", (int)dlen, path, name);
        if (is_readable_file(candidate))
            return candidate;
        free(candidate);
        path = colon ? colon + 1 : NULL;
    }
    if (is_readable_file(name))
        return xstrdup(name);
    return NULL;
}

/*
 * source file [arg ...] runs the file's commands in this shell, so its
 * variables and cd stay. With arguments, they are $1, $2, ... while it
 * runs, and the old ones come back after. Its status is the last
 * command's. An exit in the file leaves the shell, as POSIX says.
 */
static int builtin_source(struct shell *sh, int argc, char **argv)
{
    struct strvec saved = sh->params;
    struct reader r = { .name = argv[1] };
    char *path;
    int status;

    if (argc < 2) {
        shell_error(sh, "%s: filename argument required", argv[0]);
        return 2;
    }
    if (sh->source_depth >= MAX_SOURCE_DEPTH) {
        shell_error(sh, "%s: %s: sourced too deeply (a loop?)", argv[0], argv[1]);
        return 1;
    }
    path = find_sourced(sh, argv[1]);
    if (path == NULL) {
        shell_error(sh, "%s: %s: not found", argv[0], argv[1]);
        return 1;
    }
    r.fd = script_open(path);
    if (r.fd < 0) {
        shell_error(sh, "%s: %s: %s", argv[0], path, strerror(errno));
        free(path);
        return 1;
    }
    free(path);
    if (argc > 2) {
        sh->params = (struct strvec){ 0 };
        for (int i = 2; i < argc; i++)
            strvec_push(&sh->params, xstrdup(argv[i]));
    }
    sh->source_depth++;
    status = script_run(sh, &r);
    sh->source_depth--;
    if (argc > 2) {
        strvec_free(&sh->params);
        sh->params = saved;
    }
    close(r.fd);
    return status;
}

/* ---- set and explain ---------------------------------------------------- */

/* set with nothing: every variable, NAME='value', sorted, as dash prints
 * them. */
static void print_variables(const struct shell *sh)
{
    size_t n;
    const struct var **list = vars_sorted(&sh->vars, &n);

    for (size_t i = 0; i < n; i++) {
        if (list[i]->value == NULL || !var_name_valid(list[i]->name))
            continue;
        printf("%s=", list[i]->name);
        print_quoted(list[i]->value);
        putchar('\n');
    }
    free(list);
}

/* set -o explain / set +o explain. The line that runs set isn't explained
 * after all; the ones after it are. */
static int set_option(struct shell *sh, bool on, const char *name)
{
    if (name == NULL) {
        printf("explain         %s\n", sh->explain_all ? "on" : "off");
        return 0;
    }
    if (strcmp(name, "explain") != 0) {
        shell_error(sh, "set: %s: no such option (acsh has explain)", name);
        return 2;
    }
    if (on && !explain_start(sh))
        return 1;
    sh->explain_all = on;
    return 0;
}

/*
 * set                 list the variables
 * set -o | +o         show the options
 * set -o explain      explain every line from now on; +o turns it off
 * set [--] arg ...    make the args $1, $2, ...
 */
static int builtin_set(struct shell *sh, int argc, char **argv)
{
    int i = 1;

    if (argc == 1) {
        print_variables(sh);
        return 0;
    }
    if (strcmp(argv[1], "-o") == 0 || strcmp(argv[1], "+o") == 0) {
        if (argc > 3) {
            shell_error(sh, "set: too many arguments");
            return 2;
        }
        return set_option(sh, argv[1][0] == '-', argc == 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "--") == 0) {
        i = 2;
    } else if ((argv[1][0] == '-' || argv[1][0] == '+') && argv[1][1] != '\0') {
        shell_error(sh, "set: %s isn't supported yet (only -o explain)", argv[1]);
        return 2;
    }
    strvec_free(&sh->params);
    for (; i < argc; i++)
        strvec_push(&sh->params, xstrdup(argv[i]));
    return 0;
}

/* explain only means something at the start of a line, where the parser
 * takes it; anywhere else this runs instead. */
static int builtin_explain(struct shell *sh, int argc, char **argv)
{
    (void)sh;
    (void)argc;
    (void)argv;
    shell_error(sh, "explain: put it at the start of a line, before the command");
    return 2;
}

/* ---- help --------------------------------------------------------------- */

static int builtin_help(struct shell *sh, int argc, char **argv);

static const struct builtin builtins[] = {
    { ".",      builtin_source, ". file [arg ...]",
      "Run the commands in file in this shell. The same as source.", true },
    { ":",      builtin_true,   ": [arg ...]",
      "Do nothing, successfully.", true },
    { "break",  builtin_break,  "break [n]",
      "Leave the loop, or n loops.", true },
    { "cd",     builtin_cd,     "cd [dir | -]",
      "Go to dir. With no dir, go home; with -, go back to the last folder.", false },
    { "continue", builtin_continue, "continue [n]",
      "Go on with the next round of the loop, or of the nth loop out.", true },
    { "echo",   builtin_echo,   "echo [-neE] [word ...]",
      "Print the words. -n: no newline at the end. -e: read \\n, \\t and other escapes.", false },
    { "exit",   builtin_exit,   "exit [n]",
      "Leave acsh with status n, or with the last command's status.", true },
    { "explain", builtin_explain, "explain command ...",
      "Show the plan and every fork, pipe, dup2 and exec while it runs.", false },
    { "export", builtin_export, "export [name[=value] ...]",
      "Pass variables on to commands. With no names, list the ones passed on.", true },
    { "false",  builtin_false,  "false",
      "Fail: give status 1.", false },
    { "help",   builtin_help,   "help [name]",
      "List the built-ins, or show one.", false },
    { "pwd",    builtin_pwd,    "pwd",
      "Print the current folder.", false },
    { "read",   builtin_read,   "read [-r] [name ...]",
      "Read a line from stdin into the names, split on IFS. -r: keep backslashes.", false },
    { "set",    builtin_set,    "set [-o|+o explain] [arg]",
      "-o explain: explain every line, +o: stop. With args: set $1, $2, ...", true },
    { "shift",  builtin_shift,  "shift [n]",
      "Drop the first n arguments ($1 ...), 1 if n isn't given.", true },
    { "source", builtin_source, "source file [arg ...]",
      "Run the commands in file in this shell, with args as $1 ...", true },
    { "true",   builtin_true,   "true",
      "Succeed: give status 0.", false },
    { "unset",  builtin_unset,  "unset name ...",
      "Remove the variables.", true },
};

#define NBUILTINS (sizeof builtins / sizeof builtins[0])

/* help name: more than the one line, for each built-in. */
static const struct {
    const char *name;
    const char *text;
} details[] = {
    { ".",
      "Reads file and runs its commands in this shell, so its variables and cd\n"
      "stay. Without a /, file is looked for in PATH, then in this folder. The\n"
      "status is the last command's, and an exit in the file leaves the shell.\n" },
    { ":",
      "Ignores its arguments and gives status 0. Handy in `while :` and as a\n"
      "placeholder.\n" },
    { "break",
      "Ends the loop it's in. break 2 also ends the loop around that one.\n"
      "Outside a loop it does nothing.\n" },
    { "cd",
      "cd dir    go to dir\n"
      "cd        go to $HOME\n"
      "cd -      go back to $OLDPWD, and print where that is\n"
      "Sets PWD and OLDPWD for the commands after it.\n" },
    { "continue",
      "Skips the rest of this round of the loop and starts the next one.\n"
      "continue 2 does that for the loop around this one.\n" },
    { "echo",
      "Prints the words with a space between them and a newline at the end, like\n"
      "bash. -n leaves out the newline. -e reads escapes: \\n \\t \\\\ \\0nnn \\xHH\n"
      "and the others, and \\c stops printing. -E turns them off again.\n" },
    { "exit",
      "Leaves acsh with status n (0 to 255), or with the last command's status.\n"
      "With an argument that isn't a number it leaves with 2.\n" },
    { "explain",
      "Put it in front of a line: acsh shows the line as a tree, then every\n"
      "system call it makes for it with a note on what it does, then the status.\n"
      "`set -o explain` does that for every line.\n" },
    { "export",
      "export X        pass X on to the commands started from now on\n"
      "export X=value  set it and pass it on\n"
      "export          list the ones passed on, in a form the shell can read back\n" },
    { "false",
      "Gives status 1 and does nothing else.\n" },
    { "help",
      "help lists every built-in in one line; help name shows more about one.\n" },
    { "pwd",
      "Prints the full path of the current folder.\n" },
    { "read",
      "Reads one line from stdin. Each name gets one field, split on IFS, and\n"
      "the last name gets the rest of the line. Without a name the line goes into\n"
      "REPLY. A backslash keeps the next character as it is, and a backslash at\n"
      "the end joins the next line; -r keeps backslashes. Status 1 at the end of\n"
      "the input.\n" },
    { "set",
      "set               list the variables\n"
      "set -o explain    explain every line from now on; set +o explain stops\n"
      "set -o            show the options\n"
      "set -- a b        make a and b the arguments, $1 and $2\n" },
    { "shift",
      "Drops $1 and moves the rest down, so $2 becomes $1. shift n drops n.\n" },
    { "source",
      "Reads file and runs its commands in this shell, so its variables and cd\n"
      "stay. Arguments after file are $1, $2, ... while it runs. Without a /,\n"
      "file is looked for in PATH, then in this folder. The same as .\n" },
    { "true",
      "Gives status 0 and does nothing else.\n" },
    { "unset",
      "Removes the variables, from the shell and from what commands get.\n" },
};

static void print_help_line(const struct builtin *b)
{
    printf("  %-26s %s\n", b->usage, b->summary);
}

/* The usage line, then the details, each line indented. */
static void print_help_page(const struct builtin *b)
{
    printf("%s\n", b->usage);
    for (size_t i = 0; i < sizeof details / sizeof details[0]; i++) {
        const char *p = details[i].text;

        if (strcmp(details[i].name, b->name) != 0)
            continue;
        while (*p != '\0') {
            size_t n = strcspn(p, "\n");

            printf("    %.*s\n", (int)n, p);
            p += n + (p[n] == '\n');
        }
        return;
    }
    printf("    %s\n", b->summary); /* every built-in has details; this is just in case */
}

static int builtin_help(struct shell *sh, int argc, char **argv)
{
    int status = 0;

    (void)sh;
    if (argc == 1) {
        puts("acsh built-ins:");
        for (size_t i = 0; i < NBUILTINS; i++)
            print_help_line(&builtins[i]);
        puts("Anything else runs as a program from PATH. `help name` shows more about\n"
             "one, and `explain command` shows what acsh does to run a command.");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const struct builtin *b = builtin_find(argv[i]);

        if (b != NULL) {
            print_help_page(b);
        } else {
            shell_error(sh, "help: no help for '%s'", argv[i]);
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
        shell_error(sh, "%s: write error: %s", b->name, strerror(errno));
        clearerr(stdout);
        if (status == 0)
            status = 1;
    }
    return status;
}
