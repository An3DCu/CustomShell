/*
 * Tests for the harness itself. They cover the comparisons the macros rely
 * on. They also check that passing checks don't count as failures. A
 * failing check can't be tested here without making this file fail. That
 * part is checked by hand: break a CHECK and run make test-unit.
 */
#include "check.h"

static void test_same_strings(void)
{
    char built[] = { 'a', 'c', 's', 'h', '\0' };

    CHECK(check_str_same("acsh", "acsh"));
    CHECK(check_str_same("acsh", built)); /* same text, different memory */
    CHECK(check_str_same("", ""));
}

static void test_different_strings(void)
{
    CHECK(!check_str_same("acsh", "acsh "));
    CHECK(!check_str_same("acsh", "ACSH"));
    CHECK(!check_str_same("", "a"));
}

static void test_null_only_equals_null(void)
{
    CHECK(check_str_same(NULL, NULL));
    CHECK(!check_str_same(NULL, ""));
    CHECK(!check_str_same("", NULL));
}

static void test_macros_pass(void)
{
    CHECK(1 + 1 == 2);
    CHECK_INT_EQ(4, 2 + 2);
    CHECK_INT_EQ(-1, -1);
    CHECK_STR_EQ("pipe", "pipe");
    CHECK_STR_EQ(NULL, NULL);
}

int main(void)
{
    RUN(test_same_strings);
    RUN(test_different_strings);
    RUN(test_null_only_equals_null);
    RUN(test_macros_pass);
    return check_summary();
}
