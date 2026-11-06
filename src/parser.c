#include "parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void skip_newlines(struct parser *p)
{
    while (peek(p)->type == TOK_NEWLINE)
        p->pos++;
}

static enum parse_status unexpected(struct parser *p, const struct token *tok)
{
    switch (tok->type) {
    case TOK_AMP:
    case TOK_LPAREN:
        snprintf(p->err, p->errlen, "'%s' isn't supported yet", token_spelling(tok));
        break;
    case TOK_NEWLINE:
    case TOK_END:
        snprintf(p->err, p->errlen, "syntax error near end of line");
        break;
    default:
        snprintf(p->err, p->errlen, "syntax error near '%s'", token_spelling(tok));
        break;
    }
    return PARSE_SYNTAX;
}

/* After |, && or ||, newlines may follow before the next command. If only
 * newlines follow, the command goes on in the next line: incomplete. */
static bool continues_on_next_line(struct parser *p, const struct token *op)
{
    skip_newlines(p);
    if (peek(p)->type != TOK_END)
        return false;
    snprintf(p->err, p->errlen, "line ends with '%s'", token_spelling(op));
    return true;
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

static bool starts_command(enum token_type type)
{
    return type == TOK_WORD || token_is_redirection(type);
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
            return PARSE_OK;
        }
    }
}

/* pipeline = ["!"] command { "|" [newline] command } */
static enum parse_status parse_pipeline(struct parser *p, struct pipeline *pl)
{
    const struct token *tok = peek(p);

    /* ! is only special as a whole, unquoted word where a pipeline starts. */
    if (tok->type == TOK_WORD && strcmp(tok->text, "!") == 0) {
        pl->negate = true;
        p->pos++;
    }
    for (;;) {
        enum parse_status status;

        tok = peek(p);
        if (!starts_command(tok->type))
            return unexpected(p, tok);
        pl->commands = xreallocarray(pl->commands, pl->ncommands + 1,
                                     sizeof *pl->commands);
        pl->commands[pl->ncommands] = (struct command){ 0 };
        status = parse_command(p, &pl->commands[pl->ncommands++]);
        if (status != PARSE_OK)
            return status;

        tok = peek(p);
        if (tok->type != TOK_PIPE)
            return PARSE_OK;
        p->pos++;
        if (continues_on_next_line(p, tok))
            return PARSE_INCOMPLETE;
    }
}

/* and_or = pipeline { ( "&&" | "||" ) [newline] pipeline } */
static enum parse_status parse_and_or(struct parser *p, struct and_or *ao)
{
    enum run_if run_if = RUN_ALWAYS;

    for (;;) {
        struct pipeline *pl;
        const struct token *tok;
        enum parse_status status;

        ao->pipelines = xreallocarray(ao->pipelines, ao->npipelines + 1,
                                      sizeof *ao->pipelines);
        pl = &ao->pipelines[ao->npipelines++];
        *pl = (struct pipeline){ .run_if = run_if };
        status = parse_pipeline(p, pl);
        if (status != PARSE_OK)
            return status;

        tok = peek(p);
        if (tok->type == TOK_AND)
            run_if = RUN_IF_OK;
        else if (tok->type == TOK_OR)
            run_if = RUN_IF_FAILED;
        else
            return PARSE_OK;
        p->pos++;
        if (continues_on_next_line(p, tok))
            return PARSE_INCOMPLETE;
    }
}

/* program = [newline] [list] [newline], with list = and_or separated by ;
 * or newlines and an optional ; at the end. */
static enum parse_status parse_list(struct parser *p, struct list *list)
{
    skip_newlines(p);
    while (peek(p)->type != TOK_END) {
        enum parse_status status;
        const struct token *tok;

        list->items = xreallocarray(list->items, list->nitems + 1, sizeof *list->items);
        list->items[list->nitems] = (struct and_or){ 0 };
        status = parse_and_or(p, &list->items[list->nitems++]);
        if (status != PARSE_OK)
            return status;

        tok = peek(p);
        if (tok->type == TOK_SEMI) {
            p->pos++;
        } else if (tok->type != TOK_NEWLINE && tok->type != TOK_END) {
            return unexpected(p, tok);
        }
        skip_newlines(p);
    }
    return PARSE_OK;
}

enum parse_status parse_program(const char *text, struct list **out,
                                char *err, size_t errlen)
{
    struct token_list tokens;
    struct parser p = { .tokens = &tokens, .err = err, .errlen = errlen };
    struct list *list;
    enum parse_status status;

    *out = NULL;
    if (lex(text, &tokens, err, errlen) != LEX_OK)
        return PARSE_INCOMPLETE;

    list = xcalloc(1, sizeof *list);
    status = parse_list(&p, list);
    token_list_free(&tokens);
    if (status == PARSE_OK && list->nitems == 0)
        status = PARSE_EMPTY;
    if (status != PARSE_OK) {
        list_free(list);
        return status;
    }
    *out = list;
    return PARSE_OK;
}

static void command_free(struct command *cmd)
{
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

void list_free(struct list *list)
{
    if (list == NULL)
        return;
    for (size_t i = 0; i < list->nitems; i++) {
        struct and_or *ao = &list->items[i];

        for (size_t j = 0; j < ao->npipelines; j++) {
            struct pipeline *pl = &ao->pipelines[j];

            for (size_t c = 0; c < pl->ncommands; c++)
                command_free(&pl->commands[c]);
            free(pl->commands);
        }
        free(ao->pipelines);
    }
    free(list->items);
    free(list);
}
