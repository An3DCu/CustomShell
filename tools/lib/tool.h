#ifndef TOOL_H
#define TOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * What my small tools in tools/ share. Error messages come in the same
 * format as the shell's ("cat: notes.txt: No such file or directory").
 * Reading and writing go through plain read() and write(). Those can do
 * less than asked.
 */

/* The name used in messages. Call first thing in main. */
void tool_init(const char *name);

const char *tool_name(void);

/* Prints "name: what: <strerror(err)>". */
void tool_error(const char *what, int err);

/* Prints "name: <message>" for problems that have no errno. */
void tool_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Opens a file for reading; "-" means stdin. Returns the fd, or -1 with
 * errno set. */
int tool_open_input(const char *path);

/* Closes fd unless it's stdin. */
void tool_close_input(int fd);

/* read() that tries again when a signal interrupts it. */
ssize_t tool_read(int fd, void *buf, size_t len);

/* Writes all len bytes, however many write() calls it takes. Returns false
 * with errno set on an error. */
bool tool_write_all(int fd, const void *buf, size_t len);

/* Parses a whole decimal number of 0 or more. Returns false for anything
 * else ("-1", "3x", "", too big). */
bool tool_parse_count(const char *s, unsigned long long *out);

#endif
