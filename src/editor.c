#include "editor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

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
};

struct edit {
    int fd;
    const char *prompt;
    size_t plen;
    char *text;  /* the line so far, without a final 0 */
    size_t len;
    size_t cap;
    size_t pos;  /* the cursor, as an offset into text */
    size_t row;  /* the screen row of the cursor, counted from the prompt's */
    int utf8_left; /* bytes still to come of a UTF-8 character being typed */
    char *out;   /* what to write next, sent in one write() to avoid flicker */
    size_t out_len;
    size_t out_cap;
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

/* The terminal's width, asked each time, so a resized window just works. */
static size_t columns(void)
{
    struct winsize ws;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/*
 * Draws the prompt's last line and the text again, and puts the cursor at
 * pos. A long line wraps over several rows, so first go up to the prompt's
 * row, clear everything below, and write it all out. The terminal leaves
 * the cursor after the end, and from there it moves up and right to pos.
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
    flush(e);
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

/* Does what key asks. Returns 1 when the line is done (Enter), -1 when it
 * isn't coming (Ctrl+C, Ctrl+D on an empty line, end of input), else 0. */
static int handle(struct edit *e, int key, void (*repaint)(void *), void *arg)
{
    switch (key) {
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
    case CTRL_KEY('L'):
        puts_out(e, "\033[H\033[2J");
        flush(e);
        if (repaint != NULL)
            repaint(arg);
        else
            fputs(e->prompt, stderr);
        fflush(stderr);
        e->row = 0;
        break;
    case -1:
        return -1;
    default:
        /* text, UTF-8 bytes too; other control keys and unknown sequences
         * do nothing, quietly */
        if (key < ' ' || key >= 256 || key == BACKSPACE)
            return 0;
        insert(e, (char)key);
        if (utf8_part(e, (unsigned char)key))
            return 0; /* half a letter would show as junk: wait for the rest */
    }
    redraw(e);
    return 0;
}

ssize_t editor_read_line(int fd, const char *prompt, void (*repaint)(void *), void *arg,
                         char **line, size_t *cap)
{
    struct edit e = {
        .fd = fd,
        .prompt = prompt,
        .plen = strlen(prompt),
        /* never NULL, so pointer sums like text + pos are always defined */
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
    /* TCSADRAIN, not TCSAFLUSH: keys typed ahead stay for this line. */
    signals_guard_terminal(fd, &cooked);
    tcsetattr(fd, TCSADRAIN, &raw);

    while (done == 0)
        done = handle(&e, next_key(&e), repaint, arg);

    saved_errno = errno;
    tcsetattr(fd, TCSADRAIN, &cooked);
    signals_unguard_terminal();
    free(e.out);
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
