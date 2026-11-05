/* Reading lines straight from a file descriptor. */
#include "check.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"

/* A pipe with text already written into it and the write end closed. */
static int pipe_with(const char *text)
{
    int fds[2];

    if (pipe(fds) != 0)
        return -1;
    if (write(fds[1], text, strlen(text)) != (ssize_t)strlen(text)) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    close(fds[1]);
    return fds[0];
}

static void test_lines_one_by_one(void)
{
    int fd = pipe_with("echo one\n\nlast without newline");
    char *line = NULL;
    size_t cap = 0;

    CHECK(fd >= 0);
    CHECK_INT_EQ(9, input_read_line(fd, &line, &cap));
    CHECK_STR_EQ("echo one\n", line);
    CHECK_INT_EQ(1, input_read_line(fd, &line, &cap));
    CHECK_STR_EQ("\n", line);
    CHECK_INT_EQ(20, input_read_line(fd, &line, &cap));
    CHECK_STR_EQ("last without newline", line);
    CHECK_INT_EQ(-1, input_read_line(fd, &line, &cap));
    CHECK_INT_EQ(0, errno);
    free(line);
    close(fd);
}

static void test_reads_no_further_than_the_line(void)
{
    /* The rest has to stay in the pipe for the commands the shell runs. */
    int fd = pipe_with("cat\nhello\n");
    char *line = NULL;
    size_t cap = 0;
    char rest[16] = { 0 };

    CHECK(fd >= 0);
    CHECK_INT_EQ(4, input_read_line(fd, &line, &cap));
    CHECK_INT_EQ(6, read(fd, rest, sizeof rest - 1));
    CHECK_STR_EQ("hello\n", rest);
    free(line);
    close(fd);
}

static void test_empty_input(void)
{
    int fd = pipe_with("");
    char *line = NULL;
    size_t cap = 0;

    CHECK(fd >= 0);
    CHECK_INT_EQ(-1, input_read_line(fd, &line, &cap));
    CHECK_INT_EQ(0, errno);
    CHECK(line == NULL);
    close(fd);
}

static void test_long_line_grows_the_buffer(void)
{
    static char text[10002];
    int fds[2];
    char *line = NULL;
    size_t cap = 0;

    memset(text, 'x', 10000);
    text[10000] = '\n';
    CHECK(pipe(fds) == 0);
    /* 10,001 bytes fit in a Linux pipe (64 KB), so this doesn't block. */
    CHECK_INT_EQ(10001, write(fds[1], text, 10001));
    close(fds[1]);
    CHECK_INT_EQ(10001, input_read_line(fds[0], &line, &cap));
    CHECK_STR_EQ(text, line);
    CHECK(cap > 10001);
    free(line);
    close(fds[0]);
}

static void test_read_error_sets_errno(void)
{
    char *line = NULL;
    size_t cap = 0;

    CHECK_INT_EQ(-1, input_read_line(-1, &line, &cap));
    CHECK_INT_EQ(EBADF, errno);
}

int main(void)
{
    RUN(test_lines_one_by_one);
    RUN(test_reads_no_further_than_the_line);
    RUN(test_empty_input);
    RUN(test_long_line_grows_the_buffer);
    RUN(test_read_error_sets_errno);
    return check_summary();
}
