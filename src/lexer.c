#include "lexer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

/* Spaces and tabs separate words. A newline is a token of its own: like
 * ;, it ends a command. */
static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/* A backslash right before a newline joins two lines, as if neither was
 * there (POSIX 2.2.1). Only when another line follows. At the very end
 * the lexer reports it. The shell can then read the next line. */
static bool is_line_join(const char *s)
{
    return s[0] == '\\' && s[1] == '\n' && s[2] != '\0';
}

static bool is_operator_char(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>'
        || c == '(' || c == ')';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static void push(struct token_list *list, enum token_type type, char *text,
                 int io_number, size_t pos)
{
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = xreallocarray(list->items, list->cap, sizeof *list->items);
    }
    list->items[list->len++] = (struct token){
        .type = type, .text = text, .io_number = io_number, .pos = pos,
    };
}

/* Reads the operator at line[*i], the longest one that fits (>> before >),
 * and moves *i past it. */
static enum token_type read_operator(const char *line, size_t *i)
{
    char c = line[*i];
    char next = line[*i + 1]; /* safe: at worst this is the final 0 */

    *i += 1;
    switch (c) {
    case '|':
        if (next == '|') {
            *i += 1;
            return TOK_OR;
        }
        return TOK_PIPE;
    case '&':
        if (next == '&') {
            *i += 1;
            return TOK_AND;
        }
        return TOK_AMP;
    case ';':
        return TOK_SEMI;
    case '(':
        return TOK_LPAREN;
    case ')':
        return TOK_RPAREN;
    case '<':
        if (next == '&') {
            *i += 1;
            return TOK_LESSAND;
        }
        return TOK_LESS;
    default: /* '>' */
        if (next == '>') {
            *i += 1;
            return TOK_DGREAT;
        }
        if (next == '&') {
            *i += 1;
            return TOK_GREATAND;
        }
        return TOK_GREAT;
    }
}

/*
 * Finds where the word starting at line[start] ends, at a blank or an
 * operator that isn't quoted. Quotes and backslashes are stepped over. They
 * stay in the word. Sets *quoted if the word has any. Returns false and
 * writes the reason into err if the line ends inside the word.
 */
static bool scan_word(const char *line, size_t start, size_t *end, bool *quoted,
                      char *err, size_t errlen)
{
    size_t i = start;

    *quoted = false;
    while (line[i] != '\0' && line[i] != '\n' && !is_blank(line[i])
           && !is_operator_char(line[i])) {
        if (line[i] == '\'') {
            const char *close = strchr(line + i + 1, '\'');

            *quoted = true;
            if (close == NULL) {
                snprintf(err, errlen, "unclosed single quote");
                return false;
            }
            i = (size_t)(close - line) + 1;
        } else if (line[i] == '"') {
            *quoted = true;
            i++;
            while (line[i] != '"') {
                if (line[i] == '\0') {
                    snprintf(err, errlen, "unclosed double quote");
                    return false;
                }
                /* Step over the escaped character. \" then doesn't close the
                 * quote. Whether the backslash stays is decided in expand.c. */
                if (line[i] == '\\' && line[i + 1] != '\0')
                    i++;
                i++;
            }
            i++;
        } else if (line[i] == '$' && line[i + 1] == '(') {
            /* $(...) belongs to the word, parentheses and all. The
             * expansion can then say it isn't supported. Otherwise the (
             * would be read as an operator. */
            int depth = 0;

            i++; /* past the $. The loop starts at the (. */
            do {
                if (line[i] == '\0') {
                    snprintf(err, errlen, "unclosed '$('");
                    return false;
                }
                if (line[i] == '(')
                    depth++;
                else if (line[i] == ')')
                    depth--;
                i++;
            } while (depth > 0);
        } else if (line[i] == '\\') {
            *quoted = true;
            /* getline keeps the newline. "echo a\" arrives as "echo a\\\n". */
            if (line[i + 1] == '\0' || (line[i + 1] == '\n' && line[i + 2] == '\0')) {
                snprintf(err, errlen, "line ends with a backslash");
                return false;
            }
            i += 2;
        } else {
            i++;
        }
    }
    *end = i;
    return true;
}

/* Digits to a file descriptor number. Absurdly large values become INT_MAX,
 * which the system then rejects as a bad file descriptor. */
static int digits_to_fd(const char *s, size_t n)
{
    int fd = 0;

    for (size_t i = 0; i < n; i++) {
        int d = s[i] - '0';

        if (fd > (INT_MAX - d) / 10)
            return INT_MAX;
        fd = fd * 10 + d;
    }
    return fd;
}

static bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i]))
            return false;
    }
    return n > 0;
}

/* Fills in each token's line, for error messages from scripts. */
static void number_lines(const char *text, struct token_list *list)
{
    size_t at = 0;
    int line = 0;

    for (size_t i = 0; i < list->len; i++) {
        for (; at < list->items[i].pos; at++) {
            if (text[at] == '\n')
                line++;
        }
        list->items[i].line = line;
    }
}

enum lex_status lex(const char *line, struct token_list *out, char *err, size_t errlen)
{
    size_t i = 0;

    *out = (struct token_list){ 0 };
    for (;;) {
        size_t start, end;
        bool quoted;

        while (is_blank(line[i]) || is_line_join(line + i))
            i += is_blank(line[i]) ? 1 : 2;
        start = i;
        if (line[i] == '\0') {
            push(out, TOK_END, NULL, -1, start);
            number_lines(line, out);
            return LEX_OK;
        }
        if (line[i] == '#') {
            /* A comment, only where a word would start. echo a#b prints
             * a#b. It runs to the end of the line, newline not included. A
             * backslash in it joins nothing. */
            while (line[i] != '\0' && line[i] != '\n')
                i++;
            continue;
        }
        if (line[i] == '\n') {
            push(out, TOK_NEWLINE, NULL, -1, start);
            i++;
            continue;
        }
        if (is_operator_char(line[i])) {
            enum token_type type = read_operator(line, &i);

            push(out, type, NULL, -1, start);
            continue;
        }
        if (!scan_word(line, start, &end, &quoted, err, errlen)) {
            token_list_free(out);
            return LEX_INCOMPLETE;
        }
        i = end;
        /* Unquoted digits right before < or > name a file descriptor (POSIX
         * calls them an IO_NUMBER). 2>err has one. 2 >err and "2">err don't. */
        if (!quoted && (line[i] == '<' || line[i] == '>')
            && all_digits(line + start, end - start)) {
            int fd = digits_to_fd(line + start, end - start);
            enum token_type type = read_operator(line, &i);

            push(out, type, NULL, fd, start);
        } else {
            push(out, TOK_WORD, xstrndup(line + start, end - start), -1, start);
        }
    }
}

void token_list_free(struct token_list *list)
{
    for (size_t i = 0; i < list->len; i++)
        free(list->items[i].text);
    free(list->items);
    *list = (struct token_list){ 0 };
}

bool token_is_redirection(enum token_type type)
{
    return type == TOK_LESS || type == TOK_GREAT || type == TOK_DGREAT
        || type == TOK_LESSAND || type == TOK_GREATAND;
}

const char *token_spelling(const struct token *tok)
{
    switch (tok->type) {
    case TOK_WORD:     return tok->text;
    case TOK_PIPE:     return "|";
    case TOK_OR:       return "||";
    case TOK_AMP:      return "&";
    case TOK_AND:      return "&&";
    case TOK_SEMI:     return ";";
    case TOK_NEWLINE:  return "newline";
    case TOK_LPAREN:   return "(";
    case TOK_RPAREN:   return ")";
    case TOK_LESS:     return "<";
    case TOK_GREAT:    return ">";
    case TOK_DGREAT:   return ">>";
    case TOK_LESSAND:  return "<&";
    case TOK_GREATAND: return ">&";
    case TOK_END:      return "end of line";
    }
    return "?";
}
