#include "editor.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "complete.h"
#include "history.h"
#include "input.h"
#include "signals.h"
#include "xalloc.h"

/* In raw mode Ctrl+letter arrives as the letter's code with the top bits
 * cleared: Ctrl+A is 1. */
#define CTRL_KEY(c) ((c) & 0x1f)
#define ESC 27
#define BACKSPACE 127

/* Keys that come as escape sequences, numbered past every byte. */
enum {
    KEY_NONE = 256, /* a sequence acsh doesn't use */
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_BACKTAB, /* Shift+Tab */
};

struct edit {
    int fd;
    const struct editor_config *cfg;
    const char *prompt;
    size_t plen;
    char *text;  /* the line so far, without a final 0 */
    size_t len;
    size_t cap;
    size_t pos;  /* the cursor, as an offset into text */
    size_t row;  /* the screen row of the cursor, counted from the prompt's */
    int utf8_left; /* bytes still to come of a UTF-8 character being typed */
    bool stale;    /* the screen is behind the text after a put-off redraw */
    const struct history *hist; /* for Up, Down and Ctrl+R, or NULL */
    size_t hist_at; /* the entry shown, or hist->len for the line being typed */
    char *draft;    /* the line being typed, kept while Up shows older ones */
    size_t draft_len;
    char *out;   /* what to write next, sent in one write() to avoid flicker */
    size_t out_len;
    size_t out_cap;
    struct matches comp; /* Tab: the matches being gone through */
    bool cycling;        /* Tab and Shift+Tab go through comp */
    size_t comp_at;      /* the match shown */
    size_t comp_start;   /* where it is in the text, and how long */
    size_t comp_len;
};

bool editor_usable(const char *term)
{
    return term != NULL && term[0] != '\0' && strcmp(term, "dumb") != 0;
}

/* memmove that's fine with n == 0: an empty line has no text buffer yet,
 * and passing NULL to memmove is undefined even for nothing to copy. */
static void copy(void *to, const void *from, size_t n)
{
    if (n > 0)
        memmove(to, from, n);
}

/* ---- output ------------------------------------------------------------- */

static void put(struct edit *e, const char *s, size_t n)
{
    if (e->out_len + n > e->out_cap) {
        e->out_cap = (e->out_len + n) * 2;
        e->out = xreallocarray(e->out, e->out_cap, 1);
    }
    copy(e->out + e->out_len, s, n);
    e->out_len += n;
}

static void puts_out(struct edit *e, const char *s)
{
    put(e, s, strlen(s));
}

/* ESC [ n letter: move the cursor n rows up (A) or n columns right (C). */
static void move(struct edit *e, size_t n, char letter)
{
    char seq[32];

    if (n == 0)
        return;
    snprintf(seq, sizeof seq, "\033[%zu%c", n, letter);
    puts_out(e, seq);
}

static void flush(struct edit *e)
{
    size_t done = 0;

    while (done < e->out_len) {
        ssize_t n = write(STDERR_FILENO, e->out + done, e->out_len - done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; /* nothing to be done about a broken terminal */
        done += (size_t)n;
    }
    e->out_len = 0;
}

/* The terminal's width. It's asked each time, for a window that gets resized. */
static size_t columns(void)
{
    struct winsize ws;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/*
 * Draws the prompt's last line and the text again, and puts the cursor at
 * pos. A long line wraps over several rows. Drawing starts at the prompt's
 * row. Everything below is cleared, then it's all written out. The terminal
 * leaves the cursor after the end. From there it moves up and right to pos.
 */
static void redraw(struct edit *e)
{
    size_t cols = columns();
    size_t end = e->plen + e->len;
    size_t at = e->plen + e->pos;

    move(e, e->row, 'A');
    puts_out(e, "\r\033[J");
    puts_out(e, e->prompt);
    put(e, e->text, e->len);
    /* Text that ends right at the edge leaves the cursor waiting on that
     * row. A newline takes it to the next one, where the counting says. */
    if (end % cols == 0)
        puts_out(e, "\n");
    move(e, end / cols - at / cols, 'A');
    puts_out(e, "\r");
    move(e, at % cols, 'C');
    e->row = at / cols;
    e->stale = false;
    flush(e);
}

/* Whether more input is already there, like the rest of a paste. */
static bool input_waiting(const struct edit *e)
{
    struct pollfd p = { .fd = e->fd, .events = POLLIN };

    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

/* A paste arrives as many keys at once. Drawing the whole line after each
 * one takes time that grows with the square of its length. That's minutes
 * for a few pages. While more keys are waiting, the drawing waits too. */
static void redraw_when_idle(struct edit *e)
{
    if (input_waiting(e))
        e->stale = true;
    else
        redraw(e);
}

/* ---- keys --------------------------------------------------------------- */

/* One byte from the terminal. While it waits, finished jobs are collected,
 * as at the plain prompt. false at the end of input or for SIGINT sent from
 * outside, with errno set as for input_read_line. */
static bool byte(struct edit *e, unsigned char *c)
{
    for (;;) {
        ssize_t n;

        if (!signals_wait_for_input(e->fd))
            return false; /* errno is EINTR */
        n = read(e->fd, c, 1);
        if (n == 1)
            return true;
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            errno = 0;
        return false;
    }
}

/* After ESC [: digits and ;, then one final byte. Home and End have three
 * spellings between terminals. */
static int csi_key(struct edit *e)
{
    unsigned char c;
    int n = 0;

    for (;;) {
        if (!byte(e, &c))
            return KEY_NONE;
        if (c >= '0' && c <= '9')
            n = n * 10 + (c - '0');
        else if (c != ';')
            break;
    }
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'Z': return KEY_BACKTAB;
    case '~':
        if (n == 1 || n == 7)
            return KEY_HOME;
        if (n == 4 || n == 8)
            return KEY_END;
        if (n == 3)
            return KEY_DELETE;
        return KEY_NONE;
    default:
        return KEY_NONE;
    }
}

/* The next key: a byte, or one of the KEY_ values. -1 when no key will come. */
static int next_key(struct edit *e)
{
    unsigned char c;

    if (!byte(e, &c))
        return -1;
    if (c != ESC)
        return c;
    if (!byte(e, &c))
        return -1;
    if (c == '[')
        return csi_key(e);
    if (c == 'O') { /* what some terminals send for the same keys */
        if (!byte(e, &c))
            return -1;
        switch (c) {
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: return KEY_NONE;
        }
    }
    return KEY_NONE; /* Alt+something */
}

/* ---- editing ------------------------------------------------------------ */

/* Puts n bytes of s in place of the text, with the cursor at the end. */
static void set_text(struct edit *e, const char *s, size_t n)
{
    if (n > e->cap) {
        e->cap = n;
        e->text = xreallocarray(e->text, e->cap, 1);
    }
    copy(e->text, s, n);
    e->len = e->pos = n;
}

/* Up (older) or Down (newer). An entry is only ever copied into the line. A
 * change made to the copy is gone when another one is shown. History stays
 * as it was. The line being typed before the first Up is the draft, shown
 * again after the newest entry. */
static void history_step(struct edit *e, bool older)
{
    const struct history *h = e->hist;

    if (h == NULL || (older ? e->hist_at == 0 : e->hist_at >= h->len))
        return;
    if (e->hist_at == h->len) {
        free(e->draft);
        e->draft = xmalloc(e->len + 1);
        copy(e->draft, e->text, e->len);
        e->draft_len = e->len;
    }
    e->hist_at += older ? (size_t)-1 : 1;
    if (e->hist_at == h->len)
        set_text(e, e->draft, e->draft_len);
    else
        set_text(e, h->lines[e->hist_at], strlen(h->lines[e->hist_at]));
}

/*
 * Ctrl+R. The query is typed into the prompt's place, `search: sle  ->
 * sleep 30 &`. Each key looks again, from the newest entry back. Ctrl+R
 * again goes to the next older entry that's different. Returns the key to
 * act on once the search is over. Enter runs what's shown. Ctrl+C drops it.
 * An arrow key keeps it for editing. Ctrl+G returns 0 and brings back the
 * line from before the search.
 */
static int search(struct edit *e)
{
    const struct history *h = e->hist;
    const char *prompt = e->prompt;
    size_t plen = e->plen;
    char *before = xmalloc(e->len + 1);
    size_t before_len = e->len;
    char query[256] = "";
    size_t qlen = 0;
    size_t at = h->len; /* the entry shown, or h->len for none */
    char *shown = NULL;
    int key;

    copy(before, e->text, e->len);
    for (;;) {
        size_t from;

        free(shown);
        if (at < h->len) {
            shown = xmalloc(qlen + 32);
            snprintf(shown, qlen + 32, "search: %s  ->  ", query);
            set_text(e, h->lines[at], strlen(h->lines[at]));
        } else {
            shown = xmalloc(qlen + 32);
            snprintf(shown, qlen + 32, qlen ? "search: %s  (no match)" : "search: %s", query);
            set_text(e, "", 0);
        }
        e->prompt = shown;
        e->plen = strlen(shown);
        e->pos = 0;
        redraw(e);

        key = next_key(e);
        if (key == CTRL_KEY('R')) {
            from = at; /* look further back than the one shown */
        } else if (key >= ' ' && key < 256 && key != BACKSPACE) {
            if (qlen + 1 < sizeof query)
                query[qlen++] = (char)key;
            query[qlen] = '\0';
            from = at < h->len ? at + 1 : h->len; /* the one shown may still match */
        } else if (key == BACKSPACE || key == CTRL_KEY('H')) {
            if (qlen > 0)
                query[--qlen] = '\0';
            from = h->len;
        } else if (key == CTRL_KEY('G')) {
            set_text(e, before, before_len);
            key = 0;
            break;
        } else if (key == '\r' || key == '\n' || key == CTRL_KEY('C') || key == -1
                   || key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN
                   || key == KEY_HOME || key == KEY_END) {
            break;
        } else {
            continue; /* other keys do nothing during a search */
        }
        /* Look back from just before `from` for the first entry with the
         * query in it, skipping one that reads the same as the one shown. */
        {
            const char *was = at < h->len ? h->lines[at] : NULL;
            size_t i = from;

            at = h->len;
            while (i-- > 0) {
                if (strstr(h->lines[i], query) != NULL
                    && (key != CTRL_KEY('R') || was == NULL || strcmp(h->lines[i], was) != 0)) {
                    at = i;
                    break;
                }
            }
            if (key == CTRL_KEY('R') && at == h->len && was != NULL)
                at = from; /* nothing older: stay on the one shown */
        }
    }
    if (at == h->len && key != '\r' && key != '\n' && key != CTRL_KEY('C'))
        set_text(e, before, before_len); /* nothing found: the line as it was */
    e->prompt = prompt;
    e->plen = plen;
    e->pos = e->len;
    /* Up and Down go on from the entry found. The line from before the
     * search is the one after the newest. */
    e->hist_at = at;
    free(e->draft);
    e->draft = before;
    e->draft_len = before_len;
    free(shown);
    return key;
}

static void insert(struct edit *e, char c)
{
    if (e->len == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 128;
        e->text = xreallocarray(e->text, e->cap, 1);
    }
    copy(e->text + e->pos + 1, e->text + e->pos, e->len - e->pos);
    e->text[e->pos++] = c;
    e->len++;
}

/* Whether c leaves a UTF-8 character unfinished. A first byte 110xxxxx is
 * followed by one more, 1110xxxx by two, 11110xxx by three, each 10xxxxxx. */
static bool utf8_part(struct edit *e, unsigned char c)
{
    if ((c & 0xE0) == 0xC0)
        e->utf8_left = 1;
    else if ((c & 0xF0) == 0xE0)
        e->utf8_left = 2;
    else if ((c & 0xF8) == 0xF0)
        e->utf8_left = 3;
    else if ((c & 0xC0) == 0x80 && e->utf8_left > 0)
        e->utf8_left--;
    else
        e->utf8_left = 0;
    return e->utf8_left > 0;
}

/* Removes the text from from to to (from <= to) and puts the cursor there. */
static void cut(struct edit *e, size_t from, size_t to)
{
    copy(e->text + from, e->text + to, e->len - to);
    e->len -= to - from;
    e->pos = from;
}

/* Where the word before the cursor starts: back over blanks, then over
 * everything else up to the next blank, as Ctrl+W does in bash. */
static size_t word_start(const struct edit *e)
{
    size_t i = e->pos;

    while (i > 0 && e->text[i - 1] == ' ')
        i--;
    while (i > 0 && e->text[i - 1] != ' ')
        i--;
    return i;
}

/* ---- Tab ---------------------------------------------------------------- */

/* Puts s in place of the text from start to end, the cursor after it. */
static void replace(struct edit *e, size_t start, size_t end, const char *s)
{
    cut(e, start, end);
    for (; *s != '\0'; s++)
        insert(e, *s);
}

/* The matches under the line, in columns down the page like ls, at most
 * 100 of them. The line is drawn again after them. */
static void show_list(struct edit *e)
{
    const struct matches *m = &e->comp;
    size_t shown = m->len < 100 ? m->len : 100;
    size_t widest = 0;
    size_t cols;
    size_t rows;
    size_t pos = e->pos;

    e->pos = e->len; /* the list goes under the whole line */
    redraw(e);
    e->pos = pos;
    for (size_t i = 0; i < shown; i++) {
        size_t w = strlen(m->items[i].show);

        if (w > widest)
            widest = w;
    }
    cols = columns() / (widest + 2);
    if (cols == 0)
        cols = 1;
    rows = (shown + cols - 1) / cols;
    puts_out(e, "\n");
    for (size_t r = 0; r < rows; r++) {
        for (size_t col = 0; col < cols; col++) {
            size_t i = col * rows + r;

            if (i >= shown)
                break;
            puts_out(e, m->items[i].show);
            if (i + rows < shown) /* another column follows on this row */
                for (size_t pad = strlen(m->items[i].show); pad < widest + 2; pad++)
                    puts_out(e, " ");
        }
        puts_out(e, "\n");
    }
    if (m->len > shown) {
        char more[64];

        snprintf(more, sizeof more, "and %zu more\n", m->len - shown);
        puts_out(e, more);
    }
    flush(e);
    e->row = 0; /* the prompt starts again here */
}

static void stop_cycling(struct edit *e)
{
    complete_free(&e->comp);
    e->cycling = false;
}

/* The next match (or with back, the one before) in place of the shown one. */
static void cycle(struct edit *e, bool back)
{
    size_t n = e->comp.len;
    const char *insert;

    e->comp_at = back ? (e->comp_at + n - 1) % n : (e->comp_at + 1) % n;
    insert = e->comp.items[e->comp_at].insert;
    replace(e, e->comp_start, e->comp_start + e->comp_len, insert);
    e->comp_len = strlen(insert);
}

/* A key other than Tab keeps the match shown. A file or command gets its
 * space then. A folder doesn't. */
static void keep_match(struct edit *e)
{
    if (!e->comp.items[e->comp_at].dir)
        insert(e, ' ');
    stop_cycling(e);
}

static void complete(struct edit *e)
{
    size_t start = complete_word_start(e->text, e->pos);
    const struct match *first;

    complete_find(e->text + start, e->pos - start, complete_is_command(e->text, start),
                  e->cfg->path, e->cfg->home, &e->comp);
    if (e->comp.len == 0) {
        stop_cycling(e); /* nothing fits: nothing happens */
        return;
    }
    first = &e->comp.items[0];
    if (e->comp.len == 1) {
        replace(e, start, e->pos, first->insert);
        if (!first->dir)
            insert(e, ' ');
        stop_cycling(e);
        return;
    }
    show_list(e);
    replace(e, start, e->pos, first->insert);
    e->cycling = true;
    e->comp_at = 0;
    e->comp_start = start;
    e->comp_len = strlen(first->insert);
}

/* Does what key asks. Returns 1 when the line is done (Enter), -1 when it
 * isn't coming (Ctrl+C, Ctrl+D on an empty line, end of input), else 0. */
static int handle(struct edit *e, int key)
{
    if (e->cycling && key != '\t' && key != KEY_BACKTAB)
        keep_match(e);
    switch (key) {
    case '\t':
        if (e->cycling)
            cycle(e, false);
        else
            complete(e);
        break;
    case KEY_BACKTAB:
        if (e->cycling)
            cycle(e, true);
        break;
    case '\r':
    case '\n':
        e->pos = e->len;
        redraw(e);
        puts_out(e, "\n");
        flush(e);
        return 1;
    case CTRL_KEY('C'):
        e->pos = e->len;
        redraw(e);
        puts_out(e, "^C"); /* the shell adds the newline, as at the plain prompt */
        flush(e);
        errno = EINTR;
        return -1;
    case CTRL_KEY('D'):
        if (e->len == 0) {
            errno = 0;
            return -1;
        }
        if (e->pos < e->len) /* a line with text in it: like Delete */
            cut(e, e->pos, e->pos + 1);
        break;
    case KEY_DELETE:
        if (e->pos < e->len)
            cut(e, e->pos, e->pos + 1);
        break;
    case BACKSPACE:
    case CTRL_KEY('H'):
        if (e->pos > 0)
            cut(e, e->pos - 1, e->pos);
        break;
    case KEY_LEFT:
        if (e->pos > 0)
            e->pos--;
        break;
    case KEY_RIGHT:
        if (e->pos < e->len)
            e->pos++;
        break;
    case KEY_HOME:
    case CTRL_KEY('A'):
        e->pos = 0;
        break;
    case KEY_END:
    case CTRL_KEY('E'):
        e->pos = e->len;
        break;
    case CTRL_KEY('K'):
        e->len = e->pos;
        break;
    case CTRL_KEY('U'):
        cut(e, 0, e->pos);
        break;
    case CTRL_KEY('W'):
        cut(e, word_start(e), e->pos);
        break;
    case KEY_UP:
    case KEY_DOWN:
        history_step(e, key == KEY_UP);
        break;
    case CTRL_KEY('R'):
        if (e->hist == NULL)
            return 0;
        key = search(e);
        redraw(e);
        return key == 0 ? 0 : handle(e, key);
    case CTRL_KEY('L'):
        puts_out(e, "\033[H\033[2J");
        flush(e);
        if (e->cfg->repaint != NULL)
            e->cfg->repaint(e->cfg->arg);
        else
            fputs(e->prompt, stderr);
        fflush(stderr);
        e->row = 0;
        break;
    case -1:
        return -1;
    default:
        /* text, UTF-8 bytes too. Other control keys and unknown sequences
         * do nothing. */
        if (key < ' ' || key >= 256 || key == BACKSPACE)
            return 0;
        insert(e, (char)key);
        if (utf8_part(e, (unsigned char)key))
            return 0; /* wait for the rest. Half a letter would show as junk. */
    }
    redraw_when_idle(e);
    return 0;
}

ssize_t editor_read_line(int fd, const struct editor_config *cfg, char **line, size_t *cap)
{
    struct edit e = {
        .fd = fd,
        .cfg = cfg,
        .prompt = cfg->prompt,
        .plen = strlen(cfg->prompt),
        .hist = cfg->hist,
        .hist_at = cfg->hist ? cfg->hist->len : 0,
        /* never NULL. Pointer sums like text + pos are then always defined. */
        .text = xmalloc(128),
        .cap = 128,
        .out = xmalloc(256),
        .out_cap = 256,
    };
    struct termios cooked, raw;
    int done = 0;
    int saved_errno;

    if (tcgetattr(fd, &cooked) != 0) {
        free(e.text);
        free(e.out);
        return input_read_line(fd, line, cap, true);
    }
    raw = cooked;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    /* TCSADRAIN keeps the keys typed ahead for this line. TCSAFLUSH would
     * drop them. */
    signals_guard_terminal(fd, &cooked);
    tcsetattr(fd, TCSADRAIN, &raw);

    while (done == 0) {
        if (e.stale && !input_waiting(&e))
            redraw(&e); /* the paste is over, or a key ended it without a redraw */
        done = handle(&e, next_key(&e));
    }

    saved_errno = errno;
    tcsetattr(fd, TCSADRAIN, &cooked);
    signals_unguard_terminal();
    free(e.out);
    free(e.draft);
    complete_free(&e.comp);
    if (done < 0) {
        free(e.text);
        errno = saved_errno;
        return -1;
    }
    if (e.len + 2 > *cap) {
        *cap = e.len + 2;
        *line = xreallocarray(*line, *cap, 1);
    }
    copy(*line, e.text, e.len);
    (*line)[e.len] = '\n';
    (*line)[e.len + 1] = '\0';
    free(e.text);
    return (ssize_t)e.len + 1;
}
