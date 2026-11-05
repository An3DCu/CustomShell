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

static int run_external(char **argv)
{
    pid_t pid;
    int wstatus;

    /* Anything still in stdout's buffer would be copied into the child and
     * could come out twice. Built-ins already flush, but be sure. */
    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "acsh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        int err;

        execvp(argv[0], argv);
        err = errno;
        report_exec_error(argv[0], err);
        /* _exit, not exit: exit would flush stdio buffers copied from the
         * shell, which is how the old acsh printed things twice. */
        _exit(err == ENOENT ? 127 : 126);
    }
    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "acsh: waitpid: %s\n", strerror(errno));
            return 1;
        }
    }
    return exit_status_from_wait(wstatus);
}

int run_pipeline(struct shell *sh, const struct pipeline *pl)
{
    const struct command *cmd = &pl->commands[0];
    const struct builtin *builtin;
    char **argv;
    int status;

    /* Pipes and redirections are the next step. */
    if (pl->ncommands > 1) {
        fputs("acsh: pipes aren't supported yet\n", stderr);
        return 2;
    }
    if (cmd->nredirs > 0) {
        fputs("acsh: redirections aren't supported yet\n", stderr);
        return 2;
    }
    if (cmd->nwords == 0)
        return 0;

    argv = make_argv(cmd);
    builtin = builtin_find(argv[0]);
    if (builtin != NULL)
        status = builtin_run(sh, builtin, argv);
    else
        status = run_external(argv);
    free_argv(argv);
    return status;
}
