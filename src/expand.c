#include "expand.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vars.h"
#include "xalloc.h"

/*
 * While a word expands, each character remembers where it came from. That
 * decides what the later steps may do with it:
 *
 *   FROM_WORD    typed unquoted in the word: never split
 *   FROM_SPLIT   from an unquoted $X: split on IFS
 *   FROM_QUOTED  inside quotes, escaped, or from ~: left alone
 *
 * Pathname expansion (*, ?) will use the same marks: only FROM_WORD and
 * FROM_SPLIT characters can act as patterns.
 */
enum origin {
    FROM_WORD,
    FROM_SPLIT,
    FROM_QUOTED,
};

enum mode {
    MODE_FIELDS, /* command words: split into fields */
    MODE_ONE,    /* redirection targets: one string */
    MODE_ASSIGN, /* assignment values: one string, ~ after : too */
};

/* The text so far, with an origin per byte. quotes[] holds every position
 * where a pair of quotes started: "" adds no bytes, but still means "there
 * is a word here", so `""` is one empty argument while an empty $X is none. */
struct xbuf {
    char *text;
    unsigned char *origin;
    size_t len;
    size_t cap;
    size_t *quotes;
    size_t nquotes;
    size_t quotes_cap;
};

static void put(struct xbuf *b, char c, enum origin o)
{
    if (b->len + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->text = xreallocarray(b->text, b->cap, 1);
        b->origin = xreallocarray(b->origin, b->cap, 1);
    }
    b->text[b->len] = c;
    b->origin[b->len] = (unsigned char)o;
    b->len++;
}

static void put_str(struct xbuf *b, const char *s, enum origin o)
{
    for (; *s != '\0'; s++)
        put(b, *s, o);
}

static void mark_quotes(struct xbuf *b)
{
    if (b->nquotes == b->quotes_cap) {
        b->quotes_cap = b->quotes_cap ? b->quotes_cap * 2 : 4;
        b->quotes = xreallocarray(b->quotes, b->quotes_cap, sizeof *b->quotes);
    }
    b->quotes[b->nquotes++] = b->len;
}

static char *xbuf_string(const struct xbuf *b, size_t from, size_t to)
{
    return xstrndup(b->text ? b->text + from : "", to - from);
}

static void xbuf_free(struct xbuf *b)
{
    free(b->text);
    free(b->origin);
    free(b->quotes);
    *b = (struct xbuf){ 0 };
}

/* ---- step 1: tilde ------------------------------------------------------ */

/*
 * At word[*i]: a ~ followed by a login name (maybe empty) up to the first /
 * (or : in an assignment). Every character of the prefix must be unquoted,
 * so "~" and ~"x" stay as typed. An unset HOME or an unknown user also
 * leaves the word alone, like dash.
 */
static void expand_tilde(const struct shell *sh, const char *word, size_t *i,
                         struct xbuf *b, enum mode mode)
{
    size_t j = *i + 1;
    const char *home;

    if (word[*i] != '~')
        return;
    while (word[j] != '\0' && word[j] != '/' && !(mode == MODE_ASSIGN && word[j] == ':')) {
        if (word[j] == '\'' || word[j] == '"' || word[j] == '\\')
            return;
        j++;
    }
    if (j == *i + 1) {
        home = vars_get(&sh->vars, "HOME");
    } else {
        char *login = xstrndup(word + *i + 1, j - *i - 1);
        const struct passwd *pw = getpwnam(login);

        free(login);
        home = pw ? pw->pw_dir : NULL;
    }
    if (home == NULL)
        return;
    put_str(b, home, FROM_QUOTED);
    *i = j;
}

/* ---- step 2: parameters ------------------------------------------------- */

/* $?, $$ and $0 into buf; false if name isn't one of them. */
static bool special_value(const struct shell *sh, const char *name, size_t n,
                          char *buf, size_t size, const char **value)
{
    if (n != 1)
        return false;
    switch (name[0]) {
    case '?':
        snprintf(buf, size, "%d", sh->status);
        *value = buf;
        return true;
    case '$':
        snprintf(buf, size, "%ld", (long)sh->pid);
        *value = buf;
        return true;
    case '0':
        *value = sh->name ? sh->name : "acsh";
        return true;
    default:
        return false;
    }
}

/* The value of the variable whose name is the n bytes at name, or NULL. */
static const char *lookup(const struct shell *sh, const char *name, size_t n)
{
    char *copy = xstrndup(name, n);
    const char *value = vars_get(&sh->vars, copy);

    free(copy);
    return value;
}

/*
 * At word[*i], which is a $. Adds the value with origin o (FROM_SPLIT outside
 * quotes, FROM_QUOTED inside), or the $ itself when no name follows, as in
 * `echo $` or `echo 5$`. An unset variable adds nothing.
 */
static bool expand_dollar(const struct shell *sh, const char *word, size_t *i,
                          struct xbuf *b, enum origin o, char *err, size_t errlen)
{
    const char *p = word + *i + 1;
    const char *name = p;
    const char *value = NULL;
    char num[32];
    size_t n;

    if (*p == '(') {
        snprintf(err, errlen, "'$(' isn't supported yet");
        return false;
    }
    if (*p == '{') {
        const char *close = strchr(p, '}');

        if (close == NULL) {
            snprintf(err, errlen, "missing '}' after '${'");
            return false;
        }
        name = p + 1;
        n = (size_t)(close - name);
        if (!special_value(sh, name, n, num, sizeof num, &value)) {
            if (n == 0 || var_name_len(name) != n) {
                snprintf(err, errlen, "${%.*s}: bad substitution", (int)n, name);
                return false;
            }
            value = lookup(sh, name, n);
        }
        *i = (size_t)(close - word) + 1;
    } else if ((n = var_name_len(p)) > 0) {
        value = lookup(sh, p, n);
        *i += 1 + n;
    } else if (special_value(sh, p, 1, num, sizeof num, &value)) {
        *i += 2;
    } else {
        put(b, '$', o == FROM_QUOTED ? FROM_QUOTED : FROM_WORD);
        *i += 1;
        return true;
    }
    if (value != NULL)
        put_str(b, value, o);
    return true;
}

/* ---- scanning a word ---------------------------------------------------- */

/* Inside double quotes, a backslash only escapes these. */
static bool escapable_in_double_quotes(char c)
{
    return c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n';
}

static bool backquote_error(char *err, size_t errlen)
{
    snprintf(err, errlen, "'`' isn't supported yet");
    return false;
}

/*
 * Steps 1, 2 and 4 in one pass over the word: quotes and backslashes are
 * read and dropped, ~ and $ are replaced, and every byte that goes into b is
 * marked with where it came from. The quoting rules are POSIX's:
 *   'single'   everything inside is literal
 *   "double"   $ still expands; a backslash only escapes $ ` " \ and newline
 *   \x         outside quotes, x is literal
 *   \newline   removed entirely (the line continues)
 */
static bool scan_word(const struct shell *sh, const char *w, struct xbuf *b,
                      enum mode mode, char *err, size_t errlen)
{
    size_t i = 0;

    expand_tilde(sh, w, &i, b, mode);
    while (w[i] != '\0') {
        char c = w[i];

        if (c == '\'') {
            mark_quotes(b);
            i++;
            while (w[i] != '\'' && w[i] != '\0')
                put(b, w[i++], FROM_QUOTED);
            if (w[i] != '\0')
                i++;
        } else if (c == '"') {
            mark_quotes(b);
            i++;
            while (w[i] != '"' && w[i] != '\0') {
                if (w[i] == '\\' && escapable_in_double_quotes(w[i + 1])) {
                    if (w[i + 1] != '\n')
                        put(b, w[i + 1], FROM_QUOTED);
                    i += 2;
                } else if (w[i] == '$') {
                    if (!expand_dollar(sh, w, &i, b, FROM_QUOTED, err, errlen))
                        return false;
                } else if (w[i] == '`') {
                    return backquote_error(err, errlen);
                } else {
                    put(b, w[i++], FROM_QUOTED);
                }
            }
            if (w[i] != '\0')
                i++;
        } else if (c == '\\' && w[i + 1] != '\0') {
            if (w[i + 1] != '\n')
                put(b, w[i + 1], FROM_QUOTED);
            i += 2;
        } else if (c == '$') {
            enum origin o = mode == MODE_FIELDS ? FROM_SPLIT : FROM_QUOTED;

            if (!expand_dollar(sh, w, &i, b, o, err, errlen))
                return false;
        } else if (c == '`') {
            return backquote_error(err, errlen);
        } else if (c == ':' && mode == MODE_ASSIGN) {
            put(b, c, FROM_WORD);
            i++;
            expand_tilde(sh, w, &i, b, mode);
        } else {
            put(b, c, FROM_WORD);
            i++;
        }
    }
    return true;
}

/* ---- step 3: field splitting -------------------------------------------- */

static bool is_ifs(const char *ifs, char c)
{
    return c != '\0' && strchr(ifs, c) != NULL;
}

static bool is_ifs_space(const char *ifs, char c)
{
    return (c == ' ' || c == '\t' || c == '\n') && is_ifs(ifs, c);
}

/*
 * POSIX field splitting, on FROM_SPLIT bytes only:
 *   - IFS spaces, tabs and newlines in a row are one separator, and at the
 *     start or end of the text they make no empty field
 *   - any other IFS character is a separator on its own, so with IFS=:
 *     "a::b" gives a, "" and b, but a trailing ":" adds nothing
 *   - unset IFS means space, tab and newline; an empty IFS splits nothing
 * A field is kept if it has bytes or quotes, so `""` stays and an empty $X
 * goes.
 */
static void split_fields(const struct shell *sh, const struct xbuf *b, struct strvec *out)
{
    const char *ifs = vars_get(&sh->vars, "IFS");
    size_t start = 0; /* where the current field starts */
    size_t q = 0;     /* next entry in b->quotes */
    bool content = false;
    size_t i = 0;

    if (ifs == NULL)
        ifs = " \t\n";
    while (i < b->len) {
        while (q < b->nquotes && b->quotes[q] <= i) {
            content = true;
            q++;
        }
        if (b->origin[i] == FROM_SPLIT && is_ifs(ifs, b->text[i])) {
            size_t run = i;
            bool hard = false; /* the separator has a non-space IFS character */

            while (i < b->len && b->origin[i] == FROM_SPLIT && is_ifs(ifs, b->text[i])) {
                if (i > run && q < b->nquotes && b->quotes[q] == i)
                    break; /* "" between two separators is a field of its own */
                if (!is_ifs_space(ifs, b->text[i])) {
                    if (hard)
                        break;
                    hard = true;
                }
                i++;
            }
            if (hard || content)
                strvec_push(out, xbuf_string(b, start, run));
            start = i;
            content = false;
            continue;
        }
        content = true;
        i++;
    }
    if (q < b->nquotes)
        content = true;
    if (content)
        strvec_push(out, xbuf_string(b, start, b->len));
}

/* ---- the public functions ----------------------------------------------- */

static char *expand_string(struct shell *sh, const char *word, enum mode mode,
                           char *err, size_t errlen)
{
    struct xbuf b = { 0 };
    char *s = NULL;

    if (scan_word(sh, word, &b, mode, err, errlen))
        s = xbuf_string(&b, 0, b.len);
    xbuf_free(&b);
    return s;
}

char *expand_one(struct shell *sh, const char *word, char *err, size_t errlen)
{
    return expand_string(sh, word, MODE_ONE, err, errlen);
}

char *expand_assignment(struct shell *sh, const char *value, char *err, size_t errlen)
{
    return expand_string(sh, value, MODE_ASSIGN, err, errlen);
}

bool expand_command(struct shell *sh, char *const *words, size_t nwords,
                    struct strvec *out, char *err, size_t errlen)
{
    /* Only when typed as plain `export`: POSIX calls it a declaration
     * utility, and dash and bash both do this. */
    bool declaration = nwords > 0 && strcmp(words[0], "export") == 0;

    for (size_t i = 0; i < nwords; i++) {
        const char *w = words[i];
        size_t n = var_name_len(w);

        if (declaration && i > 0 && n > 0 && w[n] == '=') {
            char *value = expand_assignment(sh, w + n + 1, err, errlen);
            char *arg;

            if (value == NULL)
                return false;
            arg = xmalloc(n + 1 + strlen(value) + 1);
            memcpy(arg, w, n + 1);
            strcpy(arg + n + 1, value);
            free(value);
            strvec_push(out, arg);
        } else {
            struct xbuf b = { 0 };
            bool ok = scan_word(sh, w, &b, MODE_FIELDS, err, errlen);

            if (ok)
                split_fields(sh, &b, out);
            xbuf_free(&b);
            if (!ok)
                return false;
        }
    }
    return true;
}
