#ifndef ACSH_SIGNALS_H
#define ACSH_SIGNALS_H

#include <stdbool.h>

/*
 * Ctrl+C sends SIGINT and Ctrl+\ sends SIGQUIT to every process in the
 * terminal's foreground group: the shell and whatever it's running. The
 * command should stop; an interactive shell shouldn't.
 *
 * An interactive shell ignores SIGQUIT, as bash does. SIGINT needs more
 * care, because Ctrl+C at the prompt should drop the line and show a new
 * prompt. A handler that only interrupts read() isn't enough: a Ctrl+C that
 * comes after the prompt is printed but before read() starts would be lost,
 * and the shell would sit in read() as if nothing happened. So SIGINT stays
 * blocked in the shell, and signals_wait_for_input() waits with pselect(),
 * which unblocks it only while waiting, in one step. A Ctrl+C that comes
 * early just stays pending until then.
 *
 * A script (not a terminal) keeps the defaults, so Ctrl+C stops it.
 */

/* Sets up the shell's own signals. Call once, for an interactive shell. */
void signals_init_interactive(void);

/* For an interactive shell's jobs: sets up the self-pipe (both ends at 10
 * or above, close-on-exec, non-blocking) and a SIGCHLD handler that only
 * writes a byte into it. While the prompt waits, a byte in the pipe means
 * a child ended or stopped: the pipe is emptied and hook(arg) is called to
 * collect it. False if the pipe can't be made. */
bool signals_watch_children(void (*hook)(void *), void *arg);

/* For `wait` at the prompt: sleeps until the self-pipe says a child ended
 * or stopped (true) or Ctrl+C is pressed (false), with SIGINT let through
 * only while it sleeps, like the prompt. The caller then asks waitpid.
 * A child that ended before the call left a byte in the pipe, so it's
 * never missed. Only with the pipe (signals_watch_children). */
bool signals_sleep_until_child(void);

/* Waits until fd has input (true) or Ctrl+C was pressed (false, errno
 * EINTR). Other errors return false with errno set. While it waits, it
 * collects children through the self-pipe if there is one. */
bool signals_wait_for_input(int fd);

/* Forgets a Ctrl+C that came while a command ran. The command got it and
 * stopped; it shouldn't also count as Ctrl+C at the next prompt. */
void signals_drop_pending(void);

/* True if Ctrl+C was pressed while the shell itself was busy, and forgets
 * it. A loop of built-ins like `while :; do :; done` never waits for a
 * child or for input, so it asks this on every round to stay stoppable. */
bool signals_take_interrupt(void);

/* In a child of an interactive shell, after fork: SIGINT, SIGQUIT, SIGCHLD
 * and the job control signals (SIGTSTP, SIGTTIN, SIGTTOU) back to their
 * defaults, and SIGINT unblocked. A blocked or ignored signal stays
 * that way across exec, so without this the command couldn't be stopped. A
 * script doesn't call this: signals that were already ignored when it
 * started (under nohup, say) stay ignored for its commands, as POSIX says. */
void signals_reset_in_child(void);

#endif
