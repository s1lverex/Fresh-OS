/* Desktop graphics: draw into an off-screen back buffer, then copy changed
 * areas to the framebuffer. Colours are 0xRRGGBB; gfx converts them to the
 * framebuffer's pixel format. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "fresh/abi.h"

typedef struct {
    int x, y, w, h;
} rect_t;

extern int scr_w, scr_h;

int gfx_init(const struct fresh_fbinfo *fi);
uint32_t px(uint32_t rgb);
void gfx_clip(rect_t r);       /* restrict drawing to r */
void gfx_noclip(void);
void fill(int x, int y, int w, int h, uint32_t rgb);
void fill_px(int x, int y, int w, int h, uint32_t pixel);
void frame(int x, int y, int w, int h, uint32_t rgb); /* 1px outline */
void gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
int text(int x, int y, const char *s, uint32_t fg);  /* returns end x */
int text_n(int x, int y, const char *s, int n, uint32_t fg);
void glyph_scaled(int x, int y, unsigned char c, int scale, uint32_t fg);
void glyph_cell(int x, int y, unsigned char c, uint32_t fg_px, uint32_t bg_px);
void save_wallpaper(void);    /* snapshot the back buffer as the background */
void restore_wallpaper(void);
void present(rect_t r);       /* back buffer -> screen */
void present_all(void);
void cursor_draw(int x, int y); /* directly on screen */
void cursor_erase(int x, int y);
#define CURSOR_W 12
#define CURSOR_H 19

#endif
