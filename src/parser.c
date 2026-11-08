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
    const char *text; /* what was lexed, for the text of a background job */
    size_t pos;
    char *err;
    size_t errlen;
};

static const struct token *peek(const struct parser *p)
{
    return &p->tokens->items[p->pos];
}

/* The n bytes at s without blanks and newlines at either end, as a new
 * string. */
static char *trimmed(const char *s, size_t n)
{
    while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n'))
        n--;
    return xstrndup(s, n);
}

static void skip_newlines(struct parser *p)
{
    while (peek(p)->type == TOK_NEWLINE)
        p->pos++;
}

/* An unquoted reserved word: the token's text is exactly the word. A
 * quoted "if" keeps its quotes in the text, so it never matches. */
static bool is_reserved(const struct token *tok, const char *word)
{
    return tok->type == TOK_WORD && strcmp(tok->text, word) == 0;
}

/* The reserved words that end a list inside a compound command. */
static bool ends_list(const struct token *tok)
{
    static const char *const words[] = { "then", "elif", "else", "fi", "do", "done" };

    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (is_reserved(tok, words[i]))
            return true;
    }
    return false;
}

static enum parse_status unexpected(struct parser *p, const struct token *tok)
{
    switch (tok->type) {
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

static enum parse_status parse_list(struct parser *p, struct list *list, const char *closer);

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

/* A list inside a compound command, which must not be empty: `if; then`
 * and `then fi` are syntax errors. closer is the word it waits for. */
static enum parse_status parse_part(struct parser *p, struct list **list, const char *closer)
{
    enum parse_status status;

    *list = xcalloc(1, sizeof **list);
    status = parse_list(p, *list, closer);
    if (status == PARSE_OK && (*list)->nitems == 0)
        return unexpected(p, peek(p));
    return status;
}

/* The reserved word that has to come next, like the then after if cond. */
static enum parse_status expect(struct parser *p, const char *word)
{
    if (!is_reserved(peek(p), word))
        return unexpected(p, peek(p));
    p->pos++;
    return PARSE_OK;
}

/* if cond; then body { elif cond; then body } [ else body ] fi */
static enum parse_status parse_if(struct parser *p, struct command *cmd)
{
    enum parse_status status;

    p->pos++; /* if */
    for (;;) {
        struct if_branch *b;

        cmd->branches = xreallocarray(cmd->branches, cmd->nbranches + 1, sizeof *cmd->branches);
        b = &cmd->branches[cmd->nbranches++];
        *b = (struct if_branch){ 0 };
        if ((status = parse_part(p, &b->cond, "then")) != PARSE_OK
            || (status = expect(p, "then")) != PARSE_OK
            || (status = parse_part(p, &b->body, "fi")) != PARSE_OK)
            return status;
        if (!is_reserved(peek(p), "elif"))
            break;
        p->pos++;
    }
    if (is_reserved(peek(p), "else")) {
        p->pos++;
        if ((status = parse_part(p, &cmd->body, "fi")) != PARSE_OK)
            return status;
    }
    return expect(p, "fi");
}

/* while cond; do body; done, and until the same */
static enum parse_status parse_while(struct parser *p, struct command *cmd)
{
    enum parse_status status;

    p->pos++; /* while or until */
    if ((status = parse_part(p, &cmd->cond, "do")) != PARSE_OK
        || (status = expect(p, "do")) != PARSE_OK
        || (status = parse_part(p, &cmd->body, "done")) != PARSE_OK)
        return status;
    return expect(p, "done");
}

static enum parse_status missing(struct parser *p, const char *word)
{
    snprintf(p->err, p->errlen, "missing '%s'", word);
    return PARSE_INCOMPLETE;
}

/* for name [ in word ... ] ; do body; done
 * Without `in`, it loops over "$@": `for arg; do` or `for arg` and a
 * newline before the do. */
static enum parse_status parse_for(struct parser *p, struct command *cmd)
{
    const struct token *tok;
    enum parse_status status;

    p->pos++; /* for */
    tok = peek(p);
    if (tok->type == TOK_END || tok->type == TOK_NEWLINE)
        return tok->type == TOK_END ? missing(p, "do") : unexpected(p, tok);
    if (tok->type != TOK_WORD || !var_name_valid(tok->text)) {
        snprintf(p->err, p->errlen, "'%s' can't be a loop variable", token_spelling(tok));
        return PARSE_SYNTAX;
    }
    cmd->var = xstrdup(tok->text);
    p->pos++;
    if (peek(p)->type == TOK_SEMI) {
        p->pos++;
    } else {
        skip_newlines(p);
        if (is_reserved(peek(p), "in")) {
            cmd->has_in = true;
            p->pos++;
            while (peek(p)->type == TOK_WORD) {
                push_word(&cmd->words, &cmd->nwords, peek(p)->text);
                p->pos++;
            }
            tok = peek(p);
            if (tok->type == TOK_END)
                return missing(p, "do");
            if (tok->type != TOK_SEMI && tok->type != TOK_NEWLINE)
                return unexpected(p, tok);
            p->pos++;
        }
    }
    skip_newlines(p);
    if (peek(p)->type == TOK_END)
        return missing(p, "do");
    if ((status = expect(p, "do")) != PARSE_OK
        || (status = parse_part(p, &cmd->body, "done")) != PARSE_OK)
        return status;
    return expect(p, "done");
}

/* compound { redirection }: the redirections apply to the whole block, as
 * in `while read line; do ...; done < file`. */
static enum parse_status parse_compound(struct parser *p, struct command *cmd)
{
    const struct token *tok = peek(p);
    enum parse_status status;

    if (is_reserved(tok, "if")) {
        cmd->kind = CMD_IF;
        status = parse_if(p, cmd);
    } else if (is_reserved(tok, "for")) {
        cmd->kind = CMD_FOR;
        status = parse_for(p, cmd);
    } else {
        cmd->kind = is_reserved(tok, "while") ? CMD_WHILE : CMD_UNTIL;
        status = parse_while(p, cmd);
    }
    if (status != PARSE_OK)
        return status;
    while (token_is_redirection(peek(p)->type)) {
        const struct token *op = peek(p);
        const struct token *target = &p->tokens->items[p->pos + 1];

        if (target->type != TOK_WORD)
            return unexpected(p, target);
        add_redirection(cmd, op, target->text);
        p->pos += 2;
    }
    return PARSE_OK;
}

static bool starts_compound(const struct token *tok)
{
    return is_reserved(tok, "if") || is_reserved(tok, "while")
        || is_reserved(tok, "until") || is_reserved(tok, "for");
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
        if (!starts_command(tok->type) || ends_list(tok))
            return unexpected(p, tok);
        pl->commands = xreallocarray(pl->commands, pl->ncommands + 1,
                                     sizeof *pl->commands);
        pl->commands[pl->ncommands] = (struct command){ .line = tok->line };
        if (starts_compound(tok))
            status = parse_compound(p, &pl->commands[pl->ncommands++]);
        else
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

/* list = and_or separated by ; or newlines, with an optional ; at the end.
 * At the top (closer NULL) it runs to the end of the text. Inside a
 * compound it stops before the reserved word that ends it, and if the text
 * ends first the command isn't finished: "missing 'fi'". */
static enum parse_status parse_list(struct parser *p, struct list *list, const char *closer)
{
    skip_newlines(p);
    for (;;) {
        enum parse_status status;
        const struct token *tok = peek(p);

        if (tok->type == TOK_END)
            return closer ? missing(p, closer) : PARSE_OK;
        if (ends_list(tok))
            return closer ? PARSE_OK : unexpected(p, tok);

        struct and_or *ao;
        size_t start = tok->pos;

        list->items = xreallocarray(list->items, list->nitems + 1, sizeof *list->items);
        ao = &list->items[list->nitems++];
        *ao = (struct and_or){ 0 };
        status = parse_and_or(p, ao);
        if (status != PARSE_OK)
            return status;

        /* The text ends where its last token ends (always a word: a
         * command, an argument, a redirection's target or fi/done), so a
         * comment after it isn't part of it. */
        tok = &p->tokens->items[p->pos - 1];
        ao->text = trimmed(p->text + start, tok->pos + strlen(token_spelling(tok)) - start);
        tok = peek(p);
        if (tok->type == TOK_AMP) {
            if (ao->npipelines > 1 || ao->pipelines[0].negate) {
                snprintf(p->err, p->errlen,
                         "only a single pipeline can run in the background (for now)");
                return PARSE_SYNTAX;
            }
            ao->background = true;
            p->pos++;
        } else if (tok->type == TOK_SEMI) {
            p->pos++;
        } else if (tok->type != TOK_NEWLINE && tok->type != TOK_END && !ends_list(tok)) {
            return unexpected(p, tok);
        }
        skip_newlines(p);
    }
}

enum parse_status parse_program(const char *text, struct list **out,
                                char *err, size_t errlen)
{
    struct token_list tokens;
    struct parser p = { .tokens = &tokens, .text = text, .err = err, .errlen = errlen };
    struct list *list;
    enum parse_status status;

    *out = NULL;
    if (lex(text, &tokens, err, errlen) != LEX_OK)
        return PARSE_INCOMPLETE;

    list = xcalloc(1, sizeof *list);
    if (is_reserved(peek(&p), "explain")) {
        list->explain = true;
        p.pos++;
        if (peek(&p)->type == TOK_END || peek(&p)->type == TOK_NEWLINE) {
            snprintf(err, errlen, "explain: needs a command after it");
            token_list_free(&tokens);
            list_free(list);
            return PARSE_SYNTAX;
        }
    }
    status = parse_list(&p, list, NULL);
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
    for (size_t b = 0; b < cmd->nbranches; b++) {
        list_free(cmd->branches[b].cond);
        list_free(cmd->branches[b].body);
    }
    free(cmd->branches);
    list_free(cmd->cond);
    list_free(cmd->body);
    free(cmd->var);
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
        free(ao->text);
    }
    free(list->items);
    free(list);
}
