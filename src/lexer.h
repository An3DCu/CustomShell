#ifndef ACSH_LEXER_H
#define ACSH_LEXER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The lexer cuts a line into words and operators.
 *
 * A word keeps its quotes and backslashes exactly as typed: `"a b"` stays
 * `"a b"`. Later steps need them (in "$HOME" the $ expands, in '$HOME' it
 * doesn't), and word_unquote() removes them at the end. Both live in lexer.c,
 * so the quoting rules are written down in one place.
 */

enum token_type {
    TOK_WORD,
    TOK_PIPE,     /* |  */
    TOK_OR,       /* || */
    TOK_AMP,      /* &  */
    TOK_AND,      /* && */
    TOK_SEMI,     /* ;  */
    TOK_LESS,     /* <  */
    TOK_GREAT,    /* >  */
    TOK_DGREAT,   /* >> */
    TOK_LESSAND,  /* <& */
    TOK_GREATAND, /* >& */
    TOK_END,      /* end of the line; always the last token */
};

struct token {
    enum token_type type;
    char *text;    /* TOK_WORD: the word as typed. NULL for everything else. */
    int io_number; /* redirections: the fd written right before, like 2 in 2>, or -1 */
    size_t pos;    /* where the token starts in the line */
};

struct token_list {
    struct token *items;
    size_t len;
    size_t cap;
};

enum lex_status {
    LEX_OK,
    LEX_INCOMPLETE, /* the line stops inside a quote or after a backslash */
};

/*
 * Cuts line into tokens, ending with TOK_END. On LEX_INCOMPLETE, out is left
 * empty and err says why ("unclosed double quote"). Either way, free out with
 * token_list_free.
 */
enum lex_status lex(const char *line, struct token_list *out, char *err, size_t errlen);

void token_list_free(struct token_list *list);

bool token_is_redirection(enum token_type type);

/* How the token is written: "|", ">>", the word itself, or "end of line". */
const char *token_spelling(const struct token *tok);

/*
 * Removes quotes and backslashes the way a POSIX shell does:
 *   'single'   everything inside is literal
 *   "double"   a backslash only escapes $ ` " \ and newline; any other
 *              backslash stays
 *   \x         outside quotes, x is literal
 *   \newline   removed entirely (the line continues)
 * Returns a new string. The word should come from lex(), so its quotes are
 * closed; an unclosed one is handled without crashing, but not meaningfully.
 */
char *word_unquote(const char *word);

#endif
