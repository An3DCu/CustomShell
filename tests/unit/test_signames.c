/*
 * Signal names for kill: every way of writing one, and back from a number.
 */
#include "check.h"

#include <signal.h>

#include "signames.h"

static void test_names_in_every_form(void)
{
    CHECK_INT_EQ(SIGTERM, signame_parse("TERM"));
    CHECK_INT_EQ(SIGTERM, signame_parse("SIGTERM"));
    CHECK_INT_EQ(SIGTERM, signame_parse("term"));
    CHECK_INT_EQ(SIGTERM, signame_parse("SigTerm"));
    CHECK_INT_EQ(SIGKILL, signame_parse("KILL"));
    CHECK_INT_EQ(SIGVTALRM, signame_parse("vtalrm"));
}

static void test_numbers(void)
{
    CHECK_INT_EQ(15, signame_parse("15"));
    CHECK_INT_EQ(0, signame_parse("0"));
    CHECK_INT_EQ(64, signame_parse("64")); /* no name, but still a number */
    CHECK_INT_EQ(-1, signame_parse("15x"));
    CHECK_INT_EQ(-1, signame_parse("99999999999"));
}

static void test_not_signals(void)
{
    CHECK_INT_EQ(-1, signame_parse("FOO"));
    CHECK_INT_EQ(-1, signame_parse("SIG"));
    CHECK_INT_EQ(-1, signame_parse(""));
    CHECK_INT_EQ(-1, signame_parse("SIGSIGTERM"));
}

static void test_back_from_a_number(void)
{
    CHECK_STR_EQ("TERM", signame_of(SIGTERM));
    CHECK_STR_EQ("HUP", signame_of(SIGHUP));
    CHECK(signame_of(0) == NULL);
    CHECK(signame_max() >= SIGTERM);
#ifdef SIGPOLL
    /* POLL is accepted, but the list shows IO, Linux's name for it */
    CHECK_INT_EQ(SIGPOLL, signame_parse("POLL"));
    CHECK_STR_EQ("IO", signame_of(SIGPOLL));
#endif
}

int main(void)
{
    RUN(test_names_in_every_form);
    RUN(test_numbers);
    RUN(test_not_signals);
    RUN(test_back_from_a_number);
    return check_summary();
}
