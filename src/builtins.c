#include "builtins.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "error.h"
#include "explain.h"
#include "input.h"
#include "history.h"
#include "jobs.h"
#include "script.h"
#include "signames.h"
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
    /* dir may point into the variable table. vars_set changes that table.
     * dir isn't used after this. */
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

/* As in bash, an argument is an option only if it's "-" followed by nothing
 * but n, e and E. -nE is an option. -x, - and -- are printed. */
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

/* Prints s with echo -e escapes. Returns false at \c. That means "stop
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
    /* strtol also skips leading blanks. A number here starts right away. */
    return s[0] == '-' || s[0] == '+' || (s[0] >= '0' && s[0] <= '9');
}

static int builtin_exit(struct shell *sh, int argc, char **argv)
{
    int status = sh->status;
    long n;

    if (argc > 2) {
        shell_error(sh, "exit: too many arguments");
        return 1; /* an error. The shell stays, as in bash. */
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
    if (!jobs_may_leave(sh))
        return 1; /* it warned about stopped jobs. The shell stays. */
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

/* break [n] and continue [n] only set a count. The loops act on it (see
 * loop_ends in exec.c). n bigger than the number of loops means all of
 * them. Outside a loop there's nothing to do, as in dash. */
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
 * Reads one line from stdin into b, one byte at a time like the shell. The
 * rest stays for the next command. Without -r, a backslash keeps the next
 * byte from separating fields. A backslash at the end of the line joins the
 * next one. Returns 0 for a full line, 1 at the end of input
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

        input_warn_zero(sh);
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
 * Splits b over the names, POSIX style. Each name but the last gets one
 * field. The last gets the rest of the line, without IFS spaces at either
 * end. If that rest is a single field plus one separator, the separator
 * goes too. With IFS=:, "a:b:" gives y=b but "a:b::" gives b::.
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
 * POSIX says. After that it's looked for in the current folder, as bash
 * does. That's what makes `source setup.sh` work. Returns a new string, or
 * NULL. */
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
 * source file [arg ...] runs the file's commands in this shell. Its
 * variables and cd stay. With arguments, they are $1, $2, ... while it
 * runs. The old ones come back after. Its status is the last command's.
 * An exit in the file leaves the shell, as POSIX says.
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
        shell_error(sh, "%s: %s: %s", argv[0], path, high_fd_error(errno));
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

/* ---- history ------------------------------------------------------------ */

/* history, history n, history -c */
static int builtin_history(struct shell *sh, int argc, char **argv)
{
    struct history *h = &sh->history;
    size_t first = 0;
    long n;

    if (argc > 2) {
        shell_error(sh, "history: takes one argument at most");
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        history_clear(h);
        return 0;
    }
    if (argc == 2) {
        if (!parse_number(argv[1], &n) || n < 0) {
            shell_error(sh, "history: %s: not a number", argv[1]);
            return 2;
        }
        if ((size_t)n < h->len)
            first = h->len - (size_t)n;
    }
    for (size_t i = first; i < h->len; i++)
        printf("%5zu  %s\n", i + 1, h->lines[i]);
    return 0;
}

/* ---- jobs --------------------------------------------------------------- */

static int builtin_jobs(struct shell *sh, int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        shell_error(sh, "jobs: takes no arguments");
        return 2;
    }
    jobs_list(sh);
    return 0;
}

/* ---- fg and bg ---------------------------------------------------------- */

/* The job spec names, or the current one with spec NULL. NULL after an
 * error. It prints the error. */
static struct job *job_for(struct shell *sh, const char *name, const char *spec)
{
    char err[128];
    struct job *job;

    jobs_reap(sh);
    job = jobs_find(sh, spec != NULL ? spec : "%+", err, sizeof err);
    if (job == NULL) {
        if (spec == NULL)
            shell_error(sh, "%s: no current job", name);
        else
            shell_error(sh, "%s: %s", name, err);
    }
    return job;
}

static bool have_job_control(struct shell *sh, const char *name)
{
    if (!sh->job_control)
        shell_error(sh, "%s: no job control in this shell", name);
    return sh->job_control;
}

static int builtin_fg(struct shell *sh, int argc, char **argv)
{
    struct job *job;

    if (!have_job_control(sh, "fg"))
        return 1;
    /* only one can be in front. The rest are ignored, as in bash and dash. */
    job = job_for(sh, "fg", argc >= 2 ? argv[1] : NULL);
    return job != NULL ? jobs_fg(sh, job) : 1;
}

static int builtin_bg(struct shell *sh, int argc, char **argv)
{
    int status = 0;

    if (!have_job_control(sh, "bg"))
        return 1;
    for (int i = 1; i < argc || i == 1; i++) {
        struct job *job = job_for(sh, "bg", i < argc ? argv[i] : NULL);

        status = job != NULL ? jobs_bg(sh, job) : 1;
    }
    return status;
}

/* ---- disown ------------------------------------------------------------- */

static void disown_one(struct shell *sh, struct job *job)
{
    /* nothing in the shell can continue it once it's off the list */
    if (job->state == JOB_STOPPED)
        shell_error(sh, "disown: job %d is stopped; it stays stopped", job->id);
    jobs_disown(sh, job);
}

/* disown [%n ...] or disown -a. No argument: the current job. */
static int builtin_disown(struct shell *sh, int argc, char **argv)
{
    int status = 0;

    if (argc == 2 && strcmp(argv[1], "-a") == 0) {
        jobs_reap(sh);
        while (sh->njobs > 0)
            disown_one(sh, &sh->jobs[0]);
        return 0;
    }
    for (int i = 1; i < argc || i == 1; i++) {
        struct job *job = job_for(sh, "disown", i < argc ? argv[i] : NULL);

        if (job == NULL)
            status = 1;
        else
            disown_one(sh, job);
    }
    return status;
}

/* ---- kill --------------------------------------------------------------- */

/* The signal text names (TERM, SIGTERM, term, 15), or -1 after saying it
 * has none. A number has to be one the system has; 0 sends nothing and
 * only checks that the target is there. */
static int signal_arg(struct shell *sh, const char *text)
{
    int sig = signame_parse(text);
    struct sigaction sa;

    if (sig < 0 || (sig != 0 && sigaction(sig, NULL, &sa) != 0)) {
        shell_error(sh, "kill: %s: no such signal", text);
        return -1;
    }
    return sig;
}

/* kill -l: number, name and meaning, one per line. With arguments, the
 * name for a number (an exit status above 128 counts as its signal) or
 * the number for a name. */
static int list_signals(struct shell *sh, int argc, char **argv)
{
    int status = 0;

    if (argc == 0) {
        for (int sig = 1; sig <= signame_max(); sig++) {
            const char *name = signame_of(sig);

            if (name != NULL)
                printf("%2d  %-6s  %s\n", sig, name, strsignal(sig));
        }
        return 0;
    }
    for (int i = 0; i < argc; i++) {
        int sig = signame_parse(argv[i]);
        bool number = argv[i][0] >= '0' && argv[i][0] <= '9';
        const char *name;

        if (number && sig > 128)
            sig -= 128;
        name = sig > 0 ? signame_of(sig) : NULL;
        if (name == NULL) {
            shell_error(sh, "kill: %s: no such signal", argv[i]);
            status = 1;
        } else if (number) {
            printf("%s\n", name);
        } else {
            printf("%d\n", sig);
        }
    }
    return status;
}

/* Sends sig to pid, a whole group when it's negative. TERM or HUP to a
 * stopped target is followed by SIGCONT, as bash does. A stopped process
 * only acts on them once it runs again. */
static bool send_signal(struct shell *sh, const char *target, pid_t pid, int sig, bool stopped)
{
    const char *name = signame_of(sig);
    const char *note = pid < 0 ? "the signal goes to the whole group" : NULL;

    if (name != NULL)
        explain_event(sh, note, "kill(%ld, SIG%s)", (long)pid, name);
    else
        explain_event(sh, note, "kill(%ld, %d)", (long)pid, sig);
    if (kill(pid, sig) != 0) {
        if (errno == ESRCH)
            shell_error(sh, "kill: %s: no such process", target);
        else
            shell_error(sh, "kill: %s: %s", target, strerror(errno));
        return false;
    }
    if (stopped && (sig == SIGTERM || sig == SIGHUP)) {
        explain_event(sh, "it's stopped: wake it so it can end", "kill(%ld, SIGCONT)", (long)pid);
        kill(pid, SIGCONT);
    }
    return true;
}

/* One target: %n for a job, or a pid (negative for a group). */
static bool kill_target(struct shell *sh, const char *target, int sig)
{
    char err[128];
    struct job *job;
    long n;

    if (target[0] == '%') {
        job = jobs_find(sh, target, err, sizeof err);
        if (job == NULL) {
            shell_error(sh, "kill: %s", err);
            return false;
        }
        if (job->pgid > 0)
            return send_signal(sh, target, -job->pgid, sig, job->state == JOB_STOPPED);
        /* without job control a job has no group of its own */
        for (size_t k = 0; k < job->npids; k++) {
            if (!job->ended[k] && !send_signal(sh, target, job->pids[k], sig, false))
                return false;
        }
        return true;
    }
    if (!parse_number(target, &n) || n != (pid_t)n) {
        shell_error(sh, "kill: %s: not a pid or a job", target);
        return false;
    }
    job = jobs_find_pid(sh, (pid_t)n);
    return send_signal(sh, target, (pid_t)n, sig, job != NULL && job->state == JOB_STOPPED);
}

/*
 * kill [-SIGNAL | -s SIGNAL] [--] %n | pid ...   TERM if no signal is given
 * kill -l [n | name ...]
 * Every target is tried. The status is 1 if any of them failed.
 */
static int builtin_kill(struct shell *sh, int argc, char **argv)
{
    int sig = SIGTERM;
    int status = 0;
    int i = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0)
        return list_signals(sh, argc - 2, argv + 2);
    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        sig = signal_arg(sh, argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0' && strcmp(argv[1], "-s") != 0
               && strcmp(argv[1], "--") != 0) {
        sig = signal_arg(sh, argv[1] + 1);
        i = 2;
    }
    if (sig < 0)
        return 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc || (i == 1 && strcmp(argv[1], "-s") == 0)) { /* -s with no signal */
        shell_error(sh, "kill: usage: kill [-SIGNAL] %%n | pid ...");
        return 2;
    }
    jobs_reap(sh);
    for (; i < argc; i++) {
        if (!kill_target(sh, argv[i], sig))
            status = 1;
    }
    return status;
}

/* ---- wait --------------------------------------------------------------- */

/* A pid as a whole positive number, or 0. */
static pid_t parse_pid(const char *s)
{
    long n;

    if (!parse_number(s, &n) || n <= 0)
        return 0;
    return (pid_t)n;
}

/*
 * wait            every job, then status 0
 * wait %n | pid   that one, and its status; 127 if it isn't ours
 * Several at once give the status of the last one.
 */
static int builtin_wait(struct shell *sh, int argc, char **argv)
{
    int status = 0;

    jobs_reap(sh);
    if (argc == 1)
        return jobs_wait_all(sh);
    for (int i = 1; i < argc; i++) {
        char err[128];
        struct job *job;

        if (argv[i][0] == '%') {
            job = jobs_find(sh, argv[i], err, sizeof err);
            if (job == NULL) {
                shell_error(sh, "wait: %s", err);
                status = 127;
                continue;
            }
            status = jobs_wait(sh, job);
        } else {
            pid_t pid = parse_pid(argv[i]);

            if (pid == 0) {
                shell_error(sh, "wait: %s: not a pid or a job", argv[i]);
                status = 2;
                continue;
            }
            job = jobs_find_pid(sh, pid);
            if (job == NULL) {
                shell_error(sh, "wait: pid %ld isn't a child of this shell", (long)pid);
                status = 127;
                continue;
            }
            status = jobs_wait_pid(sh, job, pid);
        }
        if (sh->interrupted)
            return 130;
    }
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
 * after all. The ones after it are. */
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
 * takes it. Anywhere else this runs instead. */
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
    { "bg",     builtin_bg,     "bg [%n ...]",
      "Let a stopped job go on in the background.", false },
    { "break",  builtin_break,  "break [n]",
      "Leave the loop, or n loops.", true },
    { "cd",     builtin_cd,     "cd [dir | -]",
      "Go to dir. With no dir, go home; with -, go back to the last folder.", false },
    { "continue", builtin_continue, "continue [n]",
      "Go on with the next round of the loop, or of the nth loop out.", true },
    { "disown", builtin_disown, "disown [-a] [%n ...]",
      "Take jobs off the list: no notices, and no SIGHUP when acsh leaves.", false },
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
    { "fg",     builtin_fg,     "fg [%n]",
      "Bring a job back to the front: the current one, or job n.", false },
    { "help",   builtin_help,   "help [name]",
      "List the built-ins, or show one.", false },
    { "history", builtin_history, "history [n | -c]",
      "List the lines typed at the prompt, numbered. -c forgets them.", false },
    { "jobs",   builtin_jobs,   "jobs",
      "List the jobs: what runs in the background or is stopped.", false },
    { "kill",   builtin_kill,   "kill [-SIGNAL] %n | pid ...",
      "Send a signal, TERM if none is given, to jobs or processes. -l lists them.", false },
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
    { "wait",   builtin_wait,   "wait [%n | pid ...]",
      "Wait for jobs to end; give the status of the last one.", false },
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
    { "bg",
      "bg       let the current job go on in the background\n"
      "bg %n    the same for job n; several can be given\n"
      "It says `job 1 running: ...`. A job that's running already stays so.\n" },
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
    { "disown",
      "disown       take the current job off the list\n"
      "disown %n    the same for job n; several can be given\n"
      "disown -a    every job\n"
      "acsh forgets the job: no notice when it ends, and no SIGHUP when acsh\n"
      "leaves, so it keeps running. A stopped job stays stopped.\n" },
    { "echo",
      "Prints the words with a space between them and a newline at the end, like\n"
      "bash. -n leaves out the newline. -e reads escapes: \\n \\t \\\\ \\0nnn \\xHH\n"
      "and the others, and \\c stops printing. -E turns them off again.\n" },
    { "exit",
      "Leaves acsh with status n (0 to 255), or with the last command's status.\n"
      "With an argument that isn't a number it leaves with 2.\n"
      "With stopped jobs it names them and stays, with status 1. An exit right\n"
      "after that leaves, and every job gets SIGHUP. Ctrl+D works the same.\n" },
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
    { "fg",
      "fg       bring the current job to the front: the one stopped last, or else\n"
      "         the one last sent to the background\n"
      "fg %n    bring job n to the front (%- the job before the current one)\n"
      "It prints the command, gives it the terminal and its terminal modes back, and\n"
      "waits for it. Ctrl+Z stops it again.\n" },
    { "help",
      "help lists every built-in in one line; help name shows more about one.\n" },
    { "history",
      "history      every line typed at the prompt, numbered, oldest first\n"
      "history n    the last n of them\n"
      "history -c   forget them all, in the history file too\n"
      "The file is $HISTFILE, or ~/.acsh_history. It keeps $HISTSIZE lines, 1000\n"
      "if that isn't set. Up and Down go through them, and Ctrl+R searches them.\n" },
    { "jobs",
      "One line for each job: its number, running, stopped or done, and the\n"
      "command. A job that's done is shown once more, then forgotten.\n" },
    { "kill",
      "kill %n          send TERM to job n, every process in it\n"
      "kill -9 pid      send signal 9, KILL, to one process\n"
      "kill -s HUP %n   the same with -s; -HUP, -SIGHUP and -hup work too\n"
      "kill -- -pgid    a whole process group; 0 is the shell's own, -1 every\n"
      "                 process you may signal\n"
      "kill -l          list the signals; kill -l 143 names the one behind status 143\n"
      "TERM or HUP to a stopped job also sends CONT, so it wakes up and ends.\n" },
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
    { "wait",
      "wait         wait for every job, stopped ones too, then give status 0\n"
      "wait %n      wait for job n and give its status (%% or %+ the current job, %- the one before)\n"
      "wait pid     the same for one process, like $!\n"
      "In a terminal, Ctrl+C stops the waiting and the jobs it waits for (status 130).\n" },
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
    printf("    %s\n", b->summary); /* every built-in has details. This is a fallback. */
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

const char *builtin_name(size_t i)
{
    return i < NBUILTINS ? builtins[i].name : NULL;
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
