#ifndef ACSH_PARSER_H
#define ACSH_PARSER_H

#include <stddef.h>

/*
 * The parser turns tokens into a tree the shell can run:
 *
 *     ls -l | grep c > n.txt
 *
 *     pipeline
 *       command  words: ls -l
 *       command  words: grep c   redirections: 1 > n.txt
 *
 * Words, assignments and redirection targets are kept as typed (quotes and
 * $ included); they are only expanded when the command runs. Grammar for
 * now:
 *
 *     pipeline    = command { "|" command }
 *     command     = ( word | redirection ) { word | redirection }
 *     redirection = [fd] ( "<" | ">" | ">>" | "<&" | ">&" ) word
 *
 * A word of the form NAME=value that comes before the command name is an
 * assignment: in `X=1 Y=2 env`, X and Y are assignments, but in
 * `env X=1` it's an argument.
 */

enum redir_kind {
    REDIR_IN,      /* <  open the file for reading            */
    REDIR_OUT,     /* >  create or empty the file, then write */
    REDIR_APPEND,  /* >> create if needed, write at the end   */
    REDIR_DUP_IN,  /* <& copy another fd (or - to close)      */
    REDIR_DUP_OUT, /* >& copy another fd (or - to close)      */
};

struct redirection {
    int fd;               /* which fd changes: 0 for < and <&, 1 for the rest, unless given */
    enum redir_kind kind;
    char *target;         /* file name, or fd number for the dups, as typed */
};

/* A command with no words is allowed: "> f" just creates f, and "X=1"
 * sets X. */
struct command {
    char **assigns;       /* NAME=value before the command name, as typed, NULL-terminated */
    size_t nassigns;
    char **words;         /* as typed, NULL-terminated */
    size_t nwords;
    struct redirection *redirs; /* in the order written; order matters */
    size_t nredirs;
};

struct pipeline {
    struct command *commands;
    size_t ncommands;
};

enum parse_status {
    PARSE_OK,
    PARSE_EMPTY,      /* nothing but blanks */
    PARSE_INCOMPLETE, /* the line stops too early: open quote, ends with | */
    PARSE_SYNTAX,     /* something is in the wrong place */
};

/*
 * Parses one line. On PARSE_OK, *out is a new pipeline to free with
 * pipeline_free. Otherwise *out is NULL, and for PARSE_INCOMPLETE and
 * PARSE_SYNTAX err says what's wrong, e.g. "syntax error near '|'".
 */
enum parse_status parse_line(const char *line, struct pipeline **out,
                             char *err, size_t errlen);

void pipeline_free(struct pipeline *pipeline);

#endif
