/* Desktop graphics primitives (see gfx.h). */
#include "gfx.h"
#include <stdlib.h>
#include <string.h>

extern const uint8_t font8x16[256][16];

int scr_w, scr_h;
static uint32_t *screen;     /* framebuffer (mapped by the kernel) */
static uint32_t pitch_px;
static uint32_t *back;       /* back buffer, scr_w * scr_h */
static uint32_t *wallpaper;
static uint8_t rpos, gpos, bpos;
static rect_t clip;

static const char *cursor_art[CURSOR_H] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
    "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.........# ", "#......#####",
    "#...#..#    ", "#..# #..#   ", "#.#  #..#   ", "##    #..#  ", "#     #..#  ", "       #..# ",
    "        ##  ",
};

int gfx_init(const struct fresh_fbinfo *fi)
{
    scr_w = (int)fi->width;
    scr_h = (int)fi->height;
    screen = (uint32_t *)(uintptr_t)fi->addr;
    pitch_px = fi->pitch / 4;
    rpos = fi->red_pos;
    gpos = fi->green_pos;
    bpos = fi->blue_pos;
    back = malloc((size_t)scr_w * scr_h * 4);
    wallpaper = malloc((size_t)scr_w * scr_h * 4);
    if (!back || !wallpaper)
        return -1;
    gfx_noclip();
    return 0;
}

uint32_t px(uint32_t rgb)
{
    return ((rgb >> 16 & 0xFF) << rpos) | ((rgb >> 8 & 0xFF) << gpos) | ((rgb & 0xFF) << bpos);
}

void gfx_clip(rect_t r)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > scr_w ? scr_w : r.x + r.w, y1 = r.y + r.h > scr_h ? scr_h : r.y + r.h;
    clip.x = x0;
    clip.y = y0;
    clip.w = x1 > x0 ? x1 - x0 : 0;
    clip.h = y1 > y0 ? y1 - y0 : 0;
}

void gfx_noclip(void)
{
    clip.x = clip.y = 0;
    clip.w = scr_w;
    clip.h = scr_h;
}

void fill_px(int x, int y, int w, int h, uint32_t pixel)
{
    int x0 = x < clip.x ? clip.x : x, y0 = y < clip.y ? clip.y : y;
    int x1 = x + w > clip.x + clip.w ? clip.x + clip.w : x + w;
    int y1 = y + h > clip.y + clip.h ? clip.y + clip.h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = back + (size_t)yy * scr_w;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = pixel;
    }
}

void fill(int x, int y, int w, int h, uint32_t rgb) { fill_px(x, y, w, h, px(rgb)); }

void frame(int x, int y, int w, int h, uint32_t rgb)
{
    fill(x, y, w, 1, rgb);
    fill(x, y + h - 1, w, 1, rgb);
    fill(x, y, 1, h, rgb);
    fill(x + w - 1, y, 1, h, rgb);
}

static uint32_t mix(uint32_t a, uint32_t b, int num, int den)
{
    uint32_t r = ((a >> 16 & 0xFF) * (uint32_t)(den - num) + (b >> 16 & 0xFF) * (uint32_t)num) / (uint32_t)den;
    uint32_t g = ((a >> 8 & 0xFF) * (uint32_t)(den - num) + (b >> 8 & 0xFF) * (uint32_t)num) / (uint32_t)den;
    uint32_t bl = ((a & 0xFF) * (uint32_t)(den - num) + (b & 0xFF) * (uint32_t)num) / (uint32_t)den;
    return r << 16 | g << 8 | bl;
}

void gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int i = 0; i < h; i++)
        fill(x, y + i, w, 1, mix(top, bottom, i, h > 1 ? h - 1 : 1));
}

static void glyph(int x, int y, unsigned char c, uint32_t fgp)
{
    const uint8_t *g = font8x16[c];
    for (int r = 0; r < 16; r++) {
        int yy = y + r;
        if (yy < clip.y || yy >= clip.y + clip.h || !g[r])
            continue;
        uint32_t *row = back + (size_t)yy * scr_w;
        for (int b = 0; b < 8; b++) {
            int xx = x + b;
            if ((g[r] & (0x80 >> b)) && xx >= clip.x && xx < clip.x + clip.w)
                row[xx] = fgp;
        }
    }
}

void glyph_cell(int x, int y, unsigned char c, uint32_t fg_px, uint32_t bg_px)
{
    fill_px(x, y, 8, 16, bg_px);
    if (c != ' ')
        glyph(x, y, c, fg_px);
}

int text_n(int x, int y, const char *s, int n, uint32_t fg)
{
    uint32_t p = px(fg);
    for (int i = 0; i < n && s[i]; i++, x += 8)
        glyph(x, y, (unsigned char)s[i], p);
    return x;
}

int text(int x, int y, const char *s, uint32_t fg) { return text_n(x, y, s, 1 << 30, fg); }

void glyph_scaled(int x, int y, unsigned char c, int scale, uint32_t fg)
{
    const uint8_t *g = font8x16[c];
    uint32_t p = px(fg);
    for (int r = 0; r < 16; r++)
        for (int b = 0; b < 8; b++)
            if (g[r] & (0x80 >> b))
                fill_px(x + b * scale, y + r * scale, scale, scale, p);
}

void save_wallpaper(void) { memcpy(wallpaper, back, (size_t)scr_w * scr_h * 4); }
void restore_wallpaper(void) { memcpy(back, wallpaper, (size_t)scr_w * scr_h * 4); }

void present(rect_t r)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > scr_w ? scr_w : r.x + r.w, y1 = r.y + r.h > scr_h ? scr_h : r.y + r.h;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (int y = y0; y < y1; y++)
        memcpy(screen + (size_t)y * pitch_px + x0, back + (size_t)y * scr_w + x0, (size_t)(x1 - x0) * 4);
}

void present_all(void)
{
    rect_t r = {0, 0, scr_w, scr_h};
    present(r);
}

void cursor_draw(int x, int y)
{
    uint32_t black = px(0x000000), white = px(0xFFFFFF);
    for (int r = 0; r < CURSOR_H; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= scr_h)
            continue;
        for (int c = 0; c < CURSOR_W; c++) {
            int xx = x + c;
            char a = cursor_art[r][c];
            if (xx < 0 || xx >= scr_w || a == ' ')
                continue;
            screen[(size_t)yy * pitch_px + xx] = a == '#' ? black : white;
        }
    }
}

void cursor_erase(int x, int y)
{
    rect_t r = {x, y, CURSOR_W, CURSOR_H};
    present(r);
}
