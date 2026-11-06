/*
 * The variable table: what's set, what's exported, and the environment a
 * child gets.
 */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vars.h"

/* The child's environment as one string, "A=1 B=2", in table order. */
static const char *environment(const struct vars *v)
{
    static char buf[1024];
    char **env = vars_environ(v);
    size_t used = 0;

    buf[0] = '\0';
    for (size_t i = 0; env[i] != NULL; i++)
        used += (size_t)snprintf(buf + used, sizeof buf - used, "%s%s",
                                 i > 0 ? " " : "", env[i]);
    vars_free_environ(env);
    return buf;
}

static void test_names(void)
{
    CHECK(var_name_valid("X"));
    CHECK(var_name_valid("_"));
    CHECK(var_name_valid("a_1"));
    CHECK(!var_name_valid(""));
    CHECK(!var_name_valid("1a"));
    CHECK(!var_name_valid("a-b"));
    CHECK(!var_name_valid("a b"));
    CHECK_INT_EQ(3, var_name_len("abc=1"));
    CHECK_INT_EQ(0, var_name_len("=1"));
    CHECK_INT_EQ(2, var_name_len("ab$c"));
}

static void test_import_marks_everything_exported(void)
{
    char *env[] = { "HOME=/home/user", "EMPTY=", "A=b=c", NULL };
    struct vars v;

    vars_init(&v, env);
    CHECK_STR_EQ("/home/user", vars_get(&v, "HOME"));
    CHECK_STR_EQ("", vars_get(&v, "EMPTY"));
    CHECK_STR_EQ("b=c", vars_get(&v, "A")); /* only the first = splits */
    CHECK(vars_is_exported(&v, "HOME"));
    CHECK_STR_EQ("HOME=/home/user EMPTY= A=b=c", environment(&v));
    vars_free(&v);
}

static void test_import_skips_broken_entries(void)
{
    char *env[] = { "NOEQUALS", "=nameless", "X=1", "X=2", NULL };
    struct vars v;

    vars_init(&v, env);
    CHECK_STR_EQ("1", vars_get(&v, "X")); /* the first copy wins, like getenv */
    CHECK_STR_EQ("X=1", environment(&v));
    vars_free(&v);
}

static void test_odd_names_are_passed_on(void)
{
    char *env[] = { "a-b=1", NULL };
    struct vars v;

    vars_init(&v, env);
    CHECK_STR_EQ("a-b=1", environment(&v));
    vars_free(&v);
}

static void test_shell_variables_stay_in_the_shell(void)
{
    struct vars v;

    vars_init(&v, NULL);
    vars_set(&v, "X", "1");
    CHECK_STR_EQ("1", vars_get(&v, "X"));
    CHECK(!vars_is_exported(&v, "X"));
    CHECK_STR_EQ("", environment(&v));
    vars_export(&v, "X");
    CHECK_STR_EQ("X=1", environment(&v));
    vars_set(&v, "X", "2"); /* still exported */
    CHECK_STR_EQ("X=2", environment(&v));
    vars_free(&v);
}

static void test_export_without_a_value(void)
{
    struct vars v;

    vars_init(&v, NULL);
    vars_export(&v, "X");
    CHECK(vars_exists(&v, "X"));
    CHECK(vars_get(&v, "X") == NULL);
    CHECK_STR_EQ("", environment(&v)); /* nothing to pass on yet */
    vars_set(&v, "X", "now");
    CHECK_STR_EQ("X=now", environment(&v));
    vars_free(&v);
}

static void test_unset(void)
{
    char *env[] = { "A=1", "B=2", "C=3", NULL };
    struct vars v;

    vars_init(&v, env);
    vars_unset(&v, "B");
    vars_unset(&v, "NOPE");
    CHECK(vars_get(&v, "B") == NULL);
    CHECK(!vars_exists(&v, "B"));
    CHECK_STR_EQ("A=1 C=3", environment(&v)); /* the order is kept */
    vars_set(&v, "B", "again");
    CHECK(!vars_is_exported(&v, "B"));        /* a new variable now */
    vars_free(&v);
}

static void test_set_to_its_own_value(void)
{
    struct vars v;

    vars_init(&v, NULL);
    vars_set(&v, "X", "abc");
    vars_set(&v, "X", vars_get(&v, "X") + 1); /* points into the old value */
    CHECK_STR_EQ("bc", vars_get(&v, "X"));
    vars_free(&v);
}

static void test_sorted(void)
{
    char *env[] = { "B=1", "C=1", "A=1", NULL };
    struct vars v;
    size_t n;
    const struct var **list;

    vars_init(&v, env);
    list = vars_sorted(&v, &n);
    CHECK_INT_EQ(3, n);
    CHECK_STR_EQ("A", list[0]->name);
    CHECK_STR_EQ("B", list[1]->name);
    CHECK_STR_EQ("C", list[2]->name);
    free(list);
    vars_free(&v);
}

static void test_many_variables(void)
{
    struct vars v;
    char name[16];

    vars_init(&v, NULL);
    for (int i = 0; i < 1000; i++) {
        snprintf(name, sizeof name, "V%d", i);
        vars_set(&v, name, name);
    }
    CHECK_STR_EQ("V999", vars_get(&v, "V999"));
    CHECK_INT_EQ(1000, v.len);
    vars_free(&v);
}

int main(void)
{
    RUN(test_names);
    RUN(test_import_marks_everything_exported);
    RUN(test_import_skips_broken_entries);
    RUN(test_odd_names_are_passed_on);
    RUN(test_shell_variables_stay_in_the_shell);
    RUN(test_export_without_a_value);
    RUN(test_unset);
    RUN(test_set_to_its_own_value);
    RUN(test_sorted);
    RUN(test_many_variables);
    return check_summary();
}
