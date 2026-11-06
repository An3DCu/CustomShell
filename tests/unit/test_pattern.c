/*
 * The file name matcher and the folder walk. The matching rules were
 * checked against dash on a folder of awkward names (see
 * tests/e2e/test_globbing.py, which compares the two shells directly).
 */
#include "check.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pattern.h"
#include "strvec.h"

/* Every byte may be special. */
static bool matches(const char *pattern, const char *name)
{
    struct pattern p = { .text = pattern, .active = NULL, .len = strlen(pattern) };

    return pattern_match(&p, name);
}

static bool magic(const char *pattern)
{
    struct pattern p = { .text = pattern, .active = NULL, .len = strlen(pattern) };

    return pattern_has_magic(&p);
}

static void test_star(void)
{
    CHECK(matches("*", ""));
    CHECK(matches("*", "anything"));
    CHECK(matches("a*", "a"));
    CHECK(matches("a*", "abc"));
    CHECK(matches("*c", "abc"));
    CHECK(matches("a*b*c", "aXbYc"));
    CHECK(matches("a*b", "ab"));
    CHECK(matches("**", "x"));
    CHECK(!matches("*a", "bbb"));
    CHECK(!matches("a*", "ba"));
}

static void test_star_backtracks(void)
{
    CHECK(matches("*ab", "aab"));
    CHECK(matches("*a*b", "xaxxab"));
    CHECK(!matches("*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

static void test_question_mark(void)
{
    CHECK(matches("a?c", "abc"));
    CHECK(matches("???", "a b"));
    CHECK(!matches("?", ""));
    CHECK(!matches("??", "a"));
    CHECK(!matches("*?*", ""));
}

static void test_brackets(void)
{
    CHECK(matches("[abc]", "b"));
    CHECK(!matches("[abc]", "d"));
    CHECK(matches("[a-c]x", "bx"));
    CHECK(!matches("[a-c]", "B"));
    CHECK(matches("[!abc]", "d"));
    CHECK(!matches("[!abc]", "a"));
    CHECK(matches("[^a]", "^"));     /* only ! negates, as in dash */
    CHECK(!matches("[z-a]", "m"));   /* an empty range */
}

static void test_brackets_with_odd_members(void)
{
    CHECK(matches("[]a]", "]"));     /* ] first is a member */
    CHECK(matches("[!]a]", "b"));
    CHECK(!matches("[!]a]", "]"));
    CHECK(matches("[a-]", "-"));     /* - last is a member */
    CHECK(matches("[-a]", "-"));
    CHECK(matches("[*?]", "*"));     /* nothing is special inside */
}

static void test_classes(void)
{
    CHECK(matches("[[:digit:]]x", "5x"));
    CHECK(matches("[[:upper:][:digit:]]", "Q"));
    CHECK(matches("[[:alpha:]-]", "-"));
    CHECK(!matches("[[:alpha:]]", "1"));
    CHECK(!matches("[[:nope:]]", "a"));
    CHECK(!matches("[[:alpha:]]", "\xc3")); /* the first byte of ä */
}

static void test_unclosed_bracket_is_literal(void)
{
    CHECK(matches("[a", "[a"));
    CHECK(matches("a[b", "a[b"));
    CHECK(matches("*[", "x["));
    CHECK(!matches("[a", "a"));
}

static void test_quoted_bytes_are_literal(void)
{
    /* "*".c: the * came from inside quotes */
    unsigned char active[] = { 0, 1, 1 };
    struct pattern p = { .text = "*.c", .active = active, .len = 3 };

    CHECK(pattern_match(&p, "*.c"));
    CHECK(!pattern_match(&p, "a.c"));
    CHECK(!pattern_has_magic(&p));
}

static void test_quoted_closing_bracket_is_a_member(void)
{
    /* [a\]] in the shell: the first ] is escaped */
    unsigned char active[] = { 1, 1, 0, 1 };
    struct pattern p = { .text = "[a]]", .active = active, .len = 4 };

    CHECK(pattern_match(&p, "]"));
    CHECK(pattern_match(&p, "a"));
}

static void test_magic(void)
{
    CHECK(!magic("abc"));
    CHECK(!magic(""));
    CHECK(magic("a*"));
    CHECK(magic("?"));
    CHECK(magic("a[b]"));
    CHECK(!magic("[a"));       /* never closes */
    CHECK(!magic("x/[y"));
    CHECK(!magic("[/]"));      /* a bracket can't hold a / */
    CHECK(magic("x/*"));
    CHECK(magic("*/x"));
}

/* ---- the folder walk ---------------------------------------------------- */

static char root[64];

static void make_file(const char *name)
{
    char path[256];
    int fd;

    snprintf(path, sizeof path, "%s/%s", root, name);
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
        close(fd);
}

static void make_dir(const char *name)
{
    char path[256];

    snprintf(path, sizeof path, "%s/%s", root, name);
    mkdir(path, 0755);
}

/* The matches of root/<pattern>, as "[a][b]" with root/ taken off. */
static const char *expand(const char *pattern)
{
    static char out[2048];
    char full[256];
    struct strvec found = { 0 };
    size_t used = 0;
    size_t skip = strlen(root) + 1;
    struct pattern p;

    snprintf(full, sizeof full, "%s/%s", root, pattern);
    p = (struct pattern){ .text = full, .active = NULL, .len = strlen(full) };
    out[0] = '\0';
    if (pattern_expand(&p, &found) == 0)
        snprintf(out, sizeof out, "none");
    for (size_t i = 0; i < found.len; i++)
        used += (size_t)snprintf(out + used, sizeof out - used, "[%s]", found.items[i] + skip);
    strvec_free(&found);
    return out;
}

static void test_walk(void)
{
    char link_to[256];

    strcpy(root, "/tmp/acsh-pattern-XXXXXX");
    if (mkdtemp(root) == NULL) {
        check_fail(__FILE__, __LINE__, "mkdtemp failed");
        return;
    }
    make_file("b");
    make_file("a");
    make_file("B");
    make_file(".hidden");
    make_file("a b");
    make_dir("dir");
    make_dir("dir/sub");
    make_file("dir/f1");
    make_file("dir/f2");
    make_dir("empty");
    snprintf(link_to, sizeof link_to, "%s/dangling", root);
    if (symlink("nowhere", link_to) != 0)
        check_fail(__FILE__, __LINE__, "symlink failed");

    CHECK_STR_EQ("[B][a][a b][b][dangling][dir][empty]", expand("*"));
    CHECK_STR_EQ("[.][..][.hidden]", expand(".*"));
    CHECK_STR_EQ("[B][a][b]", expand("?"));
    CHECK_STR_EQ("[dir/][empty/]", expand("*/"));
    CHECK_STR_EQ("[dir/f1][dir/f2][dir/sub]", expand("*/*"));
    CHECK_STR_EQ("[dir/f1]", expand("*/f1"));
    CHECK_STR_EQ("none", expand("*/nope"));
    CHECK_STR_EQ("[dir/sub]", expand("d*/s*"));
    CHECK_STR_EQ("[dangling]", expand("dang*"));
    CHECK_STR_EQ("none", expand("nomatch*"));
    CHECK_STR_EQ("none", expand("?hidden"));
    CHECK_STR_EQ("[dir//f1][dir//f2]", expand("dir//f*"));

    /* clean up: rm -r by hand, in the right order */
    {
        const char *files[] = { "b", "a", "B", ".hidden", "a b", "dir/f1", "dir/f2",
                                "dangling", NULL };
        const char *dirs[] = { "dir/sub", "dir", "empty", NULL };
        char path[256];

        for (size_t i = 0; files[i] != NULL; i++) {
            snprintf(path, sizeof path, "%s/%s", root, files[i]);
            unlink(path);
        }
        for (size_t i = 0; dirs[i] != NULL; i++) {
            snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
            rmdir(path);
        }
        rmdir(root);
    }
}

int main(void)
{
    RUN(test_star);
    RUN(test_star_backtracks);
    RUN(test_question_mark);
    RUN(test_brackets);
    RUN(test_brackets_with_odd_members);
    RUN(test_classes);
    RUN(test_unclosed_bracket_is_literal);
    RUN(test_quoted_bytes_are_literal);
    RUN(test_quoted_closing_bracket_is_a_member);
    RUN(test_magic);
    RUN(test_walk);
    return check_summary();
}
