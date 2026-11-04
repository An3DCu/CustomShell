#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *current_test;
static int current_failures;
static int tests_run;
static int tests_failed;

void check_run(const char *name, void (*test)(void))
{
    current_test = name;
    current_failures = 0;
    test();
    tests_run++;
    if (current_failures > 0)
        tests_failed++;
}

int check_summary(void)
{
    if (tests_failed > 0) {
        printf("%d of %d tests failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("%d tests passed\n", tests_run);
    return 0;
}

void check_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    current_failures++;
    printf("FAIL %s (%s:%d): ", current_test, file, line);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

bool check_str_same(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
}
