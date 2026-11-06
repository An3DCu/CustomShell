#include "redirect.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "expand.h"
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

/* Keeps a copy of fd before its first change. */
static bool backup(struct redir_undo *undo, int fd)
{
    int copy;

    for (size_t i = 0; i < undo->len; i++) {
        if (undo->items[i].fd == fd)
            return true; /* saved already; the first original is the one to restore */
    }
    /* `10>f` would overwrite one of my own copies. Move it out of the way
     * first; fd 10 then counts as "wasn't open" for the user. */
    for (size_t i = 0; i < undo->len; i++) {
        if (undo->items[i].copy == fd) {
            int moved = fcntl(fd, F_DUPFD_CLOEXEC, BACKUP_MIN_FD);

            if (moved < 0) {
                fprintf(stderr, "acsh: %d: %s\n", fd, strerror(errno));
                return false;
            }
            undo->items[i].copy = moved;
            push_backup(undo, fd, -1);
            return true;
        }
    }
    copy = fcntl(fd, F_DUPFD_CLOEXEC, BACKUP_MIN_FD);
    if (copy < 0 && errno != EBADF) {
        fprintf(stderr, "acsh: %d: %s\n", fd, strerror(errno));
        return false;
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

/* 2>&1, <&3, >&- */
static bool apply_dup(const struct redirection *r, const char *target)
{
    int src;

    if (strcmp(target, "-") == 0) {
        close(r->fd); /* closing an fd that isn't open is fine */
        return true;
    }
    src = parse_fd(target);
    if (src < 0) {
        fprintf(stderr, "acsh: %s: bad file descriptor\n", target);
        return false;
    }
    if (!is_open(src)) {
        fprintf(stderr, "acsh: %d: %s\n", src, strerror(EBADF));
        return false;
    }
    if (src != r->fd && dup2(src, r->fd) < 0) {
        fprintf(stderr, "acsh: %d: %s\n", r->fd, strerror(errno));
        return false;
    }
    return true;
}

/* <, > and >> */
static bool apply_file(const struct redirection *r, const char *target)
{
    int flags;
    int fd;

    switch (r->kind) {
    case REDIR_IN:     flags = O_RDONLY; break;
    case REDIR_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default:           flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    }
    /* 0666 before the umask, so a new file gets the usual rw-r--r--. */
    fd = open(target, flags, 0666);
    if (fd < 0) {
        fprintf(stderr, "acsh: %s: %s\n", target, strerror(errno));
        return false;
    }
    if (fd != r->fd) {
        if (dup2(fd, r->fd) < 0) {
            fprintf(stderr, "acsh: %d: %s\n", r->fd, strerror(errno));
            close(fd);
            return false;
        }
        close(fd);
    }
    return true;
}

bool redir_apply(struct shell *sh, const struct command *cmd, struct redir_undo *undo)
{
    for (size_t i = 0; i < cmd->nredirs; i++) {
        const struct redirection *r = &cmd->redirs[i];
        char err[256];
        char *target;
        bool ok;

        if (undo != NULL && !backup(undo, r->fd))
            return false;
        target = expand_one(sh, r->target, err, sizeof err);
        if (target == NULL) {
            fprintf(stderr, "acsh: %s\n", err);
            return false;
        }
        if (r->kind == REDIR_DUP_IN || r->kind == REDIR_DUP_OUT)
            ok = apply_dup(r, target);
        else
            ok = apply_file(r, target);
        free(target);
        if (!ok)
            return false;
    }
    return true;
}

void redir_restore(struct redir_undo *undo)
{
    /* Last change first, so an fd that was changed twice ends up original. */
    for (size_t i = undo->len; i-- > 0;) {
        const struct fd_backup *b = &undo->items[i];

        if (b->copy >= 0) {
            dup2(b->copy, b->fd);
            close(b->copy);
        } else {
            close(b->fd);
        }
    }
    free(undo->items);
    *undo = (struct redir_undo){ 0 };
}
