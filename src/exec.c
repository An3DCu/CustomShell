#include "exec.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "error.h"
#include "expand.h"
#include "explain.h"
#include "jobs.h"
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
        shell_error(sh, "%s", err);
        return false;
    }
    strvec_items(argv); /* so an empty argv is { NULL }, not NULL */
    return true;
}

/* Expands and sets each NAME=value of cmd, left to right, so B=$A sees an A
 * set just before it. In a child about to exec, export them too: that's
 * what puts X=1 into the environment of `X=1 cmd`. note says for how long,
 * for explain mode. */
static bool apply_assignments(struct shell *sh, const struct command *cmd, bool export,
                              const char *note)
{
    for (size_t i = 0; i < cmd->nassigns; i++) {
        const char *a = cmd->assigns[i];
        size_t n = var_name_len(a);
        char err[256];
        char *name = xstrndup(a, n);
        char *value = expand_assignment(sh, a + n + 1, err, sizeof err);

        if (value == NULL) {
            shell_error(sh, "%s", err);
            free(name);
            return false;
        }
        vars_set(&sh->vars, name, value);
        if (export)
            vars_export(&sh->vars, name);
        explain_event(sh, note, "set %s=%s", name, value);
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

        explain_event(sh, "it was only for this built-in", "%s back as it was", s->name);
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

/* A hint for a command that wasn't found, if it looks like a mistake that
 * is easy to make. */
static void hint_not_found(const struct shell *sh, char *const *argv)
{
    struct stat st;

    /* X = 1: spaces around the = make X a command */
    if (var_name_valid(argv[0]) && argv[1] != NULL && argv[1][0] == '=') {
        const char *value = argv[1][1] != '\0' ? argv[1] + 1 : argv[2] ? argv[2] : "";

        shell_hint(sh, "no spaces around = in an assignment: %s=%s", argv[0], value);
        return;
    }
    /* build.sh, when it's right here: PATH doesn't include the current folder */
    if (stat(argv[0], &st) == 0 && S_ISREG(st.st_mode))
        shell_hint(sh, "it's in this folder; run it as ./%s", argv[0]);
}

/* Says why execvp failed, in the words bash uses. */
static void report_exec_error(const struct shell *sh, char *const *argv, int err)
{
    const char *name = argv[0];
    struct stat st;

    if (err == ENOENT && strchr(name, '/') == NULL) {
        shell_error(sh, "%s: command not found", name);
        hint_not_found(sh, argv);
        return;
    }
    /* exec gives EACCES for a folder too; say what it really is. */
    if (err == EACCES && strchr(name, '/') != NULL && stat(name, &st) == 0
        && S_ISDIR(st.st_mode))
        err = EISDIR;
    shell_error(sh, "%s: %s", name, strerror(err));
    if (err == EACCES && stat(name, &st) == 0 && S_ISREG(st.st_mode)
        && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
        shell_hint(sh, "it isn't executable; chmod +x %s, or run it with acsh %s", name, name);
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

/* ---- if, while, until and for ------------------------------------------ */

/* Something that ends every running list: exit, Ctrl+C, Ctrl+Z, break,
 * continue. */
static bool cut_short(const struct shell *sh)
{
    return sh->exiting || sh->interrupted || sh->stopped || sh->breaks > 0 || sh->continues > 0;
}

/*
 * Called by a loop after each run of its condition or body: true if the
 * loop has to end. break n and continue n count down one loop at a time,
 * so `break 2` ends this loop and the one around it, and `continue 2` ends
 * this one and goes on with the next round of the outer one.
 */
static bool loop_ends(struct shell *sh)
{
    if (sh->exiting || sh->interrupted || sh->stopped)
        return true;
    if (sh->breaks > 0) {
        sh->breaks--;
        explain_event(sh, NULL, "break: the loop ends");
        return true;
    }
    if (sh->continues > 0) {
        sh->continues--;
        explain_event(sh, NULL, sh->continues > 0 ? "continue: this loop ends, an outer one goes on"
                                                  : "continue: on to the next round");
        return sh->continues > 0;
    }
    if (signals_take_interrupt()) {
        /* Ctrl+C while only built-ins ran, as in `while :; do :; done` */
        sh->interrupted = true;
        sh->status = 130;
        return true;
    }
    return false;
}

/* The status a loop gives: its last body's, or 0 if the body never ran,
 * except after exit, Ctrl+C or Ctrl+Z, which keep theirs. */
static int loop_status(const struct shell *sh, int body_status)
{
    return sh->exiting || sh->interrupted || sh->stopped ? sh->status : body_status;
}

/* if: the first branch whose condition gives 0 runs; the status is that
 * body's, or else's, or 0 when nothing ran. */
static int run_if(struct shell *sh, const struct command *cmd)
{
    for (size_t i = 0; i < cmd->nbranches; i++) {
        int cond = run_list(sh, cmd->branches[i].cond);
        bool more = i + 1 < cmd->nbranches;

        if (cut_short(sh))
            return sh->status;
        explain_event(sh, cond == 0 ? "so: then"
                          : more ? "so: try the elif"
                          : cmd->body ? "so: else" : "so: nothing runs",
                      "%s: the condition gave %d", i == 0 ? "if" : "elif", cond);
        if (cond == 0)
            return run_list(sh, cmd->branches[i].body);
    }
    if (cmd->body != NULL)
        return run_list(sh, cmd->body);
    return 0;
}

/* while runs the body as long as the condition gives 0, until as long as
 * it doesn't. */
static int run_while(struct shell *sh, const struct command *cmd)
{
    int status = 0;

    sh->loop_depth++;
    for (;;) {
        int cond = run_list(sh, cmd->cond);

        bool goes_on;

        if (loop_ends(sh))
            break;
        goes_on = (cond == 0) == (cmd->kind == CMD_WHILE);
        explain_event(sh, goes_on ? "so: one more round" : "so: the loop ends",
                      "%s: the condition gave %d", cmd->kind == CMD_WHILE ? "while" : "until", cond);
        if (!goes_on)
            break;
        status = run_list(sh, cmd->body);
        if (loop_ends(sh))
            break;
    }
    sh->loop_depth--;
    return loop_status(sh, status);
}

/* for x in words: the words are expanded once, before the first round,
 * so `for f in *.c` sees the files as they were then. Without `in` it
 * loops over a copy of "$@", so a shift in the body changes nothing. */
static int run_for(struct shell *sh, const struct command *cmd)
{
    struct strvec values = { 0 };
    char err[256];
    int status = 0;

    if (cmd->has_in) {
        if (!expand_words(sh, cmd->words, cmd->nwords, &values, err, sizeof err)) {
            shell_error(sh, "%s", err);
            strvec_free(&values);
            return 2;
        }
    } else {
        for (size_t i = 0; i < sh->params.len; i++)
            strvec_push(&values, xstrdup(sh->params.items[i]));
    }
    sh->loop_depth++;
    for (size_t i = 0; i < values.len; i++) {
        vars_set(&sh->vars, cmd->var, values.items[i]);
        explain_event(sh, NULL, "for: %s=%s, round %zu of %zu", cmd->var, values.items[i], i + 1,
                      values.len);
        status = run_list(sh, cmd->body);
        if (loop_ends(sh))
            break;
    }
    sh->loop_depth--;
    strvec_free(&values);
    return loop_status(sh, status);
}

static int run_compound(struct shell *sh, const struct command *cmd)
{
    switch (cmd->kind) {
    case CMD_IF:
        return run_if(sh, cmd);
    case CMD_WHILE:
    case CMD_UNTIL:
        return run_while(sh, cmd);
    case CMD_FOR:
        return run_for(sh, cmd);
    case CMD_SIMPLE:
        break;
    }
    return 0;
}

/* A compound on its own runs in the shell, so `for` sets the variable for
 * later and `while ... done < file` gives the file back afterwards. */
static int run_compound_here(struct shell *sh, const struct command *cmd)
{
    struct redir_undo undo = { 0 };
    int status;

    fflush(stdout);
    if (redir_apply(sh, cmd, &undo))
        status = run_compound(sh, cmd);
    else
        status = 1;
    fflush(stdout);
    redir_restore(sh, &undo);
    return status;
}

/* What a child is for, for the fork line: its command's name as typed. */
static const char *command_name(const struct command *cmd, char **argv)
{
    switch (cmd->kind) {
    case CMD_IF:     return "an if";
    case CMD_WHILE:  return "a while loop";
    case CMD_UNTIL:  return "an until loop";
    case CMD_FOR:    return "a for loop";
    case CMD_SIMPLE: break;
    }
    if (argv != NULL && argv[0] != NULL)
        return argv[0];
    return cmd->nwords > 0 ? cmd->words[0] : "a command with no name";
}

/* A child that runs a built-in or a block won't exec, so the report pipe
 * wouldn't close on its own and the shell would wait. It says what it does
 * and closes the pipe; anything after goes straight to the explanation. */
static void stop_reporting(struct shell *sh, const char *what)
{
    if (!explain_on(sh) || sh->explain_out == sh->explain_fd)
        return;
    explain_event(sh, "no exec: the shell's own code does it", "%s runs in this child", what);
    close(sh->explain_out);
    sh->explain_out = sh->explain_fd;
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

    if (cmd->kind != CMD_SIMPLE) {
        /* `for ...; done | sort`: the whole loop runs in this child */
        int status;

        if (!redir_apply(sh, cmd, NULL))
            _exit(1);
        stop_reporting(sh, command_name(cmd, NULL));
        status = run_compound(sh, cmd);
        fflush(stdout);
        _exit(status);
    }
    if (argv == NULL) {
        if (!expand_argv(sh, cmd, &own))
            _exit(2);
        argv = strvec_items(&own);
        explain_words(sh, cmd->words, cmd->nwords, argv);
    }
    if (!redir_apply(sh, cmd, NULL))
        _exit(1);
    if (argv[0] == NULL)
        _exit(0);
    if (!apply_assignments(sh, cmd, true, "only in this command's environment"))
        _exit(2);
    b = builtin_find(argv[0]);
    if (b != NULL) {
        stop_reporting(sh, argv[0]);
        _exit(builtin_run(sh, b, argv)); /* builtin_run flushes stdout */
    }
    /* The child's environment is built from the shell's table only now,
     * and execvp passes it on. */
    environ = vars_environ(&sh->vars);
    explain_exec(sh, argv);
    execvp(argv[0], argv);
    err = errno;
    report_exec_error(sh, argv, err);
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
    if (b != NULL)
        explain_event(sh, "no fork: it runs in the shell itself", "built-in %s", argv[0]);
    if (!redir_apply(sh, cmd, &undo)) {
        status = 1;
    } else if (b == NULL) {
        status = apply_assignments(sh, cmd, false, "a shell variable, it stays") ? 0 : 2;
    } else if (b->special) {
        status = apply_assignments(sh, cmd, false, "stays: a special built-in")
                     ? builtin_run(sh, b, argv) : 2;
    } else {
        struct saved_var *saved = save_assigned(sh, cmd);

        if (apply_assignments(sh, cmd, false, "only while the built-in runs"))
            status = builtin_run(sh, b, argv);
        else
            status = 2;
        restore_assigned(sh, saved, cmd->nassigns);
    }
    redir_restore(sh, &undo);
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
/* Moves fd to 10 or above, close-on-exec, out of the way of the 0 to 9 a
 * command may redirect. Returns the new fd, or -1. */
static int move_high(int fd)
{
    int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);

    close(fd);
    return high;
}

/* The pipe a child reports its explain lines through. Both ends are high
 * and close-on-exec, so the shell sees end of file once the child's exec
 * has worked, or once it exits. */
static bool report_pipe(int fds[2])
{
    int raw[2];

    if (pipe(raw) != 0)
        return false;
    fds[0] = move_high(raw[0]);
    fds[1] = move_high(raw[1]);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0)
            close(fds[0]);
        if (fds[1] >= 0)
            close(fds[1]);
        return false;
    }
    return true;
}

/* Copies what the child reports to the explanation, until it execs. */
static void relay_report(struct shell *sh, int fd)
{
    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof buf)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(sh->explain_out, buf + done, (size_t)(n - done));

            if (w < 0 && errno != EINTR)
                return;
            if (w > 0)
                done += w;
        }
    }
}

/*
 * In a child that runs in the background without job control (a script):
 * as POSIX says, stdin is /dev/null, so `cat &` can't take the script's
 * input, and Ctrl+C and Ctrl+\ are ignored, so they only stop what's in
 * front. With job control, the job's own process group does both.
 */
static void background_child_setup(struct shell *sh, bool first)
{
    struct sigaction ignore = { 0 };
    int fd;

    if (sh->job_control)
        return;
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGINT, &ignore, NULL);
    sigaction(SIGQUIT, &ignore, NULL);
    if (!first)
        return; /* the others read from the pipe before them */
    fd = open("/dev/null", O_RDONLY);
    if (fd >= 0 && fd != STDIN_FILENO) {
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
}

/* bg_text is the job's text when the pipeline runs in the background (cmd &),
 * or NULL to run it in front and wait. */
static int run_forked(struct shell *sh, const struct pipeline *pl, char **first_argv,
                      const char *bg_text)
{
    size_t n = pl->ncommands;
    size_t started = 0;
    pid_t *pids = xcalloc(n, sizeof *pids);
    int prev_read = -1; /* read end of the pipe from the previous command */
    pid_t pgid = 0;     /* the pipeline's process group: the first child's pid */
    int status = 0;
    bool failed = false;
    bool hold_signals = bg_text != NULL && !sh->job_control;
    sigset_t held, before;

    sigemptyset(&held);
    sigaddset(&held, SIGINT);
    sigaddset(&held, SIGQUIT);

    fflush(stdout); /* or the children would get a copy of what's waiting */
    for (size_t i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        int report[2] = { -1, -1 };
        pid_t pid;

        if (i + 1 < n && pipe(fds) != 0) {
            shell_error(sh, "pipe: %s", strerror(errno));
            failed = true;
            break;
        }
        if (fds[0] >= 0 && explain_on(sh)) {
            char note[64];

            snprintf(note, sizeof note, "what goes into %d comes out of %d", fds[1], fds[0]);
            explain_event(sh, note, "pipe() = [%d, %d]", fds[0], fds[1]);
        }
        if (explain_on(sh) && !report_pipe(report))
            report[0] = report[1] = -1;
        /* A background job in a script ignores Ctrl+C, but only once the
         * child has set that up. Until then SIGINT and SIGQUIT stay blocked,
         * so one that comes in between waits and is then dropped by the
         * child, instead of killing it first. The shell unblocks right
         * after fork, and a Ctrl+C meant for it arrives as usual. */
        if (hold_signals)
            sigprocmask(SIG_BLOCK, &held, &before);
        pid = fork();
        if (pid != 0 && hold_signals)
            sigprocmask(SIG_SETMASK, &before, NULL);
        if (pid < 0) {
            shell_error(sh, "fork: %s", strerror(errno));
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            if (report[0] >= 0) {
                close(report[0]);
                close(report[1]);
            }
            failed = true;
            break;
        }
        if (pid == 0) {
            if (report[1] >= 0) {
                close(report[0]);
                sh->explain_out = report[1];
            }
            /* The group and the terminal first, while SIGTTOU is still
             * ignored as in the shell; then the signals go back to their
             * defaults, so Ctrl+C and Ctrl+Z work on the command. */
            jobs_join_group(sh, pgid, bg_text == NULL);
            if (sh->interactive)
                signals_reset_in_child();
            if (bg_text != NULL)
                background_child_setup(sh, i == 0);
            /* The child is set up in its group now. A loop that runs in it
             * (`for ...; done | cat`) must not start groups of its own or
             * take the terminal back for the shell: job control stays with
             * the shell, as in bash and dash. */
            sh->job_control = false;
            if (hold_signals)
                sigprocmask(SIG_SETMASK, &before, NULL); /* now ignored: let them through */
            if (prev_read >= 0 && prev_read != STDIN_FILENO) {
                explain_event(sh, "stdin comes from the pipe", "dup2(%d, 0)", prev_read);
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (fds[1] >= 0) {
                close(fds[0]); /* the next command reads it, not this one */
                if (fds[1] != STDOUT_FILENO) {
                    explain_event(sh, "stdout goes into the pipe", "dup2(%d, 1)", fds[1]);
                    dup2(fds[1], STDOUT_FILENO);
                    close(fds[1]);
                }
            }
            sh->line = sh->unit_line + pl->commands[i].line;
            run_in_child(sh, &pl->commands[i], i == 0 ? first_argv : NULL);
        }
        pids[started++] = pid;
        if (explain_on(sh)) {
            char note[256];

            snprintf(note, sizeof note, "a child for %s",
                     command_name(&pl->commands[i], i == 0 ? first_argv : NULL));
            explain_event(sh, note, "fork() = %ld", (long)pid);
        }
        if (pgid == 0)
            pgid = pid;
        jobs_set_group(sh, pid, pgid);
        if (i == 0 && bg_text == NULL)
            jobs_give_terminal(sh, pgid);
        if (report[0] >= 0) {
            close(report[1]);
            relay_report(sh, report[0]);
            close(report[0]);
        }
        if (prev_read >= 0) {
            explain_event(sh, "the shell lets go of its end", "close(%d)", prev_read);
            close(prev_read);
        }
        if (fds[1] >= 0) {
            explain_event(sh, "the shell lets go of its end", "close(%d)", fds[1]);
            close(fds[1]);
        }
        prev_read = fds[0];
    }
    if (prev_read >= 0) {
        explain_event(sh, "the shell lets go of its end", "close(%d)", prev_read);
        close(prev_read);
    }

    if (bg_text != NULL && started > 0) {
        /* In the background: the job takes the pids, and jobs_reap
         * collects them later, so none is left as a zombie. */
        sh->last_bg = pids[started - 1];
        jobs_add(sh, sh->job_control ? pgid : 0, pids, started, bg_text);
        return failed ? 1 : 0;
    }

    if (started == 0) {
        free(pids);
        return 1;
    }
    /* Wait for all of them, so none is left as a zombie, or until Ctrl+Z
     * stops them. The status is the last command's, as in POSIX sh
     * without pipefail. */
    status = jobs_run_in_front(sh, sh->job_control ? pgid : 0, pids, started, pl->text);
    return failed ? 1 : status;
}

static int run_pipeline(struct shell *sh, const struct pipeline *pl)
{
    const struct command *cmd = &pl->commands[0];
    struct strvec argv = { 0 };
    const struct builtin *b;
    char **av;
    int status;

    sh->line = sh->unit_line + cmd->line;
    sh->commands++;
    if (pl->ncommands > 1)
        return run_forked(sh, pl, NULL, NULL);
    if (cmd->kind != CMD_SIMPLE)
        return run_compound_here(sh, cmd);

    /* Expand first: only then is it clear what runs. After X=cd, `$X /`
     * runs the built-in, and an empty $E leaves no command at all. */
    if (!expand_argv(sh, cmd, &argv)) {
        strvec_free(&argv);
        return 2;
    }
    av = strvec_items(&argv);
    explain_words(sh, cmd->words, cmd->nwords, av);
    b = av[0] != NULL ? builtin_find(av[0]) : NULL;
    if (b != NULL || av[0] == NULL)
        status = run_in_shell(sh, cmd, b, av);
    else
        status = run_forked(sh, pl, av, NULL);
    strvec_free(&argv);
    return status;
}

/* Whether the pipeline after && or || runs, given the status so far. */
static bool should_run(const struct pipeline *pl, int status)
{
    switch (pl->run_if) {
    case RUN_IF_OK:     return status == 0;
    case RUN_IF_FAILED: return status != 0;
    case RUN_ALWAYS:    break;
    }
    return true;
}

/* a && b || c: each pipeline looks at the status of whatever ran last,
 * so in `false && x || y` the skipped x leaves 1 for y. */
static int run_and_or(struct shell *sh, const struct and_or *ao)
{
    for (size_t j = 0; j < ao->npipelines; j++) {
        const struct pipeline *pl = &ao->pipelines[j];
        int status;

        if (!should_run(pl, sh->status))
            continue;
        status = run_pipeline(sh, pl);
        if (pl->negate)
            status = status == 0 ? 1 : 0;
        sh->status = status;
        if (cut_short(sh))
            break;
    }
    return sh->status;
}

int run_list(struct shell *sh, const struct list *list)
{
    for (size_t i = 0; i < list->nitems; i++) {
        const struct and_or *ao = &list->items[i];

        if (ao->background) {
            /* cmd &: the parser lets only one pipeline through, and it
             * becomes a job of its own processes */
            run_forked(sh, &ao->pipelines[0], NULL, ao->pipelines[0].text);
            sh->status = 0; /* POSIX: starting a job always works */
        } else {
            run_and_or(sh, ao);
        }
        if (cut_short(sh))
            return sh->status;
    }
    return sh->status;
}
