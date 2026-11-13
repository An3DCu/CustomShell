#ifndef ACSH_INPUT_H
#define ACSH_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Reads one line from fd into *line (grown as needed, like getline), newline
 * included if there was one. Returns its length, or -1 at the end of input
 * (errno 0) or on a read error (errno set).
 *
 * With interruptible set (the interactive shell), Ctrl+C also returns -1,
 * with errno EINTR, and whatever was read of the line is dropped. See
 * signals.h for why the waiting goes through pselect.
 *
 * It reads one byte at a time on purpose. A buffered reader takes more than
 * one line from a pipe, and whatever it takes is gone for the commands the
 * shell runs: in `printf 'cat\nhello\n' | acsh`, cat must get "hello".
 */
ssize_t input_read_line(int fd, char **line, size_t *cap, bool interruptible);

/* A zero byte is left out of the line, as bash and dash do, since it would
 * end the C string early. Call this after reading a line: the first time
 * anything was left out, it says `ignored a zero byte in the input`. */
struct shell;
void input_warn_zero(const struct shell *sh);

#endif
