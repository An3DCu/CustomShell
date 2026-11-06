#ifndef ACSH_REDIRECT_H
#define ACSH_REDIRECT_H

#include <stdbool.h>
#include <stddef.h>

#include "parser.h"
#include "shell.h"

/*
 * Redirections, applied in the order they were written: in `cmd 2>&1 >f`,
 * stderr goes where stdout pointed before stdout moved to f.
 *
 * In a child this is simple: change the fds, then exec. A built-in runs in
 * the shell itself, though, so the shell has to put its own fds back
 * afterwards. Before an fd changes for the first time, a copy of it is kept
 * at 10 or above (close-on-exec, so no command inherits it), and
 * redir_restore() puts everything back in reverse order.
 */

struct fd_backup {
    int fd;   /* the fd that was changed */
    int copy; /* where the original is kept, or -1 if fd wasn't open */
};

struct redir_undo {
    struct fd_backup *items;
    size_t len;
    size_t cap;
};

/* Applies cmd's redirections, expanding each target first ($F, ~/log; no
 * splitting). Pass undo in the shell, NULL in a child. On failure, prints why ("acsh: out.txt: Permission denied") and returns
 * false; the changes made before it stay until redir_restore. */
bool redir_apply(struct shell *sh, const struct command *cmd, struct redir_undo *undo);

/* Puts every saved fd back and empties undo. */
void redir_restore(struct redir_undo *undo);

#endif
