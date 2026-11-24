#ifndef GFX_H
#define GFX_H
#include <stdint.h>

#include "font.h"

#define SCR_W 320
#define SCR_H 480

/* Colors are 0xRRGGBB. Drawing happens in a back buffer; gfx_flush copies to /dev/fb0. */
int gfx_init(const char *fbdev);
void gfx_flush(void);
void gfx_close(void);
void gfx_set_backlight(int percent);

void gfx_clear(uint32_t c);
void gfx_fill(int x, int y, int w, int h, uint32_t c);
void gfx_blend_px(int x, int y, uint32_t c, int a); /* a: 0..255 */
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c);
void gfx_rrect_outline(int x, int y, int w, int h, int r, int thick, uint32_t c, uint32_t inner);
void gfx_circle(int cx, int cy, int r, uint32_t c);
void gfx_ring(int cx, int cy, int r_out, int r_in, uint32_t c);
/* Copies a w*h 0xRRGGBB image with anti-aliased rounded corners of radius r. */
void gfx_blit(int x, int y, int w, int h, const uint32_t *px, int r);
/* Debug: back buffer -> PPM file. */
int gfx_dump_ppm(const char *path);
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c);

/* Text. y is the baseline. Strings are UTF-8. */
int gfx_text_width(const font_t *f, const char *s);
int gfx_text(const font_t *f, int x, int y, const char *s, uint32_t c);
void gfx_text_center(const font_t *f, int cx, int y, const char *s, uint32_t c);
void gfx_text_right(const font_t *f, int rx, int y, const char *s, uint32_t c);
/* Draws s truncated with "..." to fit maxw. */
void gfx_text_fit(const font_t *f, int x, int y, int maxw, const char *s, uint32_t c);
/* Word-wraps into at most max_lines lines (last one ellipsized). align: 0 left, 1 center.
 * Returns number of lines drawn. If draw==0 only measures. */
int gfx_text_wrap(const font_t *f, int x, int y, int maxw, int max_lines, int line_h, const char *s, uint32_t c, int align, int draw);

/* Decode one UTF-8 code point, advancing *s. */
uint32_t utf8_next(const char **s);

#endif
