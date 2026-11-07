/*
 * acsh: a small Unix shell.
 *
 * main reads the options, sets up the shell, and hands the commands to
 * script_run: from the terminal or a pipe, from a script file, or from the
 * text after -c. See script.c for the loop itself.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jobs.h"
#include "script.h"
#include "shell.h"
#include "signals.h"
#include "strvec.h"
#include "vars.h"
#include "version.h"
#include "xalloc.h"

extern char **environ; /* POSIX: declared by the program itself */

static void usage(FILE *out)
{
    fputs("usage: acsh [--own-tools] [--explain] [file [arg ...]]\n"
          "       acsh [--own-tools] [--explain] -c command [name [arg ...]]\n"
          "       acsh --help | --version\n"
          "\n"
          "  file         run the commands in file, with args as $1, $2, ...\n"
          "  -c command   run command, with name as $0 and args as $1, $2, ...\n"
          "  --own-tools  use my own cat, head, wc and the other tools from the\n"
          "               tools folder next to acsh, before the system's\n"
          "  --explain    show the plan and every system call for each line\n"
          "               (like typing explain in front of every line)\n"
          "\n"
          "With neither, acsh reads commands from stdin, with a prompt when\n"
          "that's a terminal.\n",
          out);
}

/* The path of the running acsh, from Linux's /proc/self/exe. */
static char *own_path(void)
{
    for (size_t size = 256;; size *= 2) {
        char *buf = xmalloc(size);
        ssize_t n = readlink("/proc/self/exe", buf, size);

        if (n < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)n < size) { /* readlink doesn't add the final 0 */
            buf[n] = '\0';
            return buf;
        }
        free(buf);
    }
}

/* --own-tools: put the tools folder next to acsh at the front of PATH, so
 * `cat` finds my cat first. Children see the same PATH. */
static bool use_own_tools(struct shell *sh)
{
    char *exe = own_path();
    char *slash, *tools, *path;
    const char *old;
    struct stat st;

    if (exe == NULL) {
        fprintf(stderr, "acsh: --own-tools: can't find acsh itself: %s\n", strerror(errno));
        return false;
    }
    slash = strrchr(exe, '/');
    *slash = '\0'; /* /proc/self/exe is always an absolute path */
    tools = xmalloc(strlen(exe) + sizeof "/tools");
    sprintf(tools, "%s/tools", exe);
    free(exe);
    if (stat(tools, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "acsh: --own-tools: no tools folder at %s\n", tools);
        free(tools);
        return false;
    }
    old = vars_get(&sh->vars, "PATH");
    path = xmalloc(strlen(tools) + (old ? strlen(old) : 0) + 2);
    if (old != NULL && old[0] != '\0')
        sprintf(path, "%s:%s", tools, old);
    else
        strcpy(path, tools);
    vars_set(&sh->vars, "PATH", path);
    vars_export(&sh->vars, "PATH");
    free(path);
    free(tools);
    return true;
}

/* Opens the script for `acsh file`, or says why it can't, with bash's
 * exit statuses: 127 if it isn't there, 126 if it can't be read. */
static int open_script(const char *path, int *status)
{
    int fd = script_open(path);

    if (fd < 0) {
        fprintf(stderr, "acsh: %s: %s\n", path, strerror(errno));
        *status = errno == ENOENT ? 127 : 126;
    }
    return fd;
}

int main(int argc, char **argv)
{
    struct shell sh = { 0 };
    struct reader r = { .fd = STDIN_FILENO };
    const char *command = NULL;
    int status = 0;
    int i = 1;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("acsh %s\n", ACSH_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    /* From here on the shell keeps its variables in its own table. */
    vars_init(&sh.vars, environ);
    sh.pid = getpid();
    sh.name = argv[0];

    /* Options come first; the first other word is the script, and
     * everything after it belongs to the script. */
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "--explain") == 0) {
            sh.explain_all = true;
        } else if (strcmp(argv[i], "--own-tools") == 0) {
            if (!use_own_tools(&sh)) {
                status = 2;
                goto out;
            }
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fputs("acsh: -c: needs a command\n", stderr);
                usage(stderr);
                status = 2;
                goto out;
            }
            command = argv[++i];
            i++;
            break;
        } else {
            fprintf(stderr, "acsh: unknown option '%s'\n", argv[i]);
            usage(stderr);
            status = 2;
            goto out;
        }
    }

    if (command != NULL) {
        /* acsh -c 'echo $0 $1' name one */
        r = (struct reader){ .fd = -1, .text = command, .name = "-c" };
        if (i < argc)
            sh.name = r.name = argv[i++]; /* bash names errors after it too */
    } else if (i < argc) {
        /* acsh script.sh one two: $0 is the script */
        sh.name = r.name = argv[i++];
        r.fd = open_script(sh.name, &status);
        if (r.fd < 0)
            goto out;
    } else {
        /* POSIX: reading commands from stdin, a shell is interactive when
         * stdin and stderr are terminals. */
        sh.interactive = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    }
    for (; i < argc; i++)
        strvec_push(&sh.params, xstrdup(argv[i]));

    r.interactive = sh.interactive;
    if (sh.interactive) {
        jobs_init(&sh); /* first: it may have to wait to be in front */
        signals_init_interactive();
    }
    status = script_run(&sh, &r);
    if (r.fd > STDIN_FILENO)
        close(r.fd);
out:
    strvec_free(&sh.params);
    vars_free(&sh.vars);
    return status;
}
