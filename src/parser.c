#include "parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "lexer.h"
#include "vars.h"
#include "xalloc.h"

/* Reads the tokens left to right; *pos is the next one. TOK_END is always
 * last, so looking one token ahead of anything else is safe. */
struct parser {
    const struct token_list *tokens;
    size_t pos;
    char *err;
    size_t errlen;
};

static const struct token *peek(const struct parser *p)
{
    return &p->tokens->items[p->pos];
}

static enum parse_status unexpected(struct parser *p, const struct token *tok)
{
    switch (tok->type) {
    case TOK_SEMI:
    case TOK_AMP:
    case TOK_AND:
    case TOK_OR:
        snprintf(p->err, p->errlen, "'%s' isn't supported yet", token_spelling(tok));
        break;
    case TOK_END:
        snprintf(p->err, p->errlen, "syntax error near end of line");
        break;
    default:
        snprintf(p->err, p->errlen, "syntax error near '%s'", token_spelling(tok));
        break;
    }
    return PARSE_SYNTAX;
}

/* Adds text to a NULL-terminated list of n words. */
static void push_word(char ***list, size_t *n, const char *text)
{
    /* +2: room for the new word and the NULL after it. */
    *list = xreallocarray(*list, *n + 2, sizeof **list);
    (*list)[(*n)++] = xstrdup(text);
    (*list)[*n] = NULL;
}

/* NAME=value with a plain, unquoted name: X=1 and X="a b", but not "X"=1
 * or 1X=1. Only the name has to be plain; the value can be anything. */
static bool is_assignment(const char *word)
{
    size_t n = var_name_len(word);

    return n > 0 && word[n] == '=';
}

static void add_redirection(struct command *cmd, const struct token *op,
                            const char *target)
{
    struct redirection r = { .target = xstrdup(target) };

    switch (op->type) {
    case TOK_LESS:     r.kind = REDIR_IN;      r.fd = 0; break;
    case TOK_LESSAND:  r.kind = REDIR_DUP_IN;  r.fd = 0; break;
    case TOK_DGREAT:   r.kind = REDIR_APPEND;  r.fd = 1; break;
    case TOK_GREATAND: r.kind = REDIR_DUP_OUT; r.fd = 1; break;
    default:           r.kind = REDIR_OUT;     r.fd = 1; break;
    }
    if (op->io_number >= 0)
        r.fd = op->io_number;
    cmd->redirs = xreallocarray(cmd->redirs, cmd->nredirs + 1, sizeof *cmd->redirs);
    cmd->redirs[cmd->nredirs++] = r;
}

/* command = ( word | redirection ) { word | redirection }
 * Words like NAME=value before the command name are assignments; after it,
 * they're ordinary arguments (`echo X=1` prints X=1). */
static enum parse_status parse_command(struct parser *p, struct command *cmd)
{
    for (;;) {
        const struct token *tok = peek(p);

        if (tok->type == TOK_WORD) {
            if (cmd->nwords == 0 && is_assignment(tok->text))
                push_word(&cmd->assigns, &cmd->nassigns, tok->text);
            else
                push_word(&cmd->words, &cmd->nwords, tok->text);
            p->pos++;
        } else if (token_is_redirection(tok->type)) {
            const struct token *target = &p->tokens->items[p->pos + 1];

            if (target->type != TOK_WORD)
                return unexpected(p, target);
            add_redirection(cmd, tok, target->text);
            p->pos += 2;
        } else {
            break;
        }
    }
    return PARSE_OK;
}

static bool starts_command(enum token_type type)
{
    return type == TOK_WORD || token_is_redirection(type);
}

/* pipeline = command { "|" command } */
static enum parse_status parse_pipeline(struct parser *p, struct pipeline *pl)
{
    for (;;) {
        const struct token *tok = peek(p);
        enum parse_status status;

        if (!starts_command(tok->type)) {
            if (tok->type == TOK_END && pl->ncommands > 0) {
                snprintf(p->err, p->errlen, "line ends with '|'");
                return PARSE_INCOMPLETE;
            }
            return unexpected(p, tok);
        }
        pl->commands = xreallocarray(pl->commands, pl->ncommands + 1,
                                     sizeof *pl->commands);
        pl->commands[pl->ncommands] = (struct command){ 0 };
        status = parse_command(p, &pl->commands[pl->ncommands++]);
        if (status != PARSE_OK)
            return status;

        tok = peek(p);
        if (tok->type == TOK_END)
            return PARSE_OK;
        if (tok->type != TOK_PIPE)
            return unexpected(p, tok);
        p->pos++;
    }
}

enum parse_status parse_line(const char *line, struct pipeline **out,
                             char *err, size_t errlen)
{
    struct token_list tokens;
    struct parser p = { .tokens = &tokens, .err = err, .errlen = errlen };
    struct pipeline *pl;
    enum parse_status status;

    *out = NULL;
    if (lex(line, &tokens, err, errlen) != LEX_OK)
        return PARSE_INCOMPLETE;
    if (tokens.items[0].type == TOK_END) {
        token_list_free(&tokens);
        return PARSE_EMPTY;
    }

    pl = xcalloc(1, sizeof *pl);
    status = parse_pipeline(&p, pl);
    token_list_free(&tokens);
    if (status != PARSE_OK) {
        pipeline_free(pl);
        return status;
    }
    *out = pl;
    return PARSE_OK;
}

void pipeline_free(struct pipeline *pipeline)
{
    if (pipeline == NULL)
        return;
    for (size_t i = 0; i < pipeline->ncommands; i++) {
        struct command *cmd = &pipeline->commands[i];

        for (size_t w = 0; w < cmd->nwords; w++)
            free(cmd->words[w]);
        free(cmd->words);
        for (size_t a = 0; a < cmd->nassigns; a++)
            free(cmd->assigns[a]);
        free(cmd->assigns);
        for (size_t r = 0; r < cmd->nredirs; r++)
            free(cmd->redirs[r].target);
        free(cmd->redirs);
    }
    free(pipeline->commands);
    free(pipeline);
}
