/* Terminal emulator for GUI terminal windows (see term.h). Supports the same
 * escape sequences as the kernel console:
 *   CSI n A/B/C/D, CSI r;c H/f, CSI n G/d, CSI n J/K, CSI ... m,
 *   CSI ?25 l/h, CSI s/u; '\n' starts a new line (CR+LF), '\r', '\b', '\t'. */
#include "term.h"
#include <string.h>

int term_init(term_t *t, int cols, int rows, uint16_t *cells)
{
    memset(t, 0, sizeof(*t));
    t->cols = cols;
    t->rows = rows;
    t->cells = cells;
    t->attr = TERM_DEFAULT_ATTR;
    t->base_fg = 7;
    t->cursor_on = 1;
    for (int i = 0; i < cols * rows; i++)
        cells[i] = (uint16_t)(TERM_DEFAULT_ATTR << 8 | ' ');
    t->dirty = 1;
    return 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void scroll_up(term_t *t)
{
    memmove(t->cells, t->cells + t->cols, (size_t)(t->rows - 1) * t->cols * sizeof(uint16_t));
    for (int x = 0; x < t->cols; x++)
        t->cells[(t->rows - 1) * t->cols + x] = (uint16_t)(t->attr << 8 | ' ');
}

static void newline(term_t *t)
{
    t->cx = 0;
    if (++t->cy >= t->rows) {
        t->cy = t->rows - 1;
        scroll_up(t);
    }
}

static void clear_cells(term_t *t, int from, int to)
{
    for (int i = from; i < to; i++)
        t->cells[i] = (uint16_t)(t->attr << 8 | ' ');
}

static void apply_sgr(term_t *t)
{
    uint8_t f = t->base_fg, b = t->base_bg;
    if (t->bold && f < 8)
        f += 8;
    if (t->reverse) {
        uint8_t x = f;
        f = b;
        b = x;
    }
    t->attr = (uint8_t)(b << 4 | f);
}

static void sgr(term_t *t)
{
    if (t->nparams == 0)
        t->params[t->nparams++] = 0;
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->base_fg = 7;
            t->base_bg = 0;
            t->bold = t->reverse = 0;
        } else if (p == 1) {
            t->bold = 1;
        } else if (p == 22) {
            t->bold = 0;
        } else if (p == 7) {
            t->reverse = 1;
        } else if (p == 27) {
            t->reverse = 0;
        } else if (p >= 30 && p <= 37) {
            t->base_fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            t->base_fg = 7;
        } else if (p >= 40 && p <= 47) {
            t->base_bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            t->base_bg = 0;
        } else if (p >= 90 && p <= 97) {
            t->base_fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            t->base_bg = (uint8_t)(p - 100 + 8);
        }
    }
    apply_sgr(t);
}

static int param(term_t *t, int i, int def) { return (i < t->nparams && t->params[i] > 0) ? t->params[i] : def; }

static void csi(term_t *t, char final)
{
    int cols = t->cols, rows = t->rows;
    int ccx = t->cx < cols ? t->cx : cols - 1;
    switch (final) {
    case 'A': t->cy = clampi(t->cy - param(t, 0, 1), 0, rows - 1); break;
    case 'B': t->cy = clampi(t->cy + param(t, 0, 1), 0, rows - 1); break;
    case 'C': t->cx = clampi(t->cx + param(t, 0, 1), 0, cols - 1); break;
    case 'D': t->cx = clampi(ccx - param(t, 0, 1), 0, cols - 1); break;
    case 'H':
    case 'f':
        t->cy = clampi(param(t, 0, 1) - 1, 0, rows - 1);
        t->cx = clampi(param(t, 1, 1) - 1, 0, cols - 1);
        break;
    case 'G': t->cx = clampi(param(t, 0, 1) - 1, 0, cols - 1); break;
    case 'd': t->cy = clampi(param(t, 0, 1) - 1, 0, rows - 1); break;
    case 'J': {
        int p = t->nparams ? t->params[0] : 0, pos = t->cy * cols + ccx;
        if (p == 0)
            clear_cells(t, pos, rows * cols);
        else if (p == 1)
            clear_cells(t, 0, pos + 1);
        else
            clear_cells(t, 0, rows * cols);
        break;
    }
    case 'K': {
        int p = t->nparams ? t->params[0] : 0, start = t->cy * cols, pos = start + ccx;
        if (p == 0)
            clear_cells(t, pos, start + cols);
        else if (p == 1)
            clear_cells(t, start, pos + 1);
        else
            clear_cells(t, start, start + cols);
        break;
    }
    case 'm': sgr(t); break;
    case 's':
        t->saved_cx = t->cx;
        t->saved_cy = t->cy;
        break;
    case 'u':
        t->cx = clampi(t->saved_cx, 0, cols);
        t->cy = clampi(t->saved_cy, 0, rows - 1);
        break;
    case 'l':
    case 'h':
        if (t->priv && param(t, 0, 0) == 25)
            t->cursor_on = final == 'h';
        break;
    default: break;
    }
}

static void put(term_t *t, unsigned char c)
{
    if (t->esc_state == 1) {
        if (c == '[') {
            t->esc_state = 2;
            t->nparams = 0;
            t->priv = 0;
            memset(t->params, 0, sizeof(t->params));
        } else {
            t->esc_state = 0;
        }
        return;
    }
    if (t->esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (t->nparams == 0)
                t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (*p < 10000)
                *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (t->nparams == 0)
                t->nparams = 1;
            if (t->nparams < 8)
                t->nparams++;
        } else if (c == '?') {
            t->priv = 1;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi(t, (char)c);
            t->esc_state = 0;
        } else {
            t->esc_state = 0;
        }
        return;
    }
    switch (c) {
    case 0x1B: t->esc_state = 1; return;
    case '\n': newline(t); return;
    case '\r': t->cx = 0; return;
    case '\b':
        if (t->cx > 0)
            t->cx = (t->cx < t->cols ? t->cx : t->cols) - 1;
        return;
    case '\t':
        do {
            put(t, ' ');
        } while (t->cx % 8 && t->cx < t->cols);
        return;
    default: break;
    }
    if (c < 32)
        return;
    if (t->cx >= t->cols)
        newline(t);
    t->cells[t->cy * t->cols + t->cx] = (uint16_t)(t->attr << 8 | c);
    t->cx++;
}

void term_write(term_t *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put(t, (unsigned char)s[i]);
    if (n)
        t->dirty = 1;
}
