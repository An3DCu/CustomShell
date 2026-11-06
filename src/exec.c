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
#include "expand.h"
#include "redirect.h"
#include "signals.h"
#include "strvec.h"
#include "vars.h"
#include "xalloc.h"

/* Not in any header: POSIX says a program declares it itself. */
extern char **environ;

/* Expands cmd's words into argv. Prints the error and returns false if a
 * word can't be expanded ("${A B}"). */
static bool expand_argv(struct shell *sh, const struct command *cmd, struct strvec *argv)
{
    char err[256];

    if (!expand_command(sh, cmd->words, cmd->nwords, argv, err, sizeof err)) {
        fprintf(stderr, "acsh: %s\n", err);
        return false;
    }
    strvec_items(argv); /* so an empty argv is { NULL }, not NULL */
    return true;
}

/* Expands and sets each NAME=value of cmd, left to right, so B=$A sees an A
 * set just before it. In a child about to exec, export them too: that's
 * what puts X=1 into the environment of `X=1 cmd`. */
static bool apply_assignments(struct shell *sh, const struct command *cmd, bool export)
{
    for (size_t i = 0; i < cmd->nassigns; i++) {
        const char *a = cmd->assigns[i];
        size_t n = var_name_len(a);
        char err[256];
        char *name = xstrndup(a, n);
        char *value = expand_assignment(sh, a + n + 1, err, sizeof err);

        if (value == NULL) {
            fprintf(stderr, "acsh: %s\n", err);
            free(name);
            return false;
        }
        vars_set(&sh->vars, name, value);
        if (export)
            vars_export(&sh->vars, name);
        free(name);
        free(value);
    }
    return true;
}

/* A variable as it was before `X=1 builtin` changed it for one command. */
struct saved_var {
    char *name;
    char *value;   /* NULL if it had none */
    bool existed;
};

static struct saved_var *save_assigned(const struct shell *sh, const struct command *cmd)
{
    struct saved_var *saved = xcalloc(cmd->nassigns + 1, sizeof *saved);

    for (size_t i = 0; i < cmd->nassigns; i++) {
        const char *a = cmd->assigns[i];
        const char *value;

        saved[i].name = xstrndup(a, var_name_len(a));
        saved[i].existed = vars_exists(&sh->vars, saved[i].name);
        value = vars_get(&sh->vars, saved[i].name);
        saved[i].value = value ? xstrdup(value) : NULL;
    }
    return saved;
}

/* Puts them back, last first, so X=1 X=2 ends with the X from before. */
static void restore_assigned(struct shell *sh, struct saved_var *saved, size_t n)
{
    for (size_t i = n; i-- > 0;) {
        struct saved_var *s = &saved[i];

        if (!s->existed) {
            vars_unset(&sh->vars, s->name);
        } else if (s->value != NULL) {
            vars_set(&sh->vars, s->name, s->value);
        } else { /* exported, but with no value: only `export X` makes those */
            vars_unset(&sh->vars, s->name);
            vars_export(&sh->vars, s->name);
        }
        free(s->name);
        free(s->value);
    }
    free(saved);
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
 * on stdin and stdout. argv is the expanded words if the shell expanded them
 * already, or NULL to expand them here. The command's own redirections come
 * after the pipe, so `echo hi > f | cat` writes to f, as in bash. Never
 * returns.
 *
 * Every way out is _exit, never exit: exit would flush stdio buffers the
 * child copied from the shell, which is how the old acsh printed twice.
 */
static _Noreturn void run_in_child(struct shell *sh, const struct command *cmd, char **argv)
{
    struct strvec own = { 0 };
    const struct builtin *b;
    int err;

    if (argv == NULL) {
        if (!expand_argv(sh, cmd, &own))
            _exit(2);
        argv = strvec_items(&own);
    }
    if (!redir_apply(sh, cmd, NULL))
        _exit(1);
    if (argv[0] == NULL)
        _exit(0);
    if (!apply_assignments(sh, cmd, true))
        _exit(2);
    b = builtin_find(argv[0]);
    if (b != NULL)
        _exit(builtin_run(sh, b, argv)); /* builtin_run flushes stdout */
    /* The child's environment is built from the shell's table only now,
     * and execvp passes it on. */
    environ = vars_environ(&sh->vars);
    execvp(argv[0], argv);
    err = errno;
    report_exec_error(argv[0], err);
    _exit(err == ENOENT ? 127 : 126);
}

/*
 * A built-in on its own, or a command with no words left (`> f`, `X=1`),
 * runs in the shell: cd, exit and X=1 only work there. Its redirections are
 * undone afterwards so the shell's stdin, stdout and stderr are as before.
 *
 * Assignments stay when there is no command or a special built-in, and last
 * for one command otherwise: `X=2 cd` doesn't change X. That's POSIX, and
 * what dash does.
 */
static int run_in_shell(struct shell *sh, const struct command *cmd,
                        const struct builtin *b, char **argv)
{
    struct redir_undo undo = { 0 };
    int status = 0;

    /* Anything the shell printed before belongs to the old stdout. */
    fflush(stdout);
    if (!redir_apply(sh, cmd, &undo)) {
        status = 1;
    } else if (b == NULL) {
        status = apply_assignments(sh, cmd, false) ? 0 : 2;
    } else if (b->special) {
        status = apply_assignments(sh, cmd, false) ? builtin_run(sh, b, argv) : 2;
    } else {
        struct saved_var *saved = save_assigned(sh, cmd);

        if (apply_assignments(sh, cmd, false))
            status = builtin_run(sh, b, argv);
        else
            status = 2;
        restore_assigned(sh, saved, cmd->nassigns);
    }
    redir_restore(&undo);
    return status;
}

/*
 * a | b | c: one child per command, connected by pipes. Each child expands
 * its own words, so `echo $X | cat` reads X in the child. For a single
 * command the shell has expanded them already (first_argv), to see whether
 * it's a built-in.
 *
 * The parent only ever holds the read end of the previous pipe and the new
 * pipe, and closes each end as soon as the child that needs it has it. That
 * matters: a reader only sees end of input once every write end is closed,
 * so one forgotten end in the parent and `yes | head -n 1` never finishes.
 */
static int run_forked(struct shell *sh, const struct pipeline *pl, char **first_argv)
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
            run_in_child(sh, &pl->commands[i], i == 0 ? first_argv : NULL);
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
    const struct command *cmd = &pl->commands[0];
    struct strvec argv = { 0 };
    const struct builtin *b;
    char **av;
    int status;

    if (pl->ncommands > 1)
        return run_forked(sh, pl, NULL);

    /* Expand first: only then is it clear what runs. After X=cd, `$X /`
     * runs the built-in, and an empty $E leaves no command at all. */
    if (!expand_argv(sh, cmd, &argv)) {
        strvec_free(&argv);
        return 2;
    }
    av = strvec_items(&argv);
    b = av[0] != NULL ? builtin_find(av[0]) : NULL;
    if (b != NULL || av[0] == NULL)
        status = run_in_shell(sh, cmd, b, av);
    else
        status = run_forked(sh, pl, av);
    strvec_free(&argv);
    return status;
}
