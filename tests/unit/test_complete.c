/*
 * Tab completion's parts that don't need a terminal: where the word starts,
 * whether it's where a command goes, and how a name is quoted.
 */
#include "check.h"

#include <stdlib.h>
#include <string.h>

#include "complete.h"

static size_t start_of(const char *text)
{
    return complete_word_start(text, strlen(text));
}

static void test_word_start(void)
{
    CHECK_INT_EQ(0, (int)start_of("ech"));
    CHECK_INT_EQ(4, (int)start_of("cat no"));
    CHECK_INT_EQ(4, (int)start_of("cat "));
    CHECK_INT_EQ(6, (int)start_of("ls -l|gr"));
    CHECK_INT_EQ(4, (int)start_of("cat 'my no"));   /* the blank is inside quotes */
    CHECK_INT_EQ(4, (int)start_of("cat my\\ no"));  /* and this one is escaped */
    CHECK_INT_EQ(7, (int)start_of("echo a>ou"));
}

static void test_command_position(void)
{
    CHECK(complete_is_command("ech", 0));
    CHECK(complete_is_command("  ech", 2));
    CHECK(!complete_is_command("cat no", 4));
    CHECK(complete_is_command("ls | gr", 5));
    CHECK(complete_is_command("ls|gr", 3));
    CHECK(complete_is_command("true && ec", 8));
    CHECK(complete_is_command("false || ec", 9));
    CHECK(complete_is_command("a; ec", 3));
    CHECK(complete_is_command("sleep 1 & ec", 10));
    CHECK(!complete_is_command("echo a >ou", 8));
}

static void test_quote(void)
{
    char *q;

    q = complete_quote("notes.txt");
    CHECK_STR_EQ("notes.txt", q);
    free(q);
    q = complete_quote("src/main.c");
    CHECK_STR_EQ("src/main.c", q);
    free(q);
    q = complete_quote("my notes.txt");
    CHECK_STR_EQ("'my notes.txt'", q);
    free(q);
    q = complete_quote("it's");
    CHECK_STR_EQ("'it'\\''s'", q);
    free(q);
    q = complete_quote("a*b");
    CHECK_STR_EQ("'a*b'", q);
    free(q);
    q = complete_quote("$HOME");
    CHECK_STR_EQ("'$HOME'", q);
    free(q);
}

int main(void)
{
    RUN(test_word_start);
    RUN(test_command_position);
    RUN(test_quote);
    return check_summary();
}
