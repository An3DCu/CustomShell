#ifndef ACSH_ANALYZER_H
#define ACSH_ANALYZER_H

/*
 * For code where gcc -fanalyzer (`make analyze`) reports a file
 * descriptor leak that isn't one. Its fd model has two blind spots here.
 * It follows a forked child into the code the child runs. It misses the
 * _exit that closes all of that child's fds. It also takes the fd that dup2
 * returns as a new one to close. dup2 onto fd 1 is meant to leave fd 1 open
 * for the command.
 *
 * Only gcc has this warning. Other compilers get empty macros.
 */
#if defined(__GNUC__) && !defined(__clang__)
#define FD_LEAK_IS_FINE_BEGIN \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wanalyzer-fd-leak\"")
#define FD_LEAK_IS_FINE_END _Pragma("GCC diagnostic pop")
#else
#define FD_LEAK_IS_FINE_BEGIN
#define FD_LEAK_IS_FINE_END
#endif

#endif
