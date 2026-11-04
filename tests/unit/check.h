/*
 * check: my small unit test harness.
 *
 * A test is a void function. main() runs each with RUN(name) and returns
 * check_summary(), which is non-zero if anything failed, so make stops.
 * A failed CHECK prints where and why, and the test keeps going, so one run
 * shows every failure in it.
 *
 *     static void test_adds(void) { CHECK_INT_EQ(4, 2 + 2); }
 *     int main(void) { RUN(test_adds); return check_summary(); }
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdbool.h>
#include <stddef.h> /* NULL, which most tests compare against */

void check_run(const char *name, void (*test)(void));
int check_summary(void);

void check_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The comparisons behind the macros, so the harness can test them too. */
bool check_str_same(const char *a, const char *b);

#define RUN(test) check_run(#test, test)

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond))                                                       \
            check_fail(__FILE__, __LINE__, "%s", #cond);                   \
    } while (0)

#define CHECK_INT_EQ(want, got)                                            \
    do {                                                                   \
        long long want_ = (want), got_ = (got);                            \
        if (want_ != got_)                                                 \
            check_fail(__FILE__, __LINE__, "%s: want %lld, got %lld",      \
                       #got, want_, got_);                                 \
    } while (0)

/* NULL only equals NULL; both sides are printed with quotes. */
#define CHECK_STR_EQ(want, got)                                            \
    do {                                                                   \
        const char *want_ = (want), *got_ = (got);                         \
        if (!check_str_same(want_, got_))                                  \
            check_fail(__FILE__, __LINE__, "%s: want %s%s%s, got %s%s%s",  \
                       #got, want_ ? "\"" : "", want_ ? want_ : "NULL",    \
                       want_ ? "\"" : "", got_ ? "\"" : "",                \
                       got_ ? got_ : "NULL", got_ ? "\"" : "");            \
    } while (0)

#endif
