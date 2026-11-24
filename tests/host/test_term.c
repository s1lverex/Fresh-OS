/* Unit tests for the desktop's terminal emulator (user/desktop/term.c). */
#include <stdio.h>
#include <string.h>
#include "term.h"

static int failures;
static uint16_t cells[20 * 5];

#define CHECK(c, msg)                                                                                        \
    do {                                                                                                    \
        if (!(c)) {                                                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                            \
            failures++;                                                                                     \
        }                                                                                                   \
    } while (0)

static void row(term_t *t, int y, char *out)
{
    for (int x = 0; x < t->cols; x++)
        out[x] = (char)(term_cell(t, x, y) & 0xFF);
    out[t->cols] = 0;
    for (int x = t->cols - 1; x >= 0 && out[x] == ' '; x--)
        out[x] = 0;
}

static void w(term_t *t, const char *s) { term_write(t, s, strlen(s)); }

int main(void)
{
    term_t t;
    char r[32];
    term_init(&t, 20, 5, cells);
    w(&t, "hello\nworld");
    row(&t, 0, r);
    CHECK(!strcmp(r, "hello"), "first line");
    row(&t, 1, r);
    CHECK(!strcmp(r, "world") && t.cx == 5 && t.cy == 1, "newline is CR+LF");
    w(&t, "\r\x1b[2K");
    row(&t, 1, r);
    CHECK(r[0] == 0, "erase line");
    w(&t, "\x1b[3;4HX");
    CHECK((term_cell(&t, 3, 2) & 0xFF) == 'X', "cursor position");
    w(&t, "\x1b[1;31mR\x1b[0m");
    CHECK((term_cell(&t, 4, 2) >> 8) == 0x09, "bold red attribute");
    CHECK(t.attr == TERM_DEFAULT_ATTR, "attribute reset");
    w(&t, "\x1b[2J\x1b[H");
    row(&t, 2, r);
    CHECK(r[0] == 0 && t.cx == 0 && t.cy == 0, "clear screen + home");
    for (int i = 0; i < 7; i++) {
        char line[16];
        snprintf(line, sizeof(line), "L%d\n", i);
        w(&t, line);
    }
    row(&t, 0, r);
    CHECK(!strcmp(r, "L3"), "scrolls when the screen is full");
    w(&t, "\x1b[H01234567890123456789Z");
    row(&t, 1, r);
    CHECK(r[0] == 'Z', "line wraps at the right edge");
    w(&t, "\x1b[?25l");
    CHECK(!t.cursor_on, "hide cursor");
    w(&t, "\x1b[?25h\tA");
    CHECK(t.cursor_on && (term_cell(&t, 8, 1) & 0xFF) == 'A', "show cursor + tab");
    w(&t, "\x1b[99;99H\x1b[5A\x1b[100D");
    CHECK(t.cy == 0 && t.cx == 0, "cursor moves are clamped");
    w(&t, "\x1b[s\x1b[3;3H\x1b[u");
    CHECK(t.cy == 0 && t.cx == 0, "save/restore cursor");
    /* garbage must never write outside the grid */
    for (int i = 0; i < 20000; i++) {
        char b[2] = {(char)(i * 7919 % 256), 0};
        term_write(&t, b, 1);
    }
    CHECK(t.cx <= t.cols && t.cy < t.rows, "random bytes keep the cursor inside");
    printf("test_term: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
