/*
 * Parser tests. A pipeline is described on one line: commands joined by
 * " | ", each with its words as typed, then its redirections with the fd
 * spelled out (> becomes 1>, < becomes 0<), in the order they were written.
 *
 *     ls -l | wc -l > n.txt   ->   ls -l | wc -l 1>n.txt
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

static const char *describe(const char *line)
{
    struct pipeline *pl;
    char err[128];
    size_t used = 0;

    described[0] = '\0';
    switch (parse_line(line, &pl, err, sizeof err)) {
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
    pipeline_free(pl);
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

static void test_not_supported_yet(void)
{
    CHECK_PARSE("ls ; pwd", "syntax: ';' isn't supported yet");
    CHECK_PARSE("ls && pwd", "syntax: '&&' isn't supported yet");
    CHECK_PARSE("ls || pwd", "syntax: '||' isn't supported yet");
    CHECK_PARSE("sleep 1 &", "syntax: '&' isn't supported yet");
    CHECK_PARSE("; ls", "syntax: ';' isn't supported yet");
}

static void test_unfinished_lines(void)
{
    CHECK_PARSE("ls |", "incomplete: line ends with '|'");
    CHECK_PARSE("ls | wc |  \n", "incomplete: line ends with '|'");
    CHECK_PARSE("echo 'abc", "incomplete: unclosed single quote");
    CHECK_PARSE("echo abc\\", "incomplete: line ends with a backslash");
}

static void test_words_are_null_terminated(void)
{
    struct pipeline *pl;
    char err[128];

    CHECK(parse_line("echo a b", &pl, err, sizeof err) == PARSE_OK);
    CHECK_INT_EQ(1, pl->ncommands);
    CHECK_INT_EQ(3, pl->commands[0].nwords);
    CHECK(pl->commands[0].words[3] == NULL);
    pipeline_free(pl);
}

static void test_out_is_null_on_error(void)
{
    struct pipeline dummy;
    struct pipeline *pl = &dummy; /* anything but NULL, to see it reset */
    char err[128];

    CHECK(parse_line("| x", &pl, err, sizeof err) == PARSE_SYNTAX);
    CHECK(pl == NULL);
    CHECK(parse_line("", &pl, err, sizeof err) == PARSE_EMPTY);
    CHECK(pl == NULL);
}

static void test_long_pipelines_and_commands(void)
{
    static char line[1 << 15];
    struct pipeline *pl;
    char err[128];
    size_t n = 0;

    for (int i = 0; i < 1000; i++) {
        memcpy(line + n, "cat | ", 6);
        n += 6;
    }
    memcpy(line + n, "wc", 3);
    CHECK(parse_line(line, &pl, err, sizeof err) == PARSE_OK);
    CHECK_INT_EQ(1001, pl->ncommands);
    pipeline_free(pl);

    n = 0;
    memcpy(line + n, "echo", 4);
    n += 4;
    for (int i = 0; i < 2000; i++) {
        memcpy(line + n, " w", 2);
        n += 2;
    }
    line[n] = '\0';
    CHECK(parse_line(line, &pl, err, sizeof err) == PARSE_OK);
    CHECK_INT_EQ(2001, pl->commands[0].nwords);
    pipeline_free(pl);
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
    RUN(test_not_supported_yet);
    RUN(test_unfinished_lines);
    RUN(test_words_are_null_terminated);
    RUN(test_out_is_null_on_error);
    RUN(test_long_pipelines_and_commands);
    return check_summary();
}
