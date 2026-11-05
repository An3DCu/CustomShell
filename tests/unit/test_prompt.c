/* How the prompt shows the current folder. */
#include "check.h"

#include <stdlib.h>

#include "prompt.h"

static void check_folder(const char *file, int line, const char *cwd,
                         const char *home, const char *want)
{
    char *got = prompt_folder(cwd, home);

    if (!check_str_same(want, got))
        check_fail(file, line, "prompt_folder(%s, %s): want \"%s\", got \"%s\"",
                   cwd, home ? home : "NULL", want, got);
    free(got);
}

#define CHECK_FOLDER(cwd, home, want) check_folder(__FILE__, __LINE__, cwd, home, want)

static void test_home_becomes_tilde(void)
{
    CHECK_FOLDER("/home/ac", "/home/ac", "~");
    CHECK_FOLDER("/home/ac/projects", "/home/ac", "~/projects");
    CHECK_FOLDER("/home/ac/a/b/c", "/home/ac", "~/a/b/c");
}

static void test_outside_home_stays(void)
{
    CHECK_FOLDER("/tmp", "/home/ac", "/tmp");
    CHECK_FOLDER("/", "/home/ac", "/");
    CHECK_FOLDER("/home", "/home/ac", "/home");
}

static void test_only_whole_folder_names_match(void)
{
    /* The old acsh showed this as ~2. */
    CHECK_FOLDER("/home/ac2", "/home/ac", "/home/ac2");
    CHECK_FOLDER("/home/ac2/x", "/home/ac", "/home/ac2/x");
}

static void test_home_with_trailing_slash(void)
{
    CHECK_FOLDER("/home/ac", "/home/ac/", "~");
    CHECK_FOLDER("/home/ac/x", "/home/ac//", "~/x");
}

static void test_missing_or_odd_home(void)
{
    CHECK_FOLDER("/home/ac", NULL, "/home/ac");
    CHECK_FOLDER("/home/ac", "", "/home/ac");
    CHECK_FOLDER("/", "/", "~");
    CHECK_FOLDER("/usr", "/", "/usr");
}

int main(void)
{
    RUN(test_home_becomes_tilde);
    RUN(test_outside_home_stays);
    RUN(test_only_whole_folder_names_match);
    RUN(test_home_with_trailing_slash);
    RUN(test_missing_or_odd_home);
    return check_summary();
}
