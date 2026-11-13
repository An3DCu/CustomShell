#include "redirect.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "analyzer.h"
#include "error.h"
#include "expand.h"
#include "explain.h"
#include "xalloc.h"

/* The shell's copies live from here up, away from the 0 to 9 people use. */
#define BACKUP_MIN_FD 10

static void push_backup(struct redir_undo *undo, int fd, int copy)
{
    if (undo->len == undo->cap) {
        undo->cap = undo->cap ? undo->cap * 2 : 4;
        undo->items = xreallocarray(undo->items, undo->cap, sizeof *undo->items);
    }
    undo->items[undo->len++] = (struct fd_backup){ .fd = fd, .copy = copy };
}

/* The redirection as typed, like "> f" or "2>> log", for errors. */
static void redir_text(const struct redirection *r, char *out, size_t size)
{
    static const char *const ops[] = { "<", ">", ">>", "<&", ">&" };
    int usual = r->kind == REDIR_IN || r->kind == REDIR_DUP_IN ? 0 : 1;

    if (r->fd == usual)
        snprintf(out, size, "%s %s", ops[r->kind], r->target);
    else
        snprintf(out, size, "%d%s %s", r->fd, ops[r->kind], r->target);
}

/* Keeps a copy of r's fd before its first change. */
static bool backup(struct shell *sh, struct redir_undo *undo, const struct redirection *r)
{
    int fd = r->fd;
    char what[256];
    char name[16];

    int copy;

    for (size_t i = 0; i < undo->len; i++) {
        if (undo->items[i].fd == fd)
            return true; /* saved already. The first original is the one to restore. */
    }
    /* `10>f` would overwrite one of my own copies. Move it out of the way
     * first. fd 10 then counts as "wasn't open" for the user. */
    for (size_t i = 0; i < undo->len; i++) {
        if (undo->items[i].copy == fd) {
            int moved = fcntl(fd, F_DUPFD_CLOEXEC, BACKUP_MIN_FD);

            if (moved < 0) {
                redir_text(r, what, sizeof what);
                shell_error(sh, "%s: can't save fd %d: %s", what, fd, high_fd_error(errno));
                return false;
            }
            undo->items[i].copy = moved;
            explain_event(sh, "the shell's copy moves out of the way",
                          "fcntl(%d, F_DUPFD_CLOEXEC, 10) = %d", fd, moved);
            push_backup(undo, fd, -1);
            return true;
        }
    }
    copy = fcntl(fd, F_DUPFD_CLOEXEC, BACKUP_MIN_FD);
    if (copy < 0 && errno != EBADF) {
        redir_text(r, what, sizeof what);
        shell_error(sh, "%s: can't save fd %d: %s", what, fd, high_fd_error(errno));
        return false;
    }
    if (copy >= 0) {
        char note[64];

        snprintf(note, sizeof note, "keep %s, to put it back after",
                 explain_fd_name(fd, name, sizeof name));
        explain_event(sh, note, "fcntl(%d, F_DUPFD_CLOEXEC, 10) = %d", fd, copy);
    }
    push_backup(undo, fd, copy); /* copy is -1 if fd wasn't open (EBADF) */
    return true;
}

static bool is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

/* The fd number in a dup target like the 1 in 2>&1, or -1 if it isn't one. */
static int parse_fd(const char *s)
{
    int fd = 0;

    if (s[0] == '\0')
        return -1;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        if (fd > (INT_MAX - (*p - '0')) / 10)
            return INT_MAX; /* far too big: dup2 will say so */
        fd = fd * 10 + (*p - '0');
    }
    return fd;
}

/* The fds that dup2 sets up in apply_dup and apply_file stay open for the
 * command on purpose (analyzer.h). */
FD_LEAK_IS_FINE_BEGIN

/* 2>&1, <&3, >&- */
static bool apply_dup(struct shell *sh, const struct redirection *r, const char *target)
{
    char a[16], b[16], note[64];
    int src;

    if (strcmp(target, "-") == 0) {
        snprintf(note, sizeof note, "%s is closed", explain_fd_name(r->fd, a, sizeof a));
        explain_event(sh, note, "close(%d)", r->fd);
        close(r->fd); /* closing an fd that isn't open is fine */
        return true;
    }
    src = parse_fd(target);
    if (src < 0) {
        shell_error(sh, "%s: bad file descriptor", target);
        return false;
    }
    if (!is_open(src)) {
        shell_error(sh, "%d: %s", src, strerror(EBADF));
        return false;
    }
    snprintf(note, sizeof note, "%s goes where %s goes", explain_fd_name(r->fd, a, sizeof a),
             explain_fd_name(src, b, sizeof b));
    explain_event(sh, note, "dup2(%d, %d)", src, r->fd);
    if (src != r->fd && dup2(src, r->fd) < 0) {
        shell_error(sh, "%d: %s", r->fd, strerror(errno));
        return false;
    }
    return true;
}

/* <, > and >> */
static bool apply_file(struct shell *sh, const struct redirection *r, const char *target)
{
    const char *how, *flag_names;
    char name[16], note[600];
    int flags;
    int fd;

    switch (r->kind) {
    case REDIR_IN:
        flags = O_RDONLY;
        flag_names = "O_RDONLY";
        how = "comes from";
        break;
    case REDIR_APPEND:
        flags = O_WRONLY | O_CREAT | O_APPEND;
        flag_names = "O_WRONLY|O_CREAT|O_APPEND";
        how = "is added to the end of";
        break;
    default:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        flag_names = "O_WRONLY|O_CREAT|O_TRUNC";
        how = "goes to";
        break;
    }
    /* 0666 before the umask. A new file gets the usual rw-r--r--. */
    fd = open(target, flags, 0666);
    if (fd < 0) {
        explain_event(sh, strerror(errno), "open(\"%s\", %s) failed", target, flag_names);
        shell_error(sh, "%s: %s", target, strerror(errno));
        return false;
    }
    explain_event(sh, NULL, "open(\"%s\", %s) = %d", target, flag_names, fd);
    snprintf(note, sizeof note, "%s %s %s", explain_fd_name(r->fd, name, sizeof name), how, target);
    if (fd != r->fd) {
        explain_event(sh, note, "dup2(%d, %d)", fd, r->fd);
        explain_event(sh, NULL, "close(%d)", fd);
        if (dup2(fd, r->fd) < 0) {
            shell_error(sh, "%d: %s", r->fd, strerror(errno));
            close(fd);
            return false;
        }
        close(fd);
    }
    return true;
}

FD_LEAK_IS_FINE_END

bool redir_apply(struct shell *sh, const struct command *cmd, struct redir_undo *undo)
{
    for (size_t i = 0; i < cmd->nredirs; i++) {
        const struct redirection *r = &cmd->redirs[i];
        char err[256];
        char *target;
        bool ok;

        if (undo != NULL && !backup(sh, undo, r))
            return false;
        target = expand_one(sh, r->target, err, sizeof err);
        if (target == NULL) {
            shell_error(sh, "%s", err);
            return false;
        }
        if (strcmp(target, r->target) != 0)
            explain_event(sh, NULL, "expand: %s  ->  %s", r->target, target);
        if (r->kind == REDIR_DUP_IN || r->kind == REDIR_DUP_OUT)
            ok = apply_dup(sh, r, target);
        else
            ok = apply_file(sh, r, target);
        free(target);
        if (!ok)
            return false;
    }
    return true;
}

void redir_restore(struct shell *sh, struct redir_undo *undo)
{
    /* Last change first. An fd that was changed twice ends up original. */
    for (size_t i = undo->len; i-- > 0;) {
        const struct fd_backup *b = &undo->items[i];
        char name[16], note[64];

        snprintf(note, sizeof note, "%s back as it was", explain_fd_name(b->fd, name, sizeof name));
        if (b->copy >= 0) {
            explain_event(sh, note, "dup2(%d, %d)", b->copy, b->fd);
            explain_event(sh, NULL, "close(%d)", b->copy);
            dup2(b->copy, b->fd);
            close(b->copy);
        } else {
            explain_event(sh, "it wasn't open before", "close(%d)", b->fd);
            close(b->fd);
        }
    }
    free(undo->items);
    *undo = (struct redir_undo){ 0 };
}
