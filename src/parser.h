#ifndef ACSH_PARSER_H
#define ACSH_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The parser turns tokens into a tree the shell can run:
 *
 *     make && ./run | tee log; echo done
 *
 *     list
 *       and-or
 *         pipeline   command  words: make
 *         pipeline   (only if the one before succeeded)
 *                    command  words: ./run
 *                    command  words: tee log
 *       and-or
 *         pipeline   command  words: echo done
 *
 * Words, assignments and redirection targets are kept as typed (quotes and
 * $ included); they are only expanded when the command runs. Grammar for
 * now, where newline means one or more newlines:
 *
 *     program     = [newline] [list] [newline]
 *     list        = and_or { ( ";" | newline ) and_or } [";"]
 *     and_or      = pipeline { ( "&&" | "||" ) [newline] pipeline }
 *     pipeline    = ["!"] command { "|" [newline] command }
 *     command     = simple | compound { redirection }
 *     simple      = ( word | redirection ) { word | redirection }
 *     compound    = "if" list "then" list { "elif" list "then" list }
 *                        [ "else" list ] "fi"
 *                 | ( "while" | "until" ) list "do" list "done"
 *                 | "for" name [newline] [ "in" { word } ( ";" | newline ) ]
 *                        [newline] "do" list "done"
 *     redirection = [fd] ( "<" | ">" | ">>" | "<&" | ">&" ) word
 *
 * The words in quotes are reserved words. They only count where a command
 * starts and when not quoted: `echo if` prints if, and `"if"` is a command
 * called if. Inside a compound, a list ends at the reserved word that comes
 * next (then, elif, else, fi, do, done).
 *
 * A line can start with the word explain: the rest of it is parsed as
 * usual and marked to be explained (see explain.h).
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

struct list;

enum command_kind {
    CMD_SIMPLE,
    CMD_IF,
    CMD_WHILE,
    CMD_UNTIL,
    CMD_FOR,
};

/* if cond; then body (also each elif) */
struct if_branch {
    struct list *cond;
    struct list *body;
};

/* A simple command with no words is allowed: "> f" just creates f, and
 * "X=1" sets X. Which fields are used depends on kind. */
struct command {
    enum command_kind kind;
    int line;             /* the line it starts on, counting from 0 in the parsed text */
    char **assigns;       /* simple: NAME=value before the name, as typed, NULL-terminated */
    size_t nassigns;
    char **words;         /* simple: the words; for: the words after in. As typed, NULL-terminated */
    size_t nwords;
    struct redirection *redirs; /* any kind, in the order written; order matters */
    size_t nredirs;
    struct if_branch *branches; /* if: the if and every elif */
    size_t nbranches;
    struct list *cond;    /* while, until */
    struct list *body;    /* while, until, for; for if, the else part (or NULL) */
    char *var;            /* for: the variable */
    bool has_in;          /* for: false means `for x; do`, which loops over "$@" */
};

/* When a pipeline in an and-or runs, decided by the status before it. */
enum run_if {
    RUN_ALWAYS,    /* the first one */
    RUN_IF_OK,     /* after && */
    RUN_IF_FAILED, /* after || */
};

struct pipeline {
    struct command *commands;
    size_t ncommands;
    bool negate;          /* ! in front: 0 becomes 1, anything else 0 */
    enum run_if run_if;
};

/* a && b || c */
struct and_or {
    struct pipeline *pipelines;
    size_t npipelines;
};

/* a; b, or a and b on two lines */
struct list {
    struct and_or *items;
    size_t nitems;
    bool explain; /* the line started with the word explain */
};

enum parse_status {
    PARSE_OK,
    PARSE_EMPTY,      /* nothing but blanks, newlines and comments */
    PARSE_INCOMPLETE, /* the text stops too early: open quote, ends with | or && */
    PARSE_SYNTAX,     /* something is in the wrong place */
};

/*
 * Parses text, which may hold several lines. On PARSE_OK, *out is a new
 * list to free with list_free. Otherwise *out is NULL, and for
 * PARSE_INCOMPLETE and PARSE_SYNTAX err says what's wrong, e.g.
 * "syntax error near '|'". On PARSE_INCOMPLETE the shell reads another
 * line, adds it to text and calls this again.
 */
enum parse_status parse_program(const char *text, struct list **out,
                                char *err, size_t errlen);

void list_free(struct list *list);

#endif
