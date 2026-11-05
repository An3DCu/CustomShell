/*
 * Lexer tests. Each line is turned into a short description: words in
 * brackets exactly as typed, operators as written, a file descriptor number
 * glued to its operator (2>), and the final end-of-line token left out.
 *
 *     echo hi>out.txt   ->   [echo] [hi] > [out.txt]
 */
#include "check.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "lexer.h"

static char described[4096];

static const char *describe(const char *line)
{
    struct token_list tokens;
    char err[128];
    size_t used = 0;

    described[0] = '\0';
    if (lex(line, &tokens, err, sizeof err) != LEX_OK) {
        snprintf(described, sizeof described, "error: %s", err);
        token_list_free(&tokens);
        return described;
    }
    for (size_t i = 0; i < tokens.len; i++) {
        const struct token *tok = &tokens.items[i];
        const char *sep = used ? " " : "";

        if (tok->type == TOK_END)
            break;
        if (tok->type == TOK_WORD)
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "%s[%s]", sep, tok->text);
        else if (tok->io_number >= 0)
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "%s%d%s", sep, tok->io_number, token_spelling(tok));
        else
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "%s%s", sep, token_spelling(tok));
    }
    token_list_free(&tokens);
    return described;
}

#define CHECK_LEX(line, want) CHECK_STR_EQ(want, describe(line))

static void test_words_and_blanks(void)
{
    CHECK_LEX("echo hi", "[echo] [hi]");
    CHECK_LEX("  echo \t  hi  \n", "[echo] [hi]");
    CHECK_LEX("ls", "[ls]");
}

static void test_empty_lines(void)
{
    CHECK_LEX("", "");
    CHECK_LEX("   ", "");
    CHECK_LEX("\n", "");
}

static void test_operators_need_no_spaces(void)
{
    /* The old shell turned this into the single argument "hi>out.txt". */
    CHECK_LEX("echo hi>out.txt", "[echo] [hi] > [out.txt]");
    CHECK_LEX("a|b", "[a] | [b]");
    CHECK_LEX("a|b|c", "[a] | [b] | [c]");
    CHECK_LEX("sort<in>out", "[sort] < [in] > [out]");
    CHECK_LEX("echo x>>log", "[echo] [x] >> [log]");
}

static void test_longest_operator_wins(void)
{
    CHECK_LEX("a||b", "[a] || [b]");
    CHECK_LEX("a&&b", "[a] && [b]");
    CHECK_LEX("a&b;c", "[a] & [b] ; [c]");
    CHECK_LEX(">>>", ">> >");
    CHECK_LEX("|||", "|| |");
    CHECK_LEX("<&3 >&2", "<& [3] >& [2]");
}

static void test_fd_numbers(void)
{
    CHECK_LEX("cmd 2>err", "[cmd] 2> [err]");
    CHECK_LEX("cmd 2>&1", "[cmd] 2>& [1]");
    CHECK_LEX("cmd 2>>log", "[cmd] 2>> [log]");
    CHECK_LEX("cat 0<in", "[cat] 0< [in]");
    CHECK_LEX("cmd 12>f", "[cmd] 12> [f]");
    CHECK_LEX("2>f cmd", "2> [f] [cmd]");
}

static void test_digits_that_are_just_words(void)
{
    CHECK_LEX("echo 2 >f", "[echo] [2] > [f]");   /* a space in between */
    CHECK_LEX("echo a2>f", "[echo] [a2] > [f]");  /* not only digits */
    CHECK_LEX("echo \"2\">f", "[echo] [\"2\"] > [f]"); /* quoted */
    CHECK_LEX("echo 2", "[echo] [2]");
}

static void test_huge_fd_number(void)
{
    struct token_list tokens;
    char err[128];

    CHECK(lex("cmd 99999999999999999999>f", &tokens, err, sizeof err) == LEX_OK);
    CHECK_INT_EQ(INT_MAX, tokens.items[1].io_number);
    token_list_free(&tokens);
}

static void test_quotes_stay_in_the_word(void)
{
    CHECK_LEX("echo 'a b' \"c d\"", "[echo] ['a b'] [\"c d\"]");
    CHECK_LEX("a\"b c\"'d'", "[a\"b c\"'d']");
    CHECK_LEX("''", "['']");
    CHECK_LEX("\"\"", "[\"\"]");
}

static void test_quoted_operators_are_words(void)
{
    CHECK_LEX("echo '|' \"a>b\" 'x;y'", "[echo] ['|'] [\"a>b\"] ['x;y']");
    CHECK_LEX("echo \\| \\>f", "[echo] [\\|] [\\>f]");
    CHECK_LEX("echo 'a'|wc", "[echo] ['a'] | [wc]");
}

static void test_backslashes(void)
{
    CHECK_LEX("echo a\\ b", "[echo] [a\\ b]");
    CHECK_LEX("echo \"a\\\"b\"", "[echo] [\"a\\\"b\"]"); /* \" doesn't close */
    CHECK_LEX("echo \"a\\\\\"", "[echo] [\"a\\\\\"]");   /* \\ then the closing " */
    CHECK_LEX("echo 'a\\'", "[echo] ['a\\']");          /* no escapes in '...' */
}

static void test_unfinished_lines(void)
{
    CHECK_LEX("echo 'abc", "error: unclosed single quote");
    CHECK_LEX("echo \"abc", "error: unclosed double quote");
    CHECK_LEX("echo \"abc\\\"", "error: unclosed double quote");
    CHECK_LEX("echo 'a'\"", "error: unclosed double quote");
    CHECK_LEX("echo abc\\", "error: line ends with a backslash");
    CHECK_LEX("echo abc\\\n", "error: line ends with a backslash");
}

static void test_error_leaves_list_empty(void)
{
    struct token_list tokens;
    char err[128];

    CHECK(lex("one two 'three", &tokens, err, sizeof err) == LEX_INCOMPLETE);
    CHECK_INT_EQ(0, tokens.len);
    CHECK(tokens.items == NULL);
}

static void test_positions(void)
{
    struct token_list tokens;
    char err[128];

    CHECK(lex("echo hi>out", &tokens, err, sizeof err) == LEX_OK);
    CHECK_INT_EQ(5, tokens.len);
    CHECK_INT_EQ(0, tokens.items[0].pos);
    CHECK_INT_EQ(5, tokens.items[1].pos);
    CHECK_INT_EQ(7, tokens.items[2].pos);
    CHECK_INT_EQ(8, tokens.items[3].pos);
    CHECK_INT_EQ(11, tokens.items[4].pos);
    CHECK(tokens.items[4].type == TOK_END);
    token_list_free(&tokens);
}

static void test_long_line(void)
{
    static char line[5000 * 4 + 1]; /* + 1 for the final 0 */
    struct token_list tokens;
    char err[128];
    size_t n = 0;

    for (int i = 0; i < 5000; i++) {
        memcpy(line + n, "ab |", 4);
        n += 4;
    }
    line[n] = '\0';
    CHECK(lex(line, &tokens, err, sizeof err) == LEX_OK);
    CHECK_INT_EQ(10001, tokens.len); /* 5000 words, 5000 pipes, the end */
    token_list_free(&tokens);
}

int main(void)
{
    RUN(test_words_and_blanks);
    RUN(test_empty_lines);
    RUN(test_operators_need_no_spaces);
    RUN(test_longest_operator_wins);
    RUN(test_fd_numbers);
    RUN(test_digits_that_are_just_words);
    RUN(test_huge_fd_number);
    RUN(test_quotes_stay_in_the_word);
    RUN(test_quoted_operators_are_words);
    RUN(test_backslashes);
    RUN(test_unfinished_lines);
    RUN(test_error_leaves_list_empty);
    RUN(test_positions);
    RUN(test_long_line);
    return check_summary();
}
