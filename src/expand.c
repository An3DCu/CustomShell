#include "expand.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pattern.h"
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
 * Pathname expansion uses the same marks: only FROM_WORD and FROM_SPLIT
 * characters can act as *, ? or [, so "*" and \* stay a plain *.
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

/* "$@" gives one word per parameter, even inside quotes, so the text has
 * to say where a word must end whatever IFS is: a break. In "$@" each
 * parameter is a word, even an empty one; in an unquoted $@ an empty one
 * disappears. */
struct field_break {
    size_t pos;
    bool quoted;
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
    struct field_break *breaks;
    size_t nbreaks;
    size_t breaks_cap;
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

static void mark_break(struct xbuf *b, bool quoted)
{
    if (b->nbreaks == b->breaks_cap) {
        b->breaks_cap = b->breaks_cap ? b->breaks_cap * 2 : 4;
        b->breaks = xreallocarray(b->breaks, b->breaks_cap, sizeof *b->breaks);
    }
    b->breaks[b->nbreaks++] = (struct field_break){ .pos = b->len, .quoted = quoted };
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
    free(b->breaks);
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

static bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return n > 0;
}

/* $1, ${10}: the parameter, or NULL if there are fewer. */
static const char *positional(const struct shell *sh, const char *digits, size_t n)
{
    size_t index = 0;

    for (size_t i = 0; i < n; i++) {
        if (index > sh->params.len)
            return NULL; /* far past the end, and no overflow */
        index = index * 10 + (size_t)(digits[i] - '0');
    }
    if (index == 0 || index > sh->params.len)
        return NULL;
    return sh->params.items[index - 1];
}

/* $?, $$, $#, $0 and $1 to $9 (or ${10} and up) into value, which may point
 * into buf. False if name isn't one of them; $@ and $* are handled apart. */
static bool special_value(const struct shell *sh, const char *name, size_t n,
                          char *buf, size_t size, const char **value)
{
    if (all_digits(name, n) && !(n > 1 && name[0] == '0')) {
        *value = name[0] == '0' ? (sh->name ? sh->name : "acsh")
                                : positional(sh, name, n);
        return true;
    }
    if (n != 1)
        return false;
    switch (name[0]) {
    case '!':
        if (sh->last_bg <= 0)
            return true; /* nothing in the background yet: empty, as in dash */
        snprintf(buf, size, "%ld", (long)sh->last_bg);
        *value = buf;
        return true;
    case '#':
        snprintf(buf, size, "%zu", sh->params.len);
        *value = buf;
        return true;
    case '?':
        snprintf(buf, size, "%d", sh->status);
        *value = buf;
        return true;
    case '$':
        snprintf(buf, size, "%ld", (long)sh->pid);
        *value = buf;
        return true;
    default:
        return false;
    }
}

/* What separates the parameters in "$*": the first character of IFS, a
 * space when IFS is unset, nothing when it's empty. */
static const char *star_separator(const struct shell *sh, char *buf)
{
    const char *ifs = vars_get(&sh->vars, "IFS");

    buf[0] = ifs == NULL ? ' ' : ifs[0];
    buf[1] = '\0';
    return buf;
}

/*
 * $@ and $*. Where the result becomes command words, $@ (and an unquoted
 * $*) gives one word per parameter, with breaks in between. "$*" and both
 * in an assignment or a redirection give one string, joined with a space
 * ($@) or the first character of IFS ($*).
 */
static void put_params(const struct shell *sh, struct xbuf *b, char which,
                       enum origin o, enum mode mode)
{
    bool words = mode == MODE_FIELDS && (which == '@' || o == FROM_SPLIT);
    char sep_buf[2];
    const char *sep = which == '*' ? star_separator(sh, sep_buf) : " ";

    for (size_t i = 0; i < sh->params.len; i++) {
        if (i > 0) {
            if (words)
                mark_break(b, o == FROM_QUOTED);
            else
                put_str(b, sep, o);
        }
        put_str(b, sh->params.items[i], o);
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
                          struct xbuf *b, enum origin o, enum mode mode,
                          char *err, size_t errlen)
{
    const char *p = word + *i + 1;
    const char *name;
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
        if (n == 1 && (name[0] == '@' || name[0] == '*')) {
            put_params(sh, b, name[0], o, mode);
            *i = (size_t)(close - word) + 1;
            return true;
        }
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
    } else if (*p == '@' || *p == '*') {
        put_params(sh, b, *p, o, mode);
        *i += 2;
        return true;
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
 * Steps 1, 2 and 5 in one pass over the word: quotes and backslashes are
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
                    if (!expand_dollar(sh, w, &i, b, FROM_QUOTED, mode, err, errlen))
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

            if (!expand_dollar(sh, w, &i, b, o, mode, err, errlen))
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

/* ---- step 4: pathname expansion ---------------------------------------- */

/*
 * One finished field, text[from, to). If it has an unquoted *, ? or [...]
 * and some file names match, those go to out instead of the field. With no
 * match the field stays as typed, like dash: `ls *.nope` passes "*.nope"
 * on, and ls says it doesn't exist.
 */
static void add_field(const struct xbuf *b, size_t from, size_t to, struct strvec *out)
{
    size_t n = to - from;
    unsigned char *active = xmalloc(n + 1);
    struct pattern p = { .text = b->text ? b->text + from : "", .active = active, .len = n };

    for (size_t i = 0; i < n; i++)
        active[i] = b->origin[from + i] != FROM_QUOTED;
    if (!pattern_has_magic(&p) || pattern_expand(&p, out) == 0)
        strvec_push(out, xbuf_string(b, from, to));
    free(active);
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
    size_t k = 0;     /* next entry in b->breaks */
    bool content = false;
    size_t i = 0;

    if (ifs == NULL)
        ifs = " \t\n";
    for (;;) {
        while (k < b->nbreaks && b->breaks[k].pos <= i) {
            if (content || b->breaks[k].quoted)
                add_field(b, start, i, out);
            start = i;
            content = b->breaks[k].quoted; /* "$@": even an empty one counts */
            k++;
        }
        while (q < b->nquotes && b->quotes[q] <= i) {
            content = true;
            q++;
        }
        if (i >= b->len)
            break;
        if (b->origin[i] == FROM_SPLIT && is_ifs(ifs, b->text[i])) {
            size_t run = i;
            bool hard = false; /* the separator has a non-space IFS character */

            while (i < b->len && b->origin[i] == FROM_SPLIT && is_ifs(ifs, b->text[i])) {
                if (i > run && q < b->nquotes && b->quotes[q] == i)
                    break; /* "" between two separators is a field of its own */
                if (i > run && k < b->nbreaks && b->breaks[k].pos == i)
                    break; /* so is the start of the next parameter */
                if (!is_ifs_space(ifs, b->text[i])) {
                    if (hard)
                        break;
                    hard = true;
                }
                i++;
            }
            if (hard || content)
                add_field(b, start, run, out);
            start = i;
            content = false;
            continue;
        }
        content = true;
        i++;
    }
    if (content)
        add_field(b, start, b->len, out);
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

static bool expand_list(struct shell *sh, char *const *words, size_t nwords,
                        bool declaration, struct strvec *out, char *err, size_t errlen)
{
    for (size_t i = 0; i < nwords; i++) {
        const char *w = words[i];
        size_t n = var_name_len(w);

        if (sh->params.len == 0 && (strcmp(w, "\"$@\"") == 0 || strcmp(w, "\"${@}\"") == 0))
            continue; /* "$@" with no parameters: not even an empty word */
        if (declaration && i > 0 && n > 0 && w[n] == '=') {
            char *value = expand_assignment(sh, w + n + 1, err, errlen);
            char *arg;

            if (value == NULL)
                return false;
            arg = xmalloc(n + 1 + strlen(value) + 1);
            memcpy(arg, w, n + 1);
            memcpy(arg + n + 1, value, strlen(value) + 1);
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

bool expand_command(struct shell *sh, char *const *words, size_t nwords,
                    struct strvec *out, char *err, size_t errlen)
{
    /* Only when typed as plain `export`: POSIX calls it a declaration
     * utility, and dash and bash both do this. */
    bool declaration = nwords > 0 && strcmp(words[0], "export") == 0;

    return expand_list(sh, words, nwords, declaration, out, err, errlen);
}

bool expand_words(struct shell *sh, char *const *words, size_t nwords,
                  struct strvec *out, char *err, size_t errlen)
{
    return expand_list(sh, words, nwords, false, out, err, errlen);
}
