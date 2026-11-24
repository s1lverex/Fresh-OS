/* Fresh OS kernel - text console on a linear framebuffer (8x16 font) or
 * VGA text mode, with a subset of ANSI/VT100 escape sequences:
 *   CSI n A/B/C/D  cursor moves      CSI r;c H/f  cursor position
 *   CSI n G / d    column / row      CSI n J / K  erase display / line
 *   CSI ... m      colours/attrs     CSI ?25 l/h  hide/show cursor
 *   CSI s / u      save / restore cursor */
#include "dev.h"
#include "../mm/mm.h"

extern const uint8_t font8x16[256][16];

#define MAX_COLS 240
#define MAX_ROWS 80
#define DEFAULT_ATTR 0x07

enum { MODE_NONE, MODE_FB, MODE_VGA };

static int mode;
static volatile uint32_t *fb;
static uint32_t fb_pitch_px, fb_width, fb_height;
static volatile uint16_t *vga;
static int cols, rows;
static uint16_t cells[MAX_COLS * MAX_ROWS];
static uint8_t dirty[MAX_ROWS];
static int cx, cy, saved_cx, saved_cy;
static int drawn_cx = -1, drawn_cy = -1;
static uint8_t attr = DEFAULT_ATTR;
static bool bold, reverse;
static bool cursor_on = true;
static bool suspended; /* a GUI owns the framebuffer */
static uint32_t pal_px[16];

/* Fresh palette (RGB) */
static const uint32_t palette[16] = {
    0x0E1116, 0xE5534B, 0x57C785, 0xE3B341, 0x4F8FE6, 0xB07CE8, 0x39C5CF, 0xCDD5DF,
    0x636E7B, 0xFF6F66, 0x7EE2A8, 0xF2CC60, 0x79AEF2, 0xCB9DF5, 0x6FDDE5, 0xF5F7FA,
};
static const uint8_t ansi_to_vga[16] = {0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15};

/* escape parser */
static int esc_state; /* 0 normal, 1 ESC, 2 CSI */
static int esc_params[8];
static int esc_nparams;
static bool esc_private;

static uint32_t make_px(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    uint8_t rs = g_boot.fb_red_size ? g_boot.fb_red_size : 8;
    uint8_t gs = g_boot.fb_green_size ? g_boot.fb_green_size : 8;
    uint8_t bs = g_boot.fb_blue_size ? g_boot.fb_blue_size : 8;
    return ((r >> (8 - rs)) << g_boot.fb_red_pos) | ((g >> (8 - gs)) << g_boot.fb_green_pos) |
           ((b >> (8 - bs)) << g_boot.fb_blue_pos);
}

void console_init(void)
{
    if (g_boot.fb_type == BOOT_FB_RGB && g_boot.fb_bpp == 32 && g_boot.fb_width >= 320 && g_boot.fb_height >= 200) {
        uint64_t size = (uint64_t)g_boot.fb_pitch * g_boot.fb_height;
        fb = vmm_map_mmio(g_boot.fb_addr, size, true);
        if (fb) {
            mode = MODE_FB;
            fb_width = g_boot.fb_width;
            fb_height = g_boot.fb_height;
            fb_pitch_px = g_boot.fb_pitch / 4;
            cols = MIN((int)(fb_width / 8), MAX_COLS);
            rows = MIN((int)(fb_height / 16), MAX_ROWS);
            for (int i = 0; i < 16; i++)
                pal_px[i] = make_px(palette[i]);
            for (uint32_t y = 0; y < fb_height; y++)
                for (uint32_t x = 0; x < fb_width; x++)
                    fb[y * fb_pitch_px + x] = pal_px[0];
        }
    } else if (g_boot.boot_type == BOOT_TYPE_BIOS) {
        mode = MODE_VGA;
        vga = (volatile uint16_t *)phys_to_virt(0xB8000);
        cols = 80;
        rows = 25;
    }
    if (mode == MODE_NONE)
        return;
    for (int i = 0; i < cols * rows; i++)
        cells[i] = (uint16_t)(DEFAULT_ATTR << 8 | ' ');
    memset(dirty, 1, sizeof(dirty));
    cx = cy = 0;
}

int console_cols(void) { return mode ? cols : 80; }
int console_rows(void) { return mode ? rows : 25; }
bool console_has_display(void) { return mode != MODE_NONE; }

static void draw_cell(int x, int y)
{
    uint16_t c = cells[y * cols + x];
    uint8_t ch = c & 0xFF, a = c >> 8;
    bool cur = cursor_on && x == MIN(cx, cols - 1) && y == cy;
    if (mode == MODE_VGA) {
        uint8_t va = (uint8_t)(ansi_to_vga[a & 15] | ansi_to_vga[a >> 4] << 4);
        vga[y * 80 + x] = (uint16_t)(va << 8 | ch);
        return;
    }
    uint32_t fg = pal_px[a & 15], bg = pal_px[a >> 4];
    if (cur) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    const uint8_t *g = font8x16[ch];
    volatile uint32_t *p = fb + (uint64_t)y * 16 * fb_pitch_px + (uint64_t)x * 8;
    for (int r = 0; r < 16; r++, p += fb_pitch_px) {
        uint8_t bits = g[r];
        for (int b = 0; b < 8; b++)
            p[b] = (bits & (0x80 >> b)) ? fg : bg;
    }
}

static void vga_cursor(void)
{
    uint16_t pos = (uint16_t)(cy * 80 + MIN(cx, cols - 1));
    if (!cursor_on)
        pos = 0xFFFF;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

static void flush(void)
{
    if (mode == MODE_NONE || suspended)
        return;
    if (drawn_cy >= 0 && drawn_cy < rows && !dirty[drawn_cy] && drawn_cx < cols) {
        int sx = cx, sy = cy;
        cy = -1; /* draw without cursor */
        draw_cell(drawn_cx, drawn_cy);
        cx = sx;
        cy = sy;
    }
    for (int y = 0; y < rows; y++) {
        if (!dirty[y])
            continue;
        for (int x = 0; x < cols; x++)
            draw_cell(x, y);
        dirty[y] = 0;
    }
    int ccx = MIN(cx, cols - 1);
    if (cursor_on)
        draw_cell(ccx, cy);
    drawn_cx = ccx;
    drawn_cy = cy;
    if (mode == MODE_VGA)
        vga_cursor();
}

static void scroll_up(void)
{
    memmove(cells, cells + cols, (size_t)(rows - 1) * cols * sizeof(uint16_t));
    for (int x = 0; x < cols; x++)
        cells[(rows - 1) * cols + x] = (uint16_t)(attr << 8 | ' ');
    memset(dirty, 1, (size_t)rows);
    drawn_cy = -1;
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows) {
        cy = rows - 1;
        scroll_up();
    }
}

static void clear_cells(int from, int to) /* [from, to) linear indices */
{
    for (int i = from; i < to; i++)
        cells[i] = (uint16_t)(attr << 8 | ' ');
    for (int y = from / cols; y <= (to - 1) / cols && y < rows; y++)
        dirty[y] = 1;
}

static void update_attr(int fg, int bg)
{
    uint8_t f = attr & 15, b = attr >> 4;
    if (fg >= 0)
        f = (uint8_t)fg;
    if (bg >= 0)
        b = (uint8_t)bg;
    attr = (uint8_t)(b << 4 | f);
}

static uint8_t base_fg = 7, base_bg = 0;

static void apply_sgr(void)
{
    uint8_t f = base_fg, b = base_bg;
    if (bold && f < 8)
        f += 8;
    if (reverse) {
        uint8_t t = f;
        f = b;
        b = t;
    }
    attr = (uint8_t)(b << 4 | f);
}

static void sgr(void)
{
    if (esc_nparams == 0)
        esc_params[esc_nparams++] = 0;
    for (int i = 0; i < esc_nparams; i++) {
        int p = esc_params[i];
        if (p == 0) {
            base_fg = 7;
            base_bg = 0;
            bold = reverse = false;
        } else if (p == 1) {
            bold = true;
        } else if (p == 22) {
            bold = false;
        } else if (p == 7) {
            reverse = true;
        } else if (p == 27) {
            reverse = false;
        } else if (p >= 30 && p <= 37) {
            base_fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            base_fg = 7;
        } else if (p >= 40 && p <= 47) {
            base_bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            base_bg = 0;
        } else if (p >= 90 && p <= 97) {
            base_fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            base_bg = (uint8_t)(p - 100 + 8);
        }
    }
    apply_sgr();
    (void)update_attr;
}

static int param(int i, int def)
{
    return (i < esc_nparams && esc_params[i] > 0) ? esc_params[i] : def;
}

static void csi(char final)
{
    switch (final) {
    case 'A':
        cy = MAX(0, cy - param(0, 1));
        break;
    case 'B':
        cy = MIN(rows - 1, cy + param(0, 1));
        break;
    case 'C':
        cx = MIN(cols - 1, cx + param(0, 1));
        break;
    case 'D':
        cx = MAX(0, MIN(cx, cols - 1) - param(0, 1));
        break;
    case 'H':
    case 'f':
        cy = MIN(rows - 1, param(0, 1) - 1);
        cx = MIN(cols - 1, param(1, 1) - 1);
        break;
    case 'G':
        cx = MIN(cols - 1, param(0, 1) - 1);
        break;
    case 'd':
        cy = MIN(rows - 1, param(0, 1) - 1);
        break;
    case 'J': {
        int p = esc_nparams ? esc_params[0] : 0;
        int pos = cy * cols + MIN(cx, cols - 1);
        if (p == 0)
            clear_cells(pos, rows * cols);
        else if (p == 1)
            clear_cells(0, pos + 1);
        else
            clear_cells(0, rows * cols);
        break;
    }
    case 'K': {
        int p = esc_nparams ? esc_params[0] : 0;
        int start = cy * cols, pos = start + MIN(cx, cols - 1);
        if (p == 0)
            clear_cells(pos, start + cols);
        else if (p == 1)
            clear_cells(start, pos + 1);
        else
            clear_cells(start, start + cols);
        break;
    }
    case 'm':
        sgr();
        break;
    case 's':
        saved_cx = cx;
        saved_cy = cy;
        break;
    case 'u':
        cx = saved_cx;
        cy = saved_cy;
        break;
    case 'l':
    case 'h':
        if (esc_private && param(0, 0) == 25)
            cursor_on = final == 'h';
        break;
    default:
        break;
    }
}

static void put_char(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_nparams = 0;
            esc_private = false;
            for (int i = 0; i < 8; i++)
                esc_params[i] = 0;
        } else {
            esc_state = 0;
        }
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (esc_nparams == 0)
                esc_nparams = 1;
            int *p = &esc_params[esc_nparams - 1];
            if (*p < 10000)
                *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (esc_nparams == 0)
                esc_nparams = 1;
            if (esc_nparams < 8)
                esc_nparams++;
        } else if (c == '?') {
            esc_private = true;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi((char)c);
            esc_state = 0;
        } else {
            esc_state = 0;
        }
        return;
    }
    switch (c) {
    case 0x1B:
        esc_state = 1;
        return;
    case '\n':
        newline();
        return;
    case '\r':
        cx = 0;
        return;
    case '\b':
        if (cx > 0)
            cx = MIN(cx, cols) - 1;
        return;
    case '\t':
        do {
            put_char(' ');
        } while (cx % 8 && cx < cols);
        return;
    case 7:
        return;
    default:
        break;
    }
    if (c < 32)
        return;
    if (cx >= cols)
        newline();
    cells[cy * cols + cx] = (uint16_t)(attr << 8 | c);
    dirty[cy] = 1;
    cx++;
}

void console_suspend(bool on)
{
    uint64_t f = irq_save();
    suspended = on;
    if (!on && mode == MODE_FB) {
        for (uint32_t y = 0; y < fb_height; y++) /* wipe what the GUI left */
            for (uint32_t x = 0; x < fb_width; x++)
                fb[y * fb_pitch_px + x] = pal_px[0];
    }
    if (!on && mode != MODE_NONE) {
        memset(dirty, 1, sizeof(dirty));
        drawn_cy = -1;
        flush();
    }
    irq_restore(f);
}

void console_write(const char *s, size_t n)
{
    if (mode == MODE_NONE)
        return;
    uint64_t f = irq_save();
    for (size_t i = 0; i < n; i++)
        put_char(s[i]);
    flush();
    irq_restore(f);
}
