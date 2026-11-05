#include "exec.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "lexer.h"
#include "redirect.h"
#include "signals.h"
#include "xalloc.h"

/* The command's words without their quotes, NULL-terminated for execvp. */
static char **make_argv(const struct command *cmd)
{
    char **argv = xcalloc(cmd->nwords + 1, sizeof *argv);

    for (size_t i = 0; i < cmd->nwords; i++)
        argv[i] = word_unquote(cmd->words[i]);
    return argv;
}

static void free_argv(char **argv)
{
    for (size_t i = 0; argv[i] != NULL; i++)
        free(argv[i]);
    free(argv);
}

/* The built-in this command runs, or NULL for a program (or no words). */
static const struct builtin *find_builtin(const struct command *cmd)
{
    const struct builtin *b;
    char *name;

    if (cmd->nwords == 0)
        return NULL;
    name = word_unquote(cmd->words[0]);
    b = builtin_find(name);
    free(name);
    return b;
}

/* Says why execvp failed, in the words bash uses. */
static void report_exec_error(const char *name, int err)
{
    struct stat st;

    if (err == ENOENT && strchr(name, '/') == NULL) {
        fprintf(stderr, "acsh: %s: command not found\n", name);
        return;
    }
    /* exec gives EACCES for a folder too; say what it really is. */
    if (err == EACCES && strchr(name, '/') != NULL && stat(name, &st) == 0
        && S_ISDIR(st.st_mode))
        err = EISDIR;
    fprintf(stderr, "acsh: %s: %s\n", name, strerror(err));
}

int exit_status_from_wait(int wstatus)
{
    if (WIFEXITED(wstatus))
        return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);

        if (sig != SIGINT && sig != SIGPIPE) {
            bool core = false;
#ifdef WCOREDUMP /* not in POSIX, but Linux and the BSDs have it */
            core = WCOREDUMP(wstatus);
#endif
            fprintf(stderr, "%s%s\n", strsignal(sig), core ? " (core dumped)" : "");
        }
        return 128 + sig;
    }
    return 1;
}

static int wait_for(struct shell *sh, pid_t pid)
{
    int wstatus;

    /* Ctrl+C reaches the shell too and interrupts waitpid with EINTR. The
     * child got the same signal, so keep waiting for it. */
    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "acsh: waitpid: %s\n", strerror(errno));
            return 1;
        }
    }
    if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGINT)
        sh->interrupted = true;
    return exit_status_from_wait(wstatus);
}

/*
 * Runs one command of a pipeline in a child that already has its pipe ends
 * on stdin and stdout. The command's own redirections come after the pipe,
 * so `echo hi > f | cat` writes to f, as in bash. Never returns.
 *
 * Every way out is _exit, never exit: exit would flush stdio buffers the
 * child copied from the shell, which is how the old acsh printed twice.
 */
static _Noreturn void run_in_child(struct shell *sh, const struct command *cmd)
{
    const struct builtin *b;
    char **argv;
    int err;

    if (!redir_apply(cmd, NULL))
        _exit(1);
    if (cmd->nwords == 0)
        _exit(0);
    argv = make_argv(cmd);
    b = builtin_find(argv[0]);
    if (b != NULL)
        _exit(builtin_run(sh, b, argv)); /* builtin_run flushes stdout */
    execvp(argv[0], argv);
    err = errno;
    report_exec_error(argv[0], err);
    _exit(err == ENOENT ? 127 : 126);
}

/*
 * A built-in on its own, or a command with only redirections (`> f`), runs
 * in the shell: cd and exit only work there. Its redirections are undone
 * afterwards so the shell's stdin, stdout and stderr are as before.
 */
static int run_in_shell(struct shell *sh, const struct command *cmd,
                        const struct builtin *b)
{
    struct redir_undo undo = { 0 };
    int status = 0;

    /* Anything the shell printed before belongs to the old stdout. */
    fflush(stdout);
    if (!redir_apply(cmd, &undo)) {
        status = 1;
    } else if (b != NULL) {
        char **argv = make_argv(cmd);

        status = builtin_run(sh, b, argv);
        free_argv(argv);
    }
    redir_restore(&undo);
    return status;
}

/*
 * a | b | c: one child per command, connected by pipes.
 *
 * The parent only ever holds the read end of the previous pipe and the new
 * pipe, and closes each end as soon as the child that needs it has it. That
 * matters: a reader only sees end of input once every write end is closed,
 * so one forgotten end in the parent and `yes | head -n 1` never finishes.
 */
static int run_forked(struct shell *sh, const struct pipeline *pl)
{
    size_t n = pl->ncommands;
    size_t started = 0;
    pid_t *pids = xcalloc(n, sizeof *pids);
    int prev_read = -1; /* read end of the pipe from the previous command */
    int status = 0;
    bool failed = false;

    fflush(stdout); /* or the children would get a copy of what's waiting */
    for (size_t i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        pid_t pid;

        if (i + 1 < n && pipe(fds) != 0) {
            fprintf(stderr, "acsh: pipe: %s\n", strerror(errno));
            failed = true;
            break;
        }
        pid = fork();
        if (pid < 0) {
            fprintf(stderr, "acsh: fork: %s\n", strerror(errno));
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            failed = true;
            break;
        }
        if (pid == 0) {
            if (sh->interactive)
                signals_reset_in_child();
            if (prev_read >= 0 && prev_read != STDIN_FILENO) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (fds[1] >= 0) {
                close(fds[0]); /* the next command reads it, not this one */
                if (fds[1] != STDOUT_FILENO) {
                    dup2(fds[1], STDOUT_FILENO);
                    close(fds[1]);
                }
            }
            run_in_child(sh, &pl->commands[i]);
        }
        pids[started++] = pid;
        if (prev_read >= 0)
            close(prev_read);
        if (fds[1] >= 0)
            close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0)
        close(prev_read);

    /* Wait for all of them, so none is left as a zombie. The pipeline's
     * status is the last command's, as in POSIX sh without pipefail. */
    for (size_t i = 0; i < started; i++) {
        int s = wait_for(sh, pids[i]);

        if (i == n - 1)
            status = s;
    }
    free(pids);
    return failed ? 1 : status;
}

int run_pipeline(struct shell *sh, const struct pipeline *pl)
{
    if (pl->ncommands == 1) {
        const struct command *cmd = &pl->commands[0];
        const struct builtin *b = find_builtin(cmd);

        if (b != NULL || cmd->nwords == 0)
            return run_in_shell(sh, cmd, b);
    }
    return run_forked(sh, pl);
}
