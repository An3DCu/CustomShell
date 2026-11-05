#include "lexer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

/* A newline counts as a blank for now: lines arrive one at a time. Scripts
 * will need it as a separator. */
static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static bool is_operator_char(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
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
 * Finds where the word starting at line[start] ends: at a blank or an
 * operator that isn't quoted. Quotes and backslashes are stepped over, not
 * removed. Sets *quoted if the word has any. Returns false and writes the
 * reason into err if the line ends inside the word.
 */
static bool scan_word(const char *line, size_t start, size_t *end, bool *quoted,
                      char *err, size_t errlen)
{
    size_t i = start;

    *quoted = false;
    while (line[i] != '\0' && !is_blank(line[i]) && !is_operator_char(line[i])) {
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
                /* Step over the escaped character, so \" doesn't close the
                 * quote. Whether the backslash stays is word_unquote's job. */
                if (line[i] == '\\' && line[i + 1] != '\0')
                    i++;
                i++;
            }
            i++;
        } else if (line[i] == '\\') {
            *quoted = true;
            /* getline keeps the newline, so "echo a\" arrives as "echo a\\\n". */
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

enum lex_status lex(const char *line, struct token_list *out, char *err, size_t errlen)
{
    size_t i = 0;

    *out = (struct token_list){ 0 };
    for (;;) {
        size_t start, end;
        bool quoted;

        while (is_blank(line[i]))
            i++;
        start = i;
        if (line[i] == '\0') {
            push(out, TOK_END, NULL, -1, start);
            return LEX_OK;
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
         * calls them an IO_NUMBER): 2>err, but not 2 >err and not "2">err. */
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
    case TOK_LESS:     return "<";
    case TOK_GREAT:    return ">";
    case TOK_DGREAT:   return ">>";
    case TOK_LESSAND:  return "<&";
    case TOK_GREATAND: return ">&";
    case TOK_END:      return "end of line";
    }
    return "?";
}

/* Inside double quotes, a backslash only escapes these. */
static bool escapable_in_double_quotes(char c)
{
    return c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n';
}

char *word_unquote(const char *word)
{
    char *out = xmalloc(strlen(word) + 1); /* unquoting only ever shrinks */
    size_t o = 0;
    size_t i = 0;

    while (word[i] != '\0') {
        if (word[i] == '\'') {
            i++;
            while (word[i] != '\'' && word[i] != '\0')
                out[o++] = word[i++];
            if (word[i] != '\0')
                i++;
        } else if (word[i] == '"') {
            i++;
            while (word[i] != '"' && word[i] != '\0') {
                if (word[i] == '\\' && word[i + 1] != '\0'
                    && escapable_in_double_quotes(word[i + 1])) {
                    if (word[i + 1] != '\n')
                        out[o++] = word[i + 1];
                    i += 2;
                } else {
                    out[o++] = word[i++];
                }
            }
            if (word[i] != '\0')
                i++;
        } else if (word[i] == '\\' && word[i + 1] != '\0') {
            if (word[i + 1] != '\n')
                out[o++] = word[i + 1];
            i += 2;
        } else {
            out[o++] = word[i++];
        }
    }
    out[o] = '\0';
    return out;
}
