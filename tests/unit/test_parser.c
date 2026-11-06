/*
 * Parser tests. A parsed list is described on one line: commands joined by
 * " | ", each with its words as typed, then its redirections with the fd
 * spelled out (> becomes 1>, < becomes 0<), in the order they were written.
 * Pipelines are joined by " && " or " || ", the list's items by " ; ", and
 * a negated pipeline starts with "! ".
 *
 *     ls -l | wc -l > n.txt; echo x   ->   ls -l | wc -l 1>n.txt ; echo x
 *
 * Errors come back as "syntax: <message>" or "incomplete: <message>".
 */
#include "check.h"

#include <stdio.h>
#include <string.h>

#include "parser.h"

static char described[1 << 16];

static const char *redir_op(enum redir_kind kind)
{
    switch (kind) {
    case REDIR_IN:      return "<";
    case REDIR_OUT:     return ">";
    case REDIR_APPEND:  return ">>";
    case REDIR_DUP_IN:  return "<&";
    case REDIR_DUP_OUT: return ">&";
    }
    return "?";
}

static size_t describe_pipeline(const struct pipeline *pl, size_t used)
{
    if (pl->negate)
        used += (size_t)snprintf(described + used, sizeof described - used, "! ");
    for (size_t c = 0; c < pl->ncommands; c++) {
        const struct command *cmd = &pl->commands[c];
        const char *sep = "";

        if (c > 0)
            used += (size_t)snprintf(described + used, sizeof described - used, " | ");
        for (size_t a = 0; a < cmd->nassigns; a++, sep = " ")
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "%sset(%s)", sep, cmd->assigns[a]);
        for (size_t w = 0; w < cmd->nwords; w++, sep = " ")
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "%s%s", sep, cmd->words[w]);
        for (size_t r = 0; r < cmd->nredirs; r++, sep = " ")
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "%s%d%s%s", sep, cmd->redirs[r].fd,
                                     redir_op(cmd->redirs[r].kind), cmd->redirs[r].target);
    }
    return used;
}

static const char *describe(const char *line)
{
    struct list *list;
    char err[128];
    size_t used = 0;

    described[0] = '\0';
    switch (parse_program(line, &list, err, sizeof err)) {
    case PARSE_EMPTY:
        return "empty";
    case PARSE_INCOMPLETE:
        snprintf(described, sizeof described, "incomplete: %s", err);
        return described;
    case PARSE_SYNTAX:
        snprintf(described, sizeof described, "syntax: %s", err);
        return described;
    case PARSE_OK:
        break;
    }
    for (size_t i = 0; i < list->nitems; i++) {
        const struct and_or *ao = &list->items[i];

        if (i > 0)
            used += (size_t)snprintf(described + used, sizeof described - used, " ; ");
        for (size_t j = 0; j < ao->npipelines; j++) {
            const struct pipeline *pl = &ao->pipelines[j];

            if (pl->run_if == RUN_IF_OK)
                used += (size_t)snprintf(described + used, sizeof described - used, " && ");
            else if (pl->run_if == RUN_IF_FAILED)
                used += (size_t)snprintf(described + used, sizeof described - used, " || ");
            used = describe_pipeline(pl, used);
        }
    }
    list_free(list);
    return described;
}

#define CHECK_PARSE(line, want) CHECK_STR_EQ(want, describe(line))

static void test_simple_commands(void)
{
    CHECK_PARSE("ls", "ls");
    CHECK_PARSE("ls -l /tmp", "ls -l /tmp");
    CHECK_PARSE("  echo   hi  \n", "echo hi");
}

static void test_words_stay_as_typed(void)
{
    CHECK_PARSE("echo \"a b\" 'c'", "echo \"a b\" 'c'");
}

static void test_pipelines(void)
{
    CHECK_PARSE("ls | wc", "ls | wc");
    CHECK_PARSE("ls -l | grep c | wc -l > n.txt", "ls -l | grep c | wc -l 1>n.txt");
    CHECK_PARSE("a|b|c|d", "a | b | c | d");
}

static void test_redirections(void)
{
    CHECK_PARSE("echo hi>out", "echo hi 1>out");
    CHECK_PARSE("sort < in > out", "sort 0<in 1>out");
    CHECK_PARSE("echo x >> log", "echo x 1>>log");
    CHECK_PARSE("cmd 2> err", "cmd 2>err");
    CHECK_PARSE("cmd 2>> err", "cmd 2>>err");
    CHECK_PARSE("cat <&3", "cat 0<&3");
    CHECK_PARSE("echo hi >&2", "echo hi 1>&2");
    CHECK_PARSE("cmd 2>&-", "cmd 2>&-");
}

static void test_redirection_order_is_kept(void)
{
    /* These two mean different things, so the order must survive. */
    CHECK_PARSE("cmd 2>&1 >f", "cmd 2>&1 1>f");
    CHECK_PARSE("cmd >f 2>&1", "cmd 1>f 2>&1");
}

static void test_redirections_anywhere_in_a_command(void)
{
    CHECK_PARSE("> out echo hi", "echo hi 1>out");
    CHECK_PARSE("echo > out hi", "echo hi 1>out");
}

static void test_assignments(void)
{
    CHECK_PARSE("X=1", "set(X=1)");
    CHECK_PARSE("X=1 Y=\"a b\" env", "set(X=1) set(Y=\"a b\") env");
    CHECK_PARSE("X= cmd", "set(X=) cmd");
    CHECK_PARSE("_a1=x", "set(_a1=x)");
    CHECK_PARSE("X=1 > f Y=2 cmd", "set(X=1) set(Y=2) cmd 1>f");
}

static void test_words_that_are_not_assignments(void)
{
    CHECK_PARSE("echo X=1", "echo X=1");        /* after the command name */
    CHECK_PARSE("\"X\"=1", "\"X\"=1");          /* a quoted name */
    CHECK_PARSE("1X=1", "1X=1");
    CHECK_PARSE("=1", "=1");
    CHECK_PARSE("X-Y=1", "X-Y=1");
    CHECK_PARSE("X=1 | Y=2 cmd", "set(X=1) | set(Y=2) cmd");
}

static void test_command_with_only_redirections(void)
{
    CHECK_PARSE("> f", "1>f");
    CHECK_PARSE("< in", "0<in");
}

static void test_empty_lines(void)
{
    CHECK_PARSE("", "empty");
    CHECK_PARSE("   \t\n", "empty");
    CHECK_PARSE("\n\n\n", "empty");
    CHECK_PARSE("# just a comment\n", "empty");
    CHECK_PARSE("  # indented\n\n# two\n", "empty");
}

static void test_syntax_errors_name_the_token(void)
{
    CHECK_PARSE("| ls", "syntax: syntax error near '|'");
    CHECK_PARSE("ls | | wc", "syntax: syntax error near '|'");
    CHECK_PARSE("ls > | wc", "syntax: syntax error near '|'");
    CHECK_PARSE("ls >> >> f", "syntax: syntax error near '>>'");
    CHECK_PARSE("ls >", "syntax: syntax error near end of line");
    CHECK_PARSE("ls 2>", "syntax: syntax error near end of line");
}

static void test_lists(void)
{
    CHECK_PARSE("ls ; pwd", "ls ; pwd");
    CHECK_PARSE("ls;pwd;", "ls ; pwd");
    CHECK_PARSE("a\nb\n", "a ; b");
    CHECK_PARSE("\n\na\n\n\nb", "a ; b");
    CHECK_PARSE("a; \n b", "a ; b");
}

static void test_and_or(void)
{
    CHECK_PARSE("make && ./run", "make && ./run");
    CHECK_PARSE("a || b && c", "a || b && c");
    CHECK_PARSE("a | b && c | d; e", "a | b && c | d ; e");
    CHECK_PARSE("a &&\n\n b", "a && b");
    CHECK_PARSE("a ||\n b", "a || b");
    CHECK_PARSE("a |\n b", "a | b");
}

static void test_negation(void)
{
    CHECK_PARSE("! false", "! false");
    CHECK_PARSE("! a | b && ! c", "! a | b && ! c");
    CHECK_PARSE("echo !", "echo !");
    CHECK_PARSE("'!' x", "'!' x");          /* quoted: just a command name */
    CHECK_PARSE("!x", "!x");
    CHECK_PARSE("!", "syntax: syntax error near end of line");
}

static void test_comments(void)
{
    CHECK_PARSE("echo a # b c", "echo a");
    CHECK_PARSE("echo a#b", "echo a#b");
    CHECK_PARSE("echo '#a' \\#b", "echo '#a' \\#b");
    CHECK_PARSE("echo a # ; rm x\necho b", "echo a ; echo b");
    CHECK_PARSE("echo a # \\\necho b", "echo a ; echo b"); /* \ in a comment joins nothing */
}

static void test_list_syntax_errors(void)
{
    CHECK_PARSE("; ls", "syntax: syntax error near ';'");
    CHECK_PARSE("ls ;; pwd", "syntax: syntax error near ';'");
    CHECK_PARSE("ls && && pwd", "syntax: syntax error near '&&'");
    CHECK_PARSE("ls | ; pwd", "syntax: syntax error near ';'");
    CHECK_PARSE("&& ls", "syntax: syntax error near '&&'");
    CHECK_PARSE("ls > ; pwd", "syntax: syntax error near ';'");
    CHECK_PARSE("ls >\npwd", "syntax: syntax error near end of line");
}

static void test_not_supported_yet(void)
{
    CHECK_PARSE("sleep 1 &", "syntax: '&' isn't supported yet");
    CHECK_PARSE("sleep 1 & echo", "syntax: '&' isn't supported yet");
}

static void test_unfinished_lines(void)
{
    CHECK_PARSE("ls |", "incomplete: line ends with '|'");
    CHECK_PARSE("ls | wc |  \n", "incomplete: line ends with '|'");
    CHECK_PARSE("ls &&\n", "incomplete: line ends with '&&'");
    CHECK_PARSE("ls ||\n\n", "incomplete: line ends with '||'");
    CHECK_PARSE("echo 'abc", "incomplete: unclosed single quote");
    CHECK_PARSE("echo 'abc\ndef", "incomplete: unclosed single quote");
    CHECK_PARSE("echo abc\\", "incomplete: line ends with a backslash");
    CHECK_PARSE("echo abc\\\n", "incomplete: line ends with a backslash");
}

static void test_lines_joined_by_a_backslash(void)
{
    CHECK_PARSE("echo a \\\nb\n", "echo a b");
    CHECK_PARSE("echo a\\\nb\n", "echo a\\\nb");  /* one word; expansion drops the \\newline */
    CHECK_PARSE("echo 'a\nb'\n", "echo 'a\nb'");
}

static void test_words_are_null_terminated(void)
{
    struct list *list;
    const struct pipeline *pl;
    char err[128];

    CHECK(parse_program("echo a b", &list, err, sizeof err) == PARSE_OK);
    pl = &list->items[0].pipelines[0];
    CHECK_INT_EQ(1, pl->ncommands);
    CHECK_INT_EQ(3, pl->commands[0].nwords);
    CHECK(pl->commands[0].words[3] == NULL);
    list_free(list);
}

static void test_out_is_null_on_error(void)
{
    struct list dummy;
    struct list *list = &dummy; /* anything but NULL, to see it reset */
    char err[128];

    CHECK(parse_program("| x", &list, err, sizeof err) == PARSE_SYNTAX);
    CHECK(list == NULL);
    list = &dummy;
    CHECK(parse_program("", &list, err, sizeof err) == PARSE_EMPTY);
    CHECK(list == NULL);
    list = &dummy;
    CHECK(parse_program("a |", &list, err, sizeof err) == PARSE_INCOMPLETE);
    CHECK(list == NULL);
}

static void test_long_pipelines_and_commands(void)
{
    static char line[1 << 15];
    struct list *list;
    char err[128];
    size_t n = 0;

    for (int i = 0; i < 1000; i++) {
        memcpy(line + n, "cat | ", 6);
        n += 6;
    }
    memcpy(line + n, "wc", 3);
    CHECK(parse_program(line, &list, err, sizeof err) == PARSE_OK);
    CHECK_INT_EQ(1001, list->items[0].pipelines[0].ncommands);
    list_free(list);

    n = 0;
    memcpy(line + n, "echo", 4);
    n += 4;
    for (int i = 0; i < 2000; i++) {
        memcpy(line + n, " w", 2);
        n += 2;
    }
    line[n] = '\0';
    CHECK(parse_program(line, &list, err, sizeof err) == PARSE_OK);
    CHECK_INT_EQ(2001, list->items[0].pipelines[0].commands[0].nwords);
    list_free(list);

    n = 0;
    for (int i = 0; i < 1000; i++) {
        memcpy(line + n, "a && b; ", 8);
        n += 8;
    }
    line[n] = '\0';
    CHECK(parse_program(line, &list, err, sizeof err) == PARSE_OK);
    CHECK_INT_EQ(1000, list->nitems);
    CHECK_INT_EQ(2, list->items[999].npipelines);
    list_free(list);
}

int main(void)
{
    RUN(test_simple_commands);
    RUN(test_words_stay_as_typed);
    RUN(test_pipelines);
    RUN(test_redirections);
    RUN(test_redirection_order_is_kept);
    RUN(test_redirections_anywhere_in_a_command);
    RUN(test_assignments);
    RUN(test_words_that_are_not_assignments);
    RUN(test_command_with_only_redirections);
    RUN(test_empty_lines);
    RUN(test_syntax_errors_name_the_token);
    RUN(test_lists);
    RUN(test_and_or);
    RUN(test_negation);
    RUN(test_comments);
    RUN(test_list_syntax_errors);
    RUN(test_not_supported_yet);
    RUN(test_unfinished_lines);
    RUN(test_lines_joined_by_a_backslash);
    RUN(test_words_are_null_terminated);
    RUN(test_out_is_null_on_error);
    RUN(test_long_pipelines_and_commands);
    return check_summary();
}
