/*
 * Expansion and quote removal. Every expected value here was also checked
 * against dash, the POSIX shell Ubuntu uses as /bin/sh, with
 * printf '[%s]' <word>: the fields come back as "[a][b]", and no fields at
 * all as "".
 */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expand.h"
#include "shell.h"
#include "strvec.h"
#include "vars.h"

static struct shell sh;
static char described[4096];

/* A fresh shell with no variables, $? = 0, $$ = 4242 and $0 = acsh. */
static void reset(void)
{
    vars_free(&sh.vars);
    vars_init(&sh.vars, NULL);
    sh.status = 0;
    sh.pid = 4242;
    sh.name = "acsh";
}

static void set(const char *name, const char *value)
{
    vars_set(&sh.vars, name, value);
}

/* The fields of words[0..n) as "[a][b]", or "error: <why>". */
static const char *fields_of(char *const *words, size_t n)
{
    struct strvec out = { 0 };
    char err[128];
    size_t used = 0;

    described[0] = '\0';
    if (!expand_command(&sh, words, n, &out, err, sizeof err)) {
        snprintf(described, sizeof described, "error: %s", err);
    } else {
        for (size_t i = 0; i < out.len; i++)
            used += (size_t)snprintf(described + used, sizeof described - used,
                                     "[%s]", out.items[i]);
    }
    strvec_free(&out);
    return described;
}

static const char *fields(const char *word)
{
    char *words[] = { (char *)word, NULL };

    return fields_of(words, 1);
}

/* One word to one string, as for a redirection target. */
static const char *one(const char *word)
{
    char err[128];
    char *s = expand_one(&sh, word, err, sizeof err);

    if (s == NULL) {
        snprintf(described, sizeof described, "error: %s", err);
    } else {
        snprintf(described, sizeof described, "%s", s);
        free(s);
    }
    return described;
}

static const char *assignment(const char *value)
{
    char err[128];
    char *s = expand_assignment(&sh, value, err, sizeof err);

    if (s == NULL) {
        snprintf(described, sizeof described, "error: %s", err);
    } else {
        snprintf(described, sizeof described, "%s", s);
        free(s);
    }
    return described;
}

/* ---- quote removal (what word_unquote did in v0.1) ---------------------- */

static void test_plain_words(void)
{
    reset();
    CHECK_STR_EQ("echo", one("echo"));
    CHECK_STR_EQ("a-b_c.txt", one("a-b_c.txt"));
}

static void test_empty_quotes_make_an_empty_word(void)
{
    reset();
    CHECK_STR_EQ("", one("''"));
    CHECK_STR_EQ("[]", fields("''"));
    CHECK_STR_EQ("[]", fields("\"\""));
    CHECK_STR_EQ("[]", fields("''\"\""));
}

static void test_single_quotes(void)
{
    reset();
    set("HOME", "/home/user");
    CHECK_STR_EQ("a b", one("'a b'"));
    CHECK_STR_EQ("$HOME", one("'$HOME'"));
    CHECK_STR_EQ("a\\nb", one("'a\\nb'"));   /* no escapes at all */
    CHECK_STR_EQ("\\", one("'\\'"));
    CHECK_STR_EQ("\"", one("'\"'"));
}

static void test_double_quotes(void)
{
    reset();
    CHECK_STR_EQ("a b", one("\"a b\""));
    CHECK_STR_EQ("it's", one("\"it's\""));
    CHECK_STR_EQ("a\"b", one("\"a\\\"b\""));  /* \" */
    CHECK_STR_EQ("a\\b", one("\"a\\\\b\""));  /* \\ */
    CHECK_STR_EQ("$x", one("\"\\$x\""));      /* \$ */
}

static void test_backslash_stays_in_double_quotes_before_others(void)
{
    reset();
    CHECK_STR_EQ("\\a", one("\"\\a\""));
    CHECK_STR_EQ("\\n", one("\"\\n\""));
    CHECK_STR_EQ("a\\'b", one("\"a\\'b\""));
}

static void test_backslash_outside_quotes(void)
{
    reset();
    CHECK_STR_EQ("a b", one("a\\ b"));
    CHECK_STR_EQ("|", one("\\|"));
    CHECK_STR_EQ("\\", one("\\\\"));
    CHECK_STR_EQ("a", one("\\a"));
    CHECK_STR_EQ("'", one("\\'"));
    CHECK_STR_EQ("$X", one("\\$X"));
}

static void test_backslash_newline_disappears(void)
{
    reset();
    CHECK_STR_EQ("ab", one("a\\\nb"));
    CHECK_STR_EQ("ab", one("\"a\\\nb\""));
}

static void test_mixed_pieces_join_into_one_word(void)
{
    reset();
    CHECK_STR_EQ("ab cd", one("a\"b c\"'d'"));
    CHECK_STR_EQ("it's", one("'it'\\''s'"));
    CHECK_STR_EQ("$$$", one("\"$\"'$'\\$"));
}

static void test_unclosed_input_does_not_crash(void)
{
    reset();
    CHECK_STR_EQ("abc", one("'abc"));
    CHECK_STR_EQ("abc", one("\"abc"));
    CHECK_STR_EQ("abc\\", one("abc\\"));
}

/* ---- parameters --------------------------------------------------------- */

static void test_variables(void)
{
    reset();
    set("X", "hello");
    CHECK_STR_EQ("[hello]", fields("$X"));
    CHECK_STR_EQ("[hello]", fields("${X}"));
    CHECK_STR_EQ("[hello]", fields("\"$X\""));
    CHECK_STR_EQ("[hello!]", fields("$X!"));
    CHECK_STR_EQ("[hello_b]", fields("${X}_b"));
    CHECK_STR_EQ("", fields("$X_b"));           /* the name is X_b */
    CHECK_STR_EQ("[$X]", fields("'$X'"));
    CHECK_STR_EQ("[<hello>]", fields("<\"$X\">"));
}

static void test_unset_variables_are_empty(void)
{
    reset();
    CHECK_STR_EQ("", fields("$NOPE"));
    CHECK_STR_EQ("[]", fields("\"$NOPE\""));
    CHECK_STR_EQ("[a]", fields("a$NOPE"));
}

static void test_special_parameters(void)
{
    reset();
    sh.status = 127;
    CHECK_STR_EQ("[127]", fields("$?"));
    CHECK_STR_EQ("[127]", fields("${?}"));
    CHECK_STR_EQ("[4242]", fields("$$"));
    CHECK_STR_EQ("[4242]", fields("\"${$}\""));
    CHECK_STR_EQ("[acsh]", fields("$0"));
}

static void test_a_lone_dollar_stays(void)
{
    reset();
    CHECK_STR_EQ("[$]", fields("$"));
    CHECK_STR_EQ("[$]", fields("\"$\""));
    CHECK_STR_EQ("[a$]", fields("a$"));
    CHECK_STR_EQ("[$%]", fields("$%"));
    CHECK_STR_EQ("[$-x]", fields("$-x")); /* $- isn't supported; kept as typed */
}

static void test_bad_substitutions(void)
{
    reset();
    CHECK_STR_EQ("error: missing '}' after '${'", fields("${A"));
    CHECK_STR_EQ("error: ${}: bad substitution", fields("${}"));
    CHECK_STR_EQ("error: ${1x}: bad substitution", fields("${1x}"));
    CHECK_STR_EQ("error: ${X:-d}: bad substitution", fields("${X:-d}"));
}

static void test_command_substitution_is_an_error_for_now(void)
{
    reset();
    CHECK_STR_EQ("error: '$(' isn't supported yet", fields("$(ls)"));
    CHECK_STR_EQ("error: '$(' isn't supported yet", fields("\"$(ls)\""));
    CHECK_STR_EQ("error: '`' isn't supported yet", fields("`ls`"));
    CHECK_STR_EQ("[`ls`]", fields("'`ls`'"));
    CHECK_STR_EQ("[`]", fields("\\`"));
}

/* ---- tilde -------------------------------------------------------------- */

static void test_tilde(void)
{
    reset();
    set("HOME", "/home/user");
    CHECK_STR_EQ("[/home/user]", fields("~"));
    CHECK_STR_EQ("[/home/user/a]", fields("~/a"));
    CHECK_STR_EQ("[~]", fields("\"~\""));
    CHECK_STR_EQ("[~]", fields("\\~"));
    CHECK_STR_EQ("[x~]", fields("x~"));
    CHECK_STR_EQ("[a=~]", fields("a=~"));       /* only in assignments */
    CHECK_STR_EQ("[~nosuchuser]", fields("~nosuchuser"));
    CHECK_STR_EQ("[~x]", fields("~\"x\""));   /* a quote in the name: no tilde */
    CHECK_STR_EQ("[/root]", fields("~root"));
}

static void test_tilde_is_not_split(void)
{
    reset();
    set("HOME", "/home/a b");
    CHECK_STR_EQ("[/home/a b/x]", fields("~/x"));
}

static void test_tilde_without_home(void)
{
    reset();
    CHECK_STR_EQ("[~]", fields("~"));     /* unset HOME: left alone */
    set("HOME", "");
    CHECK_STR_EQ("", fields("~"));        /* empty HOME: no word at all, like dash */
}

static void test_tilde_in_assignments(void)
{
    reset();
    set("HOME", "/h");
    CHECK_STR_EQ("/h:/h/b", assignment("~:~/b"));
    CHECK_STR_EQ("a:/h", assignment("a:~"));
    CHECK_STR_EQ("a~", assignment("a~"));
}

/* ---- field splitting ---------------------------------------------------- */

static void test_split_on_spaces(void)
{
    reset();
    set("X", " a  b ");
    CHECK_STR_EQ("[a][b]", fields("$X"));
    CHECK_STR_EQ("[ a  b ]", fields("\"$X\""));
    CHECK_STR_EQ("[<][a][b][>]", fields("<$X>"));
    set("X", "a\tb\nc");
    CHECK_STR_EQ("[a][b][c]", fields("$X"));
}

static void test_split_never_touches_what_was_typed(void)
{
    reset();
    set("X", "1");
    CHECK_STR_EQ("[a b1]", fields("'a b'$X"));
    CHECK_STR_EQ("a b", one("a\\ b"));
}

static void test_split_with_other_ifs(void)
{
    reset();
    set("IFS", ":");
    set("X", "a::b:");
    CHECK_STR_EQ("[a][][b]", fields("$X"));
    set("X", "a::");
    CHECK_STR_EQ("[a][]", fields("$X"));
    set("X", ":a");
    CHECK_STR_EQ("[][a]", fields("$X"));
    set("X", "a b");
    CHECK_STR_EQ("[a b]", fields("$X"));
}

static void test_split_with_mixed_ifs(void)
{
    reset();
    set("IFS", ": ");
    set("X", " :a: b");
    CHECK_STR_EQ("[][a][b]", fields("$X"));
}

static void test_empty_ifs_splits_nothing(void)
{
    reset();
    set("IFS", "");
    set("X", "a b");
    CHECK_STR_EQ("[a b]", fields("$X"));
}

static void test_empty_quotes_keep_a_field(void)
{
    reset();
    set("X", " a");
    CHECK_STR_EQ("[][a]", fields("\"\"$X"));
    set("X", "a ");
    set("Y", " b");
    CHECK_STR_EQ("[a][][b]", fields("$X\"\"$Y"));
}

static void test_values_are_never_syntax(void)
{
    reset();
    set("X", "a; rm -rf ~ | cat > f");
    CHECK_STR_EQ("[a;][rm][-rf][~][|][cat][>][f]", fields("$X"));
    set("Y", "$X");
    CHECK_STR_EQ("[$X]", fields("$Y")); /* expanded once, not again */
    set("Q", "'a b'");
    CHECK_STR_EQ("['a][b']", fields("$Q"));
}

static void test_export_arguments_are_not_split(void)
{
    char *words[] = { "export", "X=$Y", "Z=~", "$Y", NULL };

    reset();
    set("Y", "a b");
    set("HOME", "/h");
    CHECK_STR_EQ("[export][X=a b][Z=/h][a][b]", fields_of(words, 4));
}

static void test_quoted_export_is_an_ordinary_command(void)
{
    char *words[] = { "'export'", "X=$Y", NULL };

    reset();
    set("Y", "a b");
    CHECK_STR_EQ("[export][X=a][b]", fields_of(words, 2));
}

static void test_one_word_targets_are_not_split(void)
{
    reset();
    set("F", "a b");
    CHECK_STR_EQ("a b", one("$F"));
    CHECK_STR_EQ("", one("$NOPE"));
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
    RUN(test_variables);
    RUN(test_unset_variables_are_empty);
    RUN(test_special_parameters);
    RUN(test_a_lone_dollar_stays);
    RUN(test_bad_substitutions);
    RUN(test_command_substitution_is_an_error_for_now);
    RUN(test_tilde);
    RUN(test_tilde_is_not_split);
    RUN(test_tilde_without_home);
    RUN(test_tilde_in_assignments);
    RUN(test_split_on_spaces);
    RUN(test_split_never_touches_what_was_typed);
    RUN(test_split_with_other_ifs);
    RUN(test_split_with_mixed_ifs);
    RUN(test_empty_ifs_splits_nothing);
    RUN(test_empty_quotes_keep_a_field);
    RUN(test_values_are_never_syntax);
    RUN(test_export_arguments_are_not_split);
    RUN(test_quoted_export_is_an_ordinary_command);
    RUN(test_one_word_targets_are_not_split);
    vars_free(&sh.vars);
    return check_summary();
}
