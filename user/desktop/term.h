/* Terminal emulator state for GUI terminal windows: a grid of character
 * cells driven by the same ANSI subset as the kernel console. Pure C with no
 * system calls, so it is unit tested on the host (tests/host/test_term.c). */
#ifndef TERM_H
#define TERM_H

#include <stdint.h>
#include <stddef.h>

#define TERM_DEFAULT_ATTR 0x07

typedef struct term {
    int cols, rows;
    uint16_t *cells; /* char | attr << 8, attr = fg | bg << 4 */
    int cx, cy, saved_cx, saved_cy;
    uint8_t attr, base_fg, base_bg;
    int bold, reverse, cursor_on;
    int esc_state, nparams, priv;
    int params[8];
    int dirty;
} term_t;

int term_init(term_t *t, int cols, int rows, uint16_t *cells);
void term_write(term_t *t, const char *s, size_t n);
static inline uint16_t term_cell(const term_t *t, int x, int y) { return t->cells[y * t->cols + x]; }

#endif
