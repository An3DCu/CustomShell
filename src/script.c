#include "script.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "error.h"
#include "exec.h"
#include "explain.h"
#include "jobs.h"
#include "input.h"
#include "parser.h"
#include "prompt.h"
#include "signals.h"
#include "vars.h"
#include "xalloc.h"

/* The next line of the -c text, newline included, like input_read_line. */
static ssize_t read_text_line(struct reader *r, char **line, size_t *cap)
{
    const char *start = r->text + r->pos;
    const char *nl = strchr(start, '\n');
    size_t len = nl ? (size_t)(nl - start) + 1 : strlen(start);

    if (len == 0) {
        errno = 0;
        return -1;
    }
    if (len + 1 > *cap) {
        *cap = len + 1;
        *line = xreallocarray(*line, *cap, 1);
    }
    memcpy(*line, start, len);
    (*line)[len] = '\0';
    r->pos += len;
    return (ssize_t)len;
}

static ssize_t read_line(struct reader *r, char **line, size_t *cap)
{
    if (r->fd < 0)
        return read_text_line(r, line, cap);
    return input_read_line(r->fd, line, cap, r->interactive);
}

/* The text of a command that spans lines so far. */
struct pending {
    char *text;
    size_t len;
    size_t cap;
};

static void pending_add(struct pending *p, const char *line, size_t n)
{
    if (p->len + n + 1 > p->cap) {
        p->cap = (p->len + n + 1) * 2;
        p->text = xreallocarray(p->text, p->cap, 1);
    }
    memcpy(p->text + p->len, line, n + 1);
    p->len += n;
}

static void run_text(struct shell *sh, const struct list *list)
{
    bool was = sh->explaining;
    bool explain = list->explain || sh->explain_all || sh->explaining;

    sh->interrupted = false;
    sh->stopped = false;
    if (explain && explain_start(sh)) {
        sh->explaining = true;
        explain_plan(sh, list);
    }
    run_list(sh, list);
    explain_status(sh, sh->status);
    sh->explaining = was;
    /* The terminal shows ^C but no newline; start the prompt on a fresh
     * line, as bash does. */
    if (sh->interactive && sh->interrupted)
        fputc('\n', stderr);
}

int script_run(struct shell *sh, struct reader *r)
{
    struct pending cmd = { 0 };
    char *line = NULL;
    size_t cap = 0;
    char err[256] = "";
    bool ran = false;
    const char *saved_where = sh->where;
    int saved_line = sh->line;
    int saved_unit = sh->unit_line;
    int unit = 1; /* the line the command being read starts on */

    sh->where = r->name;
    for (;;) {
        struct list *list;
        enum parse_status st;
        ssize_t n;

        if (r->interactive) {
            if (cmd.len == 0) {
                /* Background jobs that ended get their notice now, just
                 * before the prompt, as in bash. */
                jobs_reap(sh);
                jobs_notify(sh);
                prompt_print(sh);
            } else {
                fputs("> ", stderr); /* the command isn't finished yet */
            }
        }
        if (cmd.len == 0)
            unit = r->lines + 1;
        n = read_line(r, &line, &cap);
        if (n >= 0)
            r->lines++;
        sh->line = r->lines; /* for errors found while reading */
        if (n < 0) {
            if (errno == EINTR) {
                /* Ctrl+C at the prompt: drop what was typed, start again.
                 * bash sets the status to 130 here too. */
                fputc('\n', stderr);
                cmd.len = 0;
                sh->status = 130;
                ran = true;
                continue;
            }
            if (errno != 0) {
                shell_error(sh, "reading input: %s", strerror(errno));
            } else if (cmd.len > 0) {
                /* The input ended in the middle of a command. */
                shell_error(sh, "%s", err);
                sh->status = 2;
                ran = true;
            } else if (r->interactive) {
                fputc('\n', stderr); /* Ctrl+D: end the prompt's line */
                sh->commands++;      /* it counts as an exit */
                if (!jobs_may_leave(sh)) {
                    sh->status = 1;
                    ran = true;
                    continue;
                }
            }
            break;
        }
        pending_add(&cmd, line, (size_t)n);
        st = parse_program(cmd.text, &list, err, sizeof err);
        if (st == PARSE_INCOMPLETE)
            continue;
        cmd.len = 0;
        if (st == PARSE_EMPTY)
            continue; /* an empty line keeps the last status, as in bash */
        ran = true;
        if (st == PARSE_SYNTAX) {
            shell_error(sh, "%s", err);
            if (strcmp(err, "syntax error near '|'") == 0)
                shell_hint(sh, "a | needs a command on both sides");
            sh->status = 2;
            if (!r->interactive)
                break;
            continue;
        }
        sh->unit_line = unit;
        jobs_reap(sh); /* in a script too, so finished jobs don't stay zombies */
        run_text(sh, list);
        list_free(list);
        if (sh->exiting)
            break;
        if (r->interactive)
            signals_drop_pending();
    }
    free(cmd.text);
    free(line);
    sh->where = saved_where;
    sh->line = saved_line;
    sh->unit_line = saved_unit;
    return ran ? sh->status : 0;
}

int script_open(const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int high;

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return -1;
    }
    high = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    close(fd);
    return high; /* -1 with errno set if even that failed */
}
