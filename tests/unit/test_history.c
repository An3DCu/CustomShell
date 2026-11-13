/*
 * History in memory: which lines are kept, how many, and which file is used.
 * The file itself is tested end to end.
 */
#include "check.h"

#include <stdlib.h>

#include "history.h"

static void test_size(void)
{
    CHECK_INT_EQ(1000, (int)history_size(NULL));
    CHECK_INT_EQ(5, (int)history_size("5"));
    CHECK_INT_EQ(0, (int)history_size("0"));
    CHECK_INT_EQ(1000, (int)history_size("-3"));
    CHECK_INT_EQ(1000, (int)history_size("lots"));
    CHECK_INT_EQ(1000, (int)history_size("5x"));
}

static void test_file(void)
{
    char *f;

    f = history_file("/tmp/h", "/home/me");
    CHECK_STR_EQ("/tmp/h", f);
    free(f);
    f = history_file(NULL, "/home/me");
    CHECK_STR_EQ("/home/me/.acsh_history", f);
    free(f);
    CHECK(history_file("", "/home/me") == NULL); /* HISTFILE set to nothing */
    CHECK(history_file(NULL, NULL) == NULL);
    CHECK(history_file(NULL, "") == NULL);
}

static void test_what_is_kept(void)
{
    struct history h;

    history_init(&h, NULL, 3);
    history_add(&h, "a");
    history_add(&h, "a");   /* the same as the one before */
    history_add(&h, "");
    history_add(&h, " \t ");
    history_add(&h, "b");
    history_add(&h, "a");   /* not the one before: kept */
    CHECK_INT_EQ(3, (int)h.len);
    history_add(&h, "c");   /* over the size: the oldest goes */
    CHECK_INT_EQ(3, (int)h.len);
    CHECK_STR_EQ("b", h.lines[0]);
    CHECK_STR_EQ("c", h.lines[2]);
    history_settings(&h, NULL, 1);
    CHECK_INT_EQ(1, (int)h.len);
    CHECK_STR_EQ("c", h.lines[0]);
    history_clear(&h);
    CHECK_INT_EQ(0, (int)h.len);
    history_free(&h);
}

static void test_size_zero_keeps_nothing(void)
{
    struct history h;

    history_init(&h, NULL, 0);
    history_add(&h, "a");
    CHECK_INT_EQ(0, (int)h.len);
    history_free(&h);
}

int main(void)
{
    RUN(test_size);
    RUN(test_file);
    RUN(test_what_is_kept);
    RUN(test_size_zero_keeps_nothing);
    return check_summary();
}
