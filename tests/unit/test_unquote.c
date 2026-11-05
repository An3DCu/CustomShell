/*
 * Quote removal. Every expected value here was also checked against dash,
 * the POSIX shell Ubuntu uses as /bin/sh (printf '[%s]' <word>).
 */
#include "check.h"

#include <stdlib.h>

#include "lexer.h"

static void check_unquote(const char *file, int line, const char *word, const char *want)
{
    char *got = word_unquote(word);

    if (!check_str_same(want, got))
        check_fail(file, line, "unquote(%s): want \"%s\", got \"%s\"", word, want, got);
    free(got);
}

#define CHECK_UNQUOTE(word, want) check_unquote(__FILE__, __LINE__, word, want)

static void test_plain_words(void)
{
    CHECK_UNQUOTE("echo", "echo");
    CHECK_UNQUOTE("a-b_c.txt", "a-b_c.txt");
}

static void test_empty_quotes_make_an_empty_word(void)
{
    CHECK_UNQUOTE("''", "");
    CHECK_UNQUOTE("\"\"", "");
    CHECK_UNQUOTE("''\"\"", "");
}

static void test_single_quotes(void)
{
    CHECK_UNQUOTE("'a b'", "a b");
    CHECK_UNQUOTE("'$HOME'", "$HOME");
    CHECK_UNQUOTE("'a\\nb'", "a\\nb");   /* no escapes at all */
    CHECK_UNQUOTE("'\\'", "\\");
    CHECK_UNQUOTE("'\"'", "\"");
}

static void test_double_quotes(void)
{
    CHECK_UNQUOTE("\"a b\"", "a b");
    CHECK_UNQUOTE("\"it's\"", "it's");
    CHECK_UNQUOTE("\"a\\\"b\"", "a\"b");  /* \" */
    CHECK_UNQUOTE("\"a\\\\b\"", "a\\b");  /* \\ */
    CHECK_UNQUOTE("\"\\$x\"", "$x");      /* \$ */
    CHECK_UNQUOTE("\"\\`\"", "`");        /* \` */
}

static void test_backslash_stays_in_double_quotes_before_others(void)
{
    CHECK_UNQUOTE("\"\\a\"", "\\a");
    CHECK_UNQUOTE("\"\\n\"", "\\n");
    CHECK_UNQUOTE("\"a\\'b\"", "a\\'b");
}

static void test_backslash_outside_quotes(void)
{
    CHECK_UNQUOTE("a\\ b", "a b");
    CHECK_UNQUOTE("\\|", "|");
    CHECK_UNQUOTE("\\\\", "\\");
    CHECK_UNQUOTE("\\a", "a");
    CHECK_UNQUOTE("\\'", "'");
}

static void test_backslash_newline_disappears(void)
{
    CHECK_UNQUOTE("a\\\nb", "ab");
    CHECK_UNQUOTE("\"a\\\nb\"", "ab");
}

static void test_mixed_pieces_join_into_one_word(void)
{
    CHECK_UNQUOTE("a\"b c\"'d'", "ab cd");
    CHECK_UNQUOTE("'it'\\''s'", "it's");
    CHECK_UNQUOTE("\"$\"'$'\\$", "$$$");
}

static void test_unclosed_input_does_not_crash(void)
{
    CHECK_UNQUOTE("'abc", "abc");
    CHECK_UNQUOTE("\"abc", "abc");
    CHECK_UNQUOTE("abc\\", "abc\\");
}

int main(void)
{
    RUN(test_plain_words);
    RUN(test_empty_quotes_make_an_empty_word);
    RUN(test_single_quotes);
    RUN(test_double_quotes);
    RUN(test_backslash_stays_in_double_quotes_before_others);
    RUN(test_backslash_outside_quotes);
    RUN(test_backslash_newline_disappears);
    RUN(test_mixed_pieces_join_into_one_word);
    RUN(test_unclosed_input_does_not_crash);
    return check_summary();
}
