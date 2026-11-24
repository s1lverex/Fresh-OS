/* edit - full-screen text editor.
 *   arrows / Home / End / PgUp / PgDn   move        Ctrl+S  save
 *   Backspace / Delete                  erase       Ctrl+Q  quit
 *   Ctrl+F                              find        Tabs are expanded to 4 spaces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

struct line {
    char *s;
    int len, cap;
};

static struct line *lines;
static int nlines, lines_cap;
static int cx, cy, rowoff, coloff;
static int rows, cols;
static int dirty;
static char filename[PATH_MAX];
static char message[128];
static int quit_confirm;

/* ------------------------------------------------------------ buffer */
static void line_reserve(struct line *l, int need)
{
    if (need + 1 <= l->cap)
        return;
    int cap = l->cap ? l->cap : 16;
    while (cap < need + 1)
        cap *= 2;
    l->s = realloc(l->s, (size_t)cap);
    if (!l->s) {
        write(2, "edit: out of memory\n", 20);
        exit(1);
    }
    l->cap = cap;
}

static void insert_line(int at, const char *s, int len)
{
    if (nlines == lines_cap) {
        lines_cap = lines_cap ? lines_cap * 2 : 64;
        lines = realloc(lines, sizeof(struct line) * (size_t)lines_cap);
        if (!lines) {
            write(2, "edit: out of memory\n", 20);
            exit(1);
        }
    }
    memmove(&lines[at + 1], &lines[at], sizeof(struct line) * (size_t)(nlines - at));
    struct line *l = &lines[at];
    memset(l, 0, sizeof(*l));
    line_reserve(l, len);
    memcpy(l->s, s, (size_t)len);
    l->len = len;
    l->s[len] = 0;
    nlines++;
}

static void delete_line(int at)
{
    free(lines[at].s);
    memmove(&lines[at], &lines[at + 1], sizeof(struct line) * (size_t)(nlines - at - 1));
    nlines--;
}

static void load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        insert_line(0, "", 0);
        snprintf(message, sizeof(message), "New file");
        return;
    }
    char buf[4096];
    char *cur = malloc(256);
    int clen = 0, ccap = 256;
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n') {
                insert_line(nlines, cur, clen);
                clen = 0;
                continue;
            }
            int add = c == '\t' ? 4 : 1;
            if (clen + add >= ccap) {
                ccap *= 2;
                cur = realloc(cur, (size_t)ccap);
            }
            for (int k = 0; k < add; k++)
                cur[clen++] = c == '\t' ? ' ' : c;
        }
    }
    if (clen || nlines == 0)
        insert_line(nlines, cur, clen);
    free(cur);
    close(fd);
}

static int save(void)
{
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(message, sizeof(message), "Cannot save: %s", strerror(fd));
        return -1;
    }
    long total = 0;
    for (int i = 0; i < nlines; i++) {
        if (write(fd, lines[i].s, (size_t)lines[i].len) != lines[i].len || write(fd, "\n", 1) != 1) {
            snprintf(message, sizeof(message), "Write error - file may be incomplete");
            close(fd);
            return -1;
        }
        total += lines[i].len + 1;
    }
    close(fd);
    sync();
    dirty = 0;
    snprintf(message, sizeof(message), "Saved %d lines, %ld bytes to %s", nlines, total, filename);
    return 0;
}

/* ------------------------------------------------------------ editing */
static void insert_char(char c)
{
    struct line *l = &lines[cy];
    line_reserve(l, l->len + 1);
    memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
    l->s[cx++] = c;
    l->len++;
    dirty = 1;
}

static void newline(void)
{
    struct line *l = &lines[cy];
    insert_line(cy + 1, l->s + cx, l->len - cx);
    l = &lines[cy]; /* array may have moved */
    l->len = cx;
    l->s[cx] = 0;
    cy++;
    cx = 0;
    dirty = 1;
}

static void backspace(void)
{
    if (cx > 0) {
        struct line *l = &lines[cy];
        memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx + 1));
        l->len--;
        cx--;
        dirty = 1;
    } else if (cy > 0) {
        struct line *prev = &lines[cy - 1], *l = &lines[cy];
        int plen = prev->len;
        line_reserve(prev, prev->len + l->len);
        memcpy(prev->s + prev->len, l->s, (size_t)l->len + 1);
        prev->len += l->len;
        delete_line(cy);
        cy--;
        cx = plen;
        dirty = 1;
    }
}

static void delete_char(void)
{
    struct line *l = &lines[cy];
    if (cx < l->len) {
        cx++;
        backspace();
    } else if (cy + 1 < nlines) {
        cy++;
        cx = 0;
        backspace();
    }
}

static void clamp(void)
{
    if (cy < 0)
        cy = 0;
    if (cy >= nlines)
        cy = nlines - 1;
    if (cx > lines[cy].len)
        cx = lines[cy].len;
    if (cx < 0)
        cx = 0;
    if (cy < rowoff)
        rowoff = cy;
    if (cy >= rowoff + rows)
        rowoff = cy - rows + 1;
    if (cx < coloff)
        coloff = cx;
    if (cx >= coloff + cols)
        coloff = cx - cols + 1;
}

/* ------------------------------------------------------------ screen */
static char *obuf;
static size_t olen, ocap;

static void ob(const char *s, size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2;
        obuf = realloc(obuf, ocap);
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void obs(const char *s) { ob(s, strlen(s)); }

static void render(void)
{
    clamp();
    olen = 0;
    obs("\033[?25l\033[H");
    for (int y = 0; y < rows; y++) {
        int fr = rowoff + y;
        if (fr < nlines) {
            int len = lines[fr].len - coloff;
            if (len > cols)
                len = cols;
            if (len > 0)
                ob(lines[fr].s + coloff, (size_t)len);
        } else {
            obs("\033[90m~\033[0m");
        }
        obs("\033[K\r\n");
    }
    char status[256];
    int n;
    if (message[0])
        n = snprintf(status, sizeof(status), " %s", message);
    else
        n = snprintf(status, sizeof(status), " edit: %s%s  |  Ln %d/%d, Col %d  |  ^S save  ^Q quit  ^F find",
                     filename, dirty ? " [modified]" : "", cy + 1, nlines, cx + 1);
    if (n > cols)
        n = cols;
    obs("\033[7m");
    ob(status, (size_t)n);
    for (int k = n; k < cols; k++)
        ob(" ", 1);
    obs("\033[0m");
    char pos[32];
    snprintf(pos, sizeof(pos), "\033[%d;%dH\033[?25h", cy - rowoff + 1, cx - coloff + 1);
    obs(pos);
    write(1, obuf, olen);
}

/* ------------------------------------------------------------ input */
static int read_byte(void)
{
    char c;
    for (;;) {
        long r = read(0, &c, 1);
        if (r == 1)
            return (unsigned char)c;
        if (r < 0)
            return -1;
    }
}

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL };

static int read_key(void)
{
    int c = read_byte();
    if (c != 0x1B)
        return c;
    int a = read_byte();
    if (a != '[' && a != 'O')
        return 0x1B;
    int b = read_byte();
    if (b >= '0' && b <= '9') {
        int t = read_byte();
        if (t != '~')
            return 0;
        switch (b) {
        case '1': case '7': return K_HOME;
        case '4': case '8': return K_END;
        case '3': return K_DEL;
        case '5': return K_PGUP;
        case '6': return K_PGDN;
        default: return 0;
        }
    }
    switch (b) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    default: return 0;
    }
}

static void find(void)
{
    char q[64];
    int n = 0;
    for (;;) {
        snprintf(message, sizeof(message), "Find: %.*s  (Enter search, Esc cancel)", n, q);
        render();
        int k = read_key();
        if (k == '\r' || k == '\n')
            break;
        if (k == 0x1B || k == 3 || k < 0) {
            message[0] = 0;
            return;
        }
        if ((k == 0x7F || k == 8) && n > 0)
            n--;
        else if (k >= 32 && k < 127 && n < 63)
            q[n++] = (char)k;
    }
    q[n] = 0;
    if (!n) {
        message[0] = 0;
        return;
    }
    for (int i = 0; i < nlines; i++) {
        int ln = (cy + i) % nlines;
        int start = i == 0 ? cx + 1 : 0;
        if (start > lines[ln].len)
            continue;
        char *hit = strstr(lines[ln].s + start, q);
        if (hit) {
            cy = ln;
            cx = (int)(hit - lines[ln].s);
            snprintf(message, sizeof(message), "Found '%s' at line %d", q, ln + 1);
            return;
        }
    }
    snprintf(message, sizeof(message), "'%s' not found", q);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: edit FILE\n");
        return 2;
    }
    strlcpy(filename, argv[1], sizeof(filename));
    struct fresh_stat st;
    if (stat(filename, &st) == 0 && st.type == FT_DIR) {
        fprintf(stderr, "edit: %s is a directory\n", filename);
        return 1;
    }
    load(filename);
    ttysize(&rows, &cols);
    if (rows < 3 || cols < 10) {
        rows = 25;
        cols = 80;
    }
    rows -= 1;
    int old_mode = ttymode(TTY_MODE_RAW);
    obs("\033[2J");
    for (;;) {
        render();
        int k = read_key();
        if (k < 0)
            break;
        int had_msg = message[0] != 0;
        if (k != 17)
            quit_confirm = 0;
        switch (k) {
        case 17: /* Ctrl+Q */
            if (dirty && !quit_confirm) {
                snprintf(message, sizeof(message), "Unsaved changes! Press Ctrl+Q again to quit, Ctrl+S to save.");
                quit_confirm = 1;
                continue;
            }
            goto done;
        case 19: /* Ctrl+S */
            save();
            continue;
        case 6: /* Ctrl+F */
            find();
            continue;
        case '\r':
        case '\n':
            newline();
            break;
        case 0x7F:
        case 8:
            backspace();
            break;
        case K_DEL:
            delete_char();
            break;
        case K_UP:
            cy--;
            break;
        case K_DOWN:
            cy++;
            break;
        case K_LEFT:
            if (cx > 0)
                cx--;
            else if (cy > 0) {
                cy--;
                cx = lines[cy].len;
            }
            break;
        case K_RIGHT:
            if (cx < lines[cy].len)
                cx++;
            else if (cy + 1 < nlines) {
                cy++;
                cx = 0;
            }
            break;
        case K_HOME:
            cx = 0;
            break;
        case K_END:
            cx = lines[cy].len;
            break;
        case K_PGUP:
            cy -= rows;
            break;
        case K_PGDN:
            cy += rows;
            break;
        case '\t':
            for (int i = 0; i < 4; i++)
                insert_char(' ');
            break;
        default:
            if (k >= 32 && k < 127)
                insert_char((char)k);
            break;
        }
        if (had_msg)
            message[0] = 0;
    }
done:
    write(1, "\033[2J\033[H", 7);
    ttymode(old_mode);
    return 0;
}
