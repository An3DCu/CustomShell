#include "explain.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "error.h"
#include "vars.h"
#include "xalloc.h"

/* Where the explanation goes is a copy of stderr at fd 10 or above. 0
 * means explain mode hasn't started. The copy is only made when it's first
 * needed. A shell that never explains has only 0, 1 and 2 open. */
bool explain_start(struct shell *sh)
{
    int fd;

    if (sh->explain_fd > 0)
        return true;
    fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 10);
    if (fd < 0) {
        shell_error(sh, "explain: %s", high_fd_error(errno));
        return false;
    }
    sh->explain_fd = fd;
    sh->explain_out = fd;
    return true;
}

bool explain_on(const struct shell *sh)
{
    return sh->explaining && sh->explain_out > 0;
}

/* Everything goes out with dprintf, straight to the fd. There's no stdio
 * buffer that a fork could copy. */
static void out(const struct shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void out(const struct shell *sh, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vdprintf(sh->explain_out, fmt, ap);
    va_end(ap);
}

const char *explain_fd_name(int fd, char *buf, size_t size)
{
    switch (fd) {
    case 0:  return "stdin";
    case 1:  return "stdout";
    case 2:  return "stderr";
    default:
        snprintf(buf, size, "fd %d", fd);
        return buf;
    }
}

void explain_event(struct shell *sh, const char *note, const char *fmt, ...)
{
    char call[512];
    char line[560];
    va_list ap;

    if (!explain_on(sh))
        return;
    va_start(ap, fmt);
    vsnprintf(call, sizeof call, fmt, ap);
    va_end(ap);
    if (getpid() != sh->pid) /* in a child: say which one */
        snprintf(line, sizeof line, "[%ld] %s", (long)getpid(), call);
    else
        snprintf(line, sizeof line, "%s", call);
    /* The notes line up in one column, unless the call is very long. */
    if (note != NULL && note[0] != '\0')
        out(sh, "  %-40s %s\n", line, note);
    else
        out(sh, "  %s\n", line);
}

void explain_status(struct shell *sh, int status)
{
    if (explain_on(sh))
        out(sh, "status %d\n", status);
}

/* ---- the plan ------------------------------------------------------------- */

/* A line of the tree. indent is what goes before the branch. It holds
 * spaces, with a │ for every level whose list goes on below. */
struct tree {
    struct shell *sh;
    char indent[256];
};

static void node(struct tree *t, bool root, bool last, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void node(struct tree *t, bool root, bool last, const char *fmt, ...)
{
    char text[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    out(t->sh, "  %s%s%s\n", t->indent, root ? "" : (last ? "└─ " : "├─ "), text);
}

/* Goes one level down under a node. Returns what to put back. */
static size_t descend(struct tree *t, bool root, bool last)
{
    size_t len = strlen(t->indent);

    if (!root)
        snprintf(t->indent + len, sizeof t->indent - len, "%s", last ? "   " : "│  ");
    return len;
}

static void ascend(struct tree *t, size_t len)
{
    t->indent[len] = '\0';
}

static const char *redir_op(enum redir_kind kind)
{
    switch (kind) {
    case REDIR_IN:      return "<";
    case REDIR_OUT:     return ">";
    case REDIR_APPEND:  return ">>";
    case REDIR_DUP_IN:  return "<&";
    case REDIR_DUP_OUT: return ">&";
    }
    return "?";
}

/* "  stdout > n.txt  stderr >& 1" after a command */
static void add_redirs(char *buf, size_t size, const struct command *cmd)
{
    size_t used = strlen(buf);

    for (size_t i = 0; i < cmd->nredirs && used < size; i++) {
        const struct redirection *r = &cmd->redirs[i];
        char name[16];

        used += (size_t)snprintf(buf + used, size - used, "  %s %s %s",
                                 explain_fd_name(r->fd, name, sizeof name),
                                 redir_op(r->kind), r->target);
    }
}

/* A simple command as typed: assignments, words, then its redirections. */
static void simple_text(char *buf, size_t size, const struct command *cmd)
{
    size_t used = 0;

    buf[0] = '\0';
    for (size_t i = 0; i < cmd->nassigns && used < size; i++)
        used += (size_t)snprintf(buf + used, size - used, "%s%s", used ? " " : "", cmd->assigns[i]);
    for (size_t i = 0; i < cmd->nwords && used < size; i++)
        used += (size_t)snprintf(buf + used, size - used, "%s%s", used ? " " : "", cmd->words[i]);
    if (used == 0)
        snprintf(buf, size, "(no command)");
    add_redirs(buf, size, cmd);
}

static void plan_list(struct tree *t, const struct list *list, bool root, bool last);

/* "condition", then the list under it */
static void plan_part(struct tree *t, const char *label, const struct list *list, bool last)
{
    size_t len;

    node(t, false, last, "%s", label);
    len = descend(t, false, last);
    plan_list(t, list, false, true);
    ascend(t, len);
}

static void plan_command(struct tree *t, const struct command *cmd, bool root, bool last,
                         const char *lead)
{
    char text[1024];
    size_t len;

    if (cmd->kind == CMD_SIMPLE) {
        simple_text(text, sizeof text, cmd);
        node(t, root, last, "%s%s", lead, text);
        return;
    }
    switch (cmd->kind) {
    case CMD_IF:
        snprintf(text, sizeof text, "if");
        break;
    case CMD_WHILE:
        snprintf(text, sizeof text, "while");
        break;
    case CMD_UNTIL:
        snprintf(text, sizeof text, "until");
        break;
    case CMD_FOR: {
        size_t used = (size_t)snprintf(text, sizeof text, "for %s", cmd->var);

        if (cmd->has_in) {
            used += (size_t)snprintf(text + used, sizeof text - used, " in");
            for (size_t i = 0; i < cmd->nwords && used < sizeof text; i++)
                used += (size_t)snprintf(text + used, sizeof text - used, " %s", cmd->words[i]);
        } else {
            snprintf(text + used, sizeof text - used, " in \"$@\"");
        }
        break;
    }
    case CMD_SIMPLE:
        break;
    }
    add_redirs(text, sizeof text, cmd);
    node(t, root, last, "%s%s", lead, text);
    len = descend(t, root, last);
    switch (cmd->kind) {
    case CMD_IF:
        for (size_t i = 0; i < cmd->nbranches; i++) {
            bool final = i + 1 == cmd->nbranches && cmd->body == NULL;

            plan_part(t, i == 0 ? "condition" : "elif condition", cmd->branches[i].cond, false);
            plan_part(t, "then", cmd->branches[i].body, final);
        }
        if (cmd->body != NULL)
            plan_part(t, "else", cmd->body, true);
        break;
    case CMD_WHILE:
    case CMD_UNTIL:
        plan_part(t, "condition", cmd->cond, false);
        plan_part(t, "do", cmd->body, true);
        break;
    case CMD_FOR:
        plan_part(t, "do", cmd->body, true);
        break;
    case CMD_SIMPLE:
        break;
    }
    ascend(t, len);
}

static void plan_pipeline(struct tree *t, const struct pipeline *pl, bool root, bool last,
                          const char *lead)
{
    char head[128];
    size_t len;

    if (pl->ncommands == 1 && !pl->negate) {
        plan_command(t, &pl->commands[0], root, last, lead);
        return;
    }
    if (pl->ncommands == 1)
        snprintf(head, sizeof head, "%s! (0 becomes 1, anything else 0)", lead);
    else
        snprintf(head, sizeof head, "%s%spipeline of %zu", lead, pl->negate ? "! " : "",
                 pl->ncommands);
    node(t, root, last, "%s", head);
    len = descend(t, root, last);
    for (size_t i = 0; i < pl->ncommands; i++)
        plan_command(t, &pl->commands[i], false, i + 1 == pl->ncommands, "");
    ascend(t, len);
}

static void plan_and_or(struct tree *t, const struct and_or *ao, bool root, bool last)
{
    const char *bg = ao->background ? "in the background (&): " : "";
    size_t len;

    if (ao->npipelines == 1) {
        plan_pipeline(t, &ao->pipelines[0], root, last, bg);
        return;
    }
    node(t, root, last, "%sand-or of %zu", bg, ao->npipelines);
    len = descend(t, root, last);
    for (size_t i = 0; i < ao->npipelines; i++) {
        const struct pipeline *pl = &ao->pipelines[i];
        const char *lead = pl->run_if == RUN_IF_OK       ? "&& if that worked: "
                         : pl->run_if == RUN_IF_FAILED   ? "|| if that failed: "
                                                         : "";

        plan_pipeline(t, pl, false, i + 1 == ao->npipelines, lead);
    }
    ascend(t, len);
}

static void plan_list(struct tree *t, const struct list *list, bool root, bool last)
{
    size_t len;

    if (list->nitems == 1) {
        plan_and_or(t, &list->items[0], root, last);
        return;
    }
    node(t, root, last, "list of %zu, one after the other", list->nitems);
    len = descend(t, root, last);
    for (size_t i = 0; i < list->nitems; i++)
        plan_and_or(t, &list->items[i], false, i + 1 == list->nitems);
    ascend(t, len);
}

void explain_plan(struct shell *sh, const struct list *list)
{
    struct tree t = { .sh = sh };

    if (!explain_on(sh))
        return;
    out(sh, "plan\n");
    plan_list(&t, list, true, true);
    out(sh, "run\n");
}

/* ---- words and exec -------------------------------------------------------- */

/* Words joined by spaces. One that has a blank, or is empty, goes in single
 * quotes. The fields can then be told apart. */
static void join_words(char *buf, size_t size, char *const *words, size_t n, bool quote)
{
    size_t used = 0;

    buf[0] = '\0';
    for (size_t i = 0; i < n && used < size; i++) {
        bool q = quote && (words[i][0] == '\0' || strpbrk(words[i], " \t\n") != NULL);

        used += (size_t)snprintf(buf + used, size - used, "%s%s%s%s", i ? " " : "",
                                 q ? "'" : "", words[i], q ? "'" : "");
    }
}

void explain_words(struct shell *sh, char *const *typed, size_t ntyped, char *const *argv)
{
    char before[512], after[512];
    size_t n = 0;

    if (!explain_on(sh))
        return;
    while (argv[n] != NULL)
        n++;
    join_words(before, sizeof before, typed, ntyped, false);
    join_words(after, sizeof after, argv, n, true);
    if (strcmp(before, after) != 0)
        explain_event(sh, NULL, "expand: %s  ->  %s", before, after[0] ? after : "(nothing)");
}

/* Where execvp will find name, the first executable file in PATH. It's
 * only shown. execvp does its own search. */
static bool find_in_path(const struct shell *sh, const char *name, char *buf, size_t size)
{
    const char *path = vars_get(&sh->vars, "PATH");

    while (path != NULL) {
        const char *colon = strchr(path, ':');
        size_t dlen = colon ? (size_t)(colon - path) : strlen(path);
        struct stat st;

        if (dlen == 0)
            snprintf(buf, size, "./%s", name);
        else
            snprintf(buf, size, "%.*s/%s", (int)dlen, path, name);
        if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0)
            return true;
        path = colon ? colon + 1 : NULL;
    }
    return false;
}

void explain_exec(struct shell *sh, char *const *argv)
{
    char args[512], where[512], note[600];
    size_t n = 0;

    if (!explain_on(sh) || argv[0] == NULL)
        return;
    while (argv[n] != NULL)
        n++;
    join_words(args, sizeof args, argv, n, true);
    if (strchr(argv[0], '/') != NULL)
        snprintf(note, sizeof note, "becomes %s", argv[0]);
    else if (find_in_path(sh, argv[0], where, sizeof where))
        snprintf(note, sizeof note, "becomes %s, found in PATH", where);
    else
        snprintf(note, sizeof note, "not in PATH");
    explain_event(sh, note, "execvp(%s)", args);
}
