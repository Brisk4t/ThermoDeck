/*
 * Software rendering into a 320x480 back buffer (0xRRGGBB per pixel) that gfx_flush()
 * copies to the Linux framebuffer. Shapes and text are anti-aliased by computing how much
 * of each pixel a shape covers and blending by that amount.
 */

#include "gfx/gfx.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/common.h"

/* everything draws here; the framebuffer is only touched by gfx_flush() */
static uint32_t backbuf[SCR_W * SCR_H];
static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_len;
static int fb_line, fb_bpp, fb_xres, fb_yres;
static int r_off = 16, g_off = 8, b_off = 0, native_xrgb = 1;

/* Opens and maps the framebuffer and records its pixel layout. */
int gfx_init(const char *fbdev)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fb_fd = open(fbdev, O_RDWR);
    if (fb_fd < 0) {
        tdlog("gfx: open %s failed", fbdev);
        return -1;
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &f)) {
        tdlog("gfx: ioctl failed");
        return -1;
    }
    fb_line = (int)f.line_length;
    fb_bpp = (int)v.bits_per_pixel;
    fb_xres = (int)v.xres;
    fb_yres = (int)v.yres;
    r_off = (int)v.red.offset;
    g_off = (int)v.green.offset;
    b_off = (int)v.blue.offset;
    native_xrgb = fb_bpp == 32 && r_off == 16 && g_off == 8 && b_off == 0;
    fb_len = f.smem_len ? f.smem_len : (size_t)fb_line * v.yres_virtual;
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        tdlog("gfx: mmap failed");
        fb_mem = NULL;
        return -1;
    }
    /* Make sure we're showing the first page and the panel is unblanked. */
    if (v.yoffset || v.xoffset) {
        v.xoffset = v.yoffset = 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &v);
    }
    ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
    tdlog("gfx: %dx%d %dbpp line=%d R%d G%d B%d", fb_xres, fb_yres, fb_bpp, fb_line, r_off, g_off, b_off);
    return 0;
}

/* Unmaps and closes the framebuffer. */
void gfx_close(void)
{
    if (fb_mem) munmap(fb_mem, fb_len);
    if (fb_fd >= 0) close(fb_fd);
}

/* Small sysfs helpers. */
static int read_int_file(const char *path, int def)
{
    FILE *f = fopen(path, "r");
    int v = def;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = def;
        fclose(f);
    }
    return v;
}

static int write_int_file(const char *path, int v)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", v);
    return fclose(f);
}

/* The ecobee's panel backlight is the PWM LED "led-pwm0" (0..255); mxs-bl exists but
 * doesn't drive it, so it's just parked at full. Percent is perceptual: PWM duty
 * follows a gamma-2 curve so equal slider steps look like equal brightness steps. */
void gfx_set_backlight(int percent)
{
    percent = CLAMP(percent, 1, 100);
    const char *led = "/sys/class/leds/led-pwm0";
    char path[96];
    snprintf(path, sizeof path, "%s/max_brightness", led);
    int max = read_int_file(path, -1);
    if (max > 0) {
        float f = (float)percent / 100.0f;
        int duty = 1 + (int)((float)(max - 1) * f * f + 0.5f);
        snprintf(path, sizeof path, "%s/brightness", led);
        write_int_file(path, duty);
    }
    const char *bl = "/sys/class/backlight/mxs-bl";
    snprintf(path, sizeof path, "%s/max_brightness", bl);
    int blmax = read_int_file(path, 100);
    snprintf(path, sizeof path, "%s/brightness", bl);
    write_int_file(path, max > 0 ? blmax : percent * blmax / 100);
    snprintf(path, sizeof path, "%s/bl_power", bl);
    write_int_file(path, 0);
}

/* Writes one 320-pixel row (0xRRGGBB) to framebuffer row y, converting the pixel format if
 * the framebuffer isn't 32-bit XRGB (the ecobee's is, so it's a straight memcpy). */
static void put_row(int y, const uint32_t *src)
{
    int w = fb_xres < SCR_W ? fb_xres : SCR_W;
    uint8_t *dst = fb_mem + (size_t)y * (size_t)fb_line;
    if (native_xrgb) {
        memcpy(dst, src, (size_t)w * 4);
    }
    else if (fb_bpp == 32) {
        uint32_t *d = (uint32_t *)dst;
        for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            d[x] = (((c >> 16) & 255) << r_off) | (((c >> 8) & 255) << g_off) | ((c & 255) << b_off);
        }
    }
    else if (fb_bpp == 16) {
        uint16_t *d = (uint16_t *)dst;
        for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            d[x] = (uint16_t)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31));
        }
    }
}

static int visible_rows(void) { return fb_yres < SCR_H ? fb_yres : SCR_H; }

/* Copies the back buffer to the screen. */
void gfx_flush(void)
{
    if (!fb_mem) return;
    for (int y = 0; y < visible_rows(); y++) put_row(y, &backbuf[y * SCR_W]);
}

uint32_t *gfx_buffer(void) { return backbuf; }

void gfx_present_slide(const uint32_t *a, const uint32_t *b, int top, int off, uint32_t fill)
{
    if (!fb_mem) return;
    static uint32_t fill_row[SCR_W];
    for (int x = 0; x < SCR_W; x++) fill_row[x] = fill;
    int h = SCR_H - top;
    for (int y = 0; y < visible_rows(); y++) {
        const uint32_t *src;
        if (y < top) {
            src = &a[y * SCR_W]; /* fixed part (header) */
        }
        else {
            /* content row y shows page a shifted by off; b is the page above (off > 0) or below */
            int pos = y - top - off;
            if (pos >= 0 && pos < h)
                src = &a[(top + pos) * SCR_W];
            else if (!b)
                src = fill_row;
            else
                src = &b[(top + (pos < 0 ? pos + h : pos - h)) * SCR_W];
        }
        put_row(y, src);
    }
}

/* Fills the whole back buffer. */
void gfx_clear(uint32_t c)
{
    for (int i = 0; i < SCR_W * SCR_H; i++) backbuf[i] = c;
}

/* Solid rectangle, clipped to the screen. */
void gfx_fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        uint32_t *p = &backbuf[j * SCR_W + x];
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

/* Mixes source colour s over d with alpha a (0..255), per channel. */
static inline uint32_t blend(uint32_t d, uint32_t s, int a)
{
    if (a >= 255) return s;
    if (a <= 0) return d;
    int ia = 256 - (a + (a >> 7));
    int aa = 256 - ia;
    uint32_t rb = ((((s & 0xFF00FF) * (uint32_t)aa) + ((d & 0xFF00FF) * (uint32_t)ia)) >> 8) & 0xFF00FF;
    uint32_t g = ((((s & 0x00FF00) * (uint32_t)aa) + ((d & 0x00FF00) * (uint32_t)ia)) >> 8) & 0x00FF00;
    return rb | g;
}

/* Blends one pixel (ignored if off screen). */
void gfx_blend_px(int x, int y, uint32_t c, int a)
{
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
    uint32_t *p = &backbuf[y * SCR_W + x];
    *p = blend(*p, c, a);
}

/* Coverage of pixel (px,py) for a disc at (cx,cy) radius r (pixel centers at +0.5). */
static inline int disc_cov(float px, float py, float cx, float cy, float r)
{
    float dx = px - cx, dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy);
    float cov = r + 0.5f - d;
    if (cov <= 0) return 0;
    if (cov >= 1) return 255;
    return (int)(cov * 255);
}

/* Rounded rectangle: three solid rectangles plus four anti-aliased quarter-disc corners. */
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        gfx_fill(x, y, w, h, c);
        return;
    }
    gfx_fill(x, y + r, w, h - 2 * r, c);
    gfx_fill(x + r, y, w - 2 * r, r, c);
    gfx_fill(x + r, y + h - r, w - 2 * r, r, c);
    /* corners */
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            int a = disc_cov(px, py, (float)r, (float)r, (float)r);
            if (!a) continue;
            gfx_blend_px(x + i, y + j, c, a);
            gfx_blend_px(x + w - 1 - i, y + j, c, a);
            gfx_blend_px(x + i, y + h - 1 - j, c, a);
            gfx_blend_px(x + w - 1 - i, y + h - 1 - j, c, a);
        }
    }
}

/* Rounded rectangle border: outer shape in c, then the inside refilled with inner. */
void gfx_rrect_outline(int x, int y, int w, int h, int r, int thick, uint32_t c, uint32_t inner)
{
    gfx_rrect(x, y, w, h, r, c);
    gfx_rrect(x + thick, y + thick, w - 2 * thick, h - 2 * thick, r > thick ? r - thick : 0, inner);
}

/* Filled anti-aliased circle. */
void gfx_circle(int cx, int cy, int r, uint32_t c) { gfx_ring(cx, cy, r, -1, c); }

/* Anti-aliased annulus; r_in < 0 means a filled disc. */
void gfx_ring(int cx, int cy, int r_out, int r_in, uint32_t c)
{
    float fcx = (float)cx + 0.5f, fcy = (float)cy + 0.5f;
    for (int y = cy - r_out - 1; y <= cy + r_out + 1; y++) {
        for (int x = cx - r_out - 1; x <= cx + r_out + 1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int a = disc_cov(px, py, fcx, fcy, (float)r_out);
            if (!a) continue;
            if (r_in >= 0) {
                int ai = disc_cov(px, py, fcx, fcy, (float)r_in);
                a = a - ai;
                if (a <= 0) continue;
            }
            gfx_blend_px(x, y, c, a);
        }
    }
}

/*
 * Anti-aliased filled triangle: for each pixel in the bounding box, the distance to the
 * nearest edge gives its coverage.
 */
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
    /* normalize winding so that inside points get positive edge distances (y axis points down) */
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area > 0) {
        float tx = x1, ty = y1;
        x1 = x2;
        y1 = y2;
        x2 = tx;
        y2 = ty;
    }
    float ex[3][3]; /* a, b, c with distance = a*x + b*y + c (normalized) */
    float vx[3] = {x0, x1, x2}, vy[3] = {y0, y1, y2};
    for (int k = 0; k < 3; k++) {
        float ax = vx[k], ay = vy[k], bx = vx[(k + 1) % 3], by = vy[(k + 1) % 3];
        float nx = -(by - ay), ny = bx - ax;
        float len = sqrtf(nx * nx + ny * ny);
        if (len == 0) return;
        nx /= len;
        ny /= len;
        ex[k][0] = nx;
        ex[k][1] = ny;
        ex[k][2] = -(nx * ax + ny * ay);
    }
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))) - 1, maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    /* coverage = signed distance to the closest edge, shifted by half a pixel */
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float d = 1e9f;
            for (int k = 0; k < 3; k++) {
                float dk = -(ex[k][0] * px + ex[k][1] * py + ex[k][2]);
                if (dk < d) d = dk;
            }
            float cov = d + 0.5f;
            if (cov <= 0) continue;
            gfx_blend_px(x, y, c, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

/* Decodes one UTF-8 code point and advances *sp (bad bytes come back as '?'). */
uint32_t utf8_next(const char **sp)
{
    const unsigned char *s = (const unsigned char *)*sp;
    uint32_t cp;
    int n;
    if (s[0] < 0x80) {
        cp = s[0];
        n = 1;
    }
    else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    }
    else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    }
    else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) |
             (s[3] & 0x3F);
        n = 4;
    }
    else {
        cp = '?';
        n = 1;
    }
    *sp += n;
    return cp;
}

/* Horizontal advance of a character in font f. */
static int glyph_adv(const font_t *f, uint32_t cp) { return f->glyphs[font_index(cp)].adv; }

/* Width in pixels of s when drawn in font f. */
int gfx_text_width(const font_t *f, const char *s)
{
    int w = 0;
    while (*s) w += glyph_adv(f, utf8_next(&s));
    return w;
}

/* Blends one glyph's alpha bitmap at baseline (x, y); returns its advance. */
static int draw_glyph(const font_t *f, int x, int y, uint32_t cp, uint32_t c)
{
    const glyph_t *g = &f->glyphs[font_index(cp)];
    const uint8_t *bits = f->bits + g->off;
    int gx = x + g->xoff, gy = y + g->yoff;
    for (int j = 0; j < g->h; j++) {
        int py = gy + j;
        if ((unsigned)py >= SCR_H) continue;
        for (int i = 0; i < g->w; i++) {
            int a = bits[j * g->w + i];
            if (!a) continue;
            int px = gx + i;
            if ((unsigned)px >= SCR_W) continue;
            uint32_t *p = &backbuf[py * SCR_W + px];
            *p = blend(*p, c, a);
        }
    }
    return g->adv;
}

/* Left-aligned text with its baseline at y; returns the width drawn. */
int gfx_text(const font_t *f, int x, int y, const char *s, uint32_t c)
{
    int x0 = x;
    while (*s) x += draw_glyph(f, x, y, utf8_next(&s), c);
    return x - x0;
}

void gfx_text_center(const font_t *f, int cx, int y, const char *s, uint32_t c)
{
    gfx_text(f, cx - gfx_text_width(f, s) / 2, y, s, c);
}

void gfx_text_right(const font_t *f, int rx, int y, const char *s, uint32_t c)
{
    gfx_text(f, rx - gfx_text_width(f, s), y, s, c);
}

/* Draws as many characters as fit in maxw followed by "...". */
void gfx_text_fit(const font_t *f, int x, int y, int maxw, const char *s, uint32_t c)
{
    if (gfx_text_width(f, s) <= maxw) {
        gfx_text(f, x, y, s, c);
        return;
    }
    int dots = 3 * glyph_adv(f, '.');
    int w = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        int a = glyph_adv(f, utf8_next(&q));
        if (w + a + dots > maxw) break;
        x += draw_glyph(f, x, y, utf8_next(&p), c);
        w += a;
    }
    for (int i = 0; i < 3; i++) x += draw_glyph(f, x, y, '.', c);
}

/* Word-wraps s into at most max_lines lines of width maxw (see gfx.h). */
int gfx_text_wrap(const font_t *f, int x, int y, int maxw, int max_lines, int line_h, const char *s, uint32_t c,
                  int align, int draw)
{
    char line[512];
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        /* find the longest prefix (breaking at spaces) that fits */
        const char *q = p, *last_break = NULL;
        int w = 0;
        while (*q && *q != '\n') {
            const char *r = q;
            uint32_t cp = utf8_next(&r);
            int a = glyph_adv(f, cp);
            if (w + a > maxw) break;
            w += a;
            q = r;
            if (*q == ' ' || !*q) last_break = q;
        }
        const char *end;
        if (!*q || *q == '\n')
            end = q; /* rest of the line fits */
        else if (last_break)
            end = last_break; /* break at word */
        else
            end = q > p ? q : p + 1; /* single long word: hard break */
        int is_last = lines == max_lines - 1;
        size_t len;
        if (is_last && *end) {
            /* last allowed line but text remains: take the rest and ellipsize */
            len = strlen(p);
        }
        else {
            len = (size_t)(end - p);
        }
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        for (char *n = line; (n = strchr(n, '\n'));) *n = ' ';
        if (draw) {
            int lw = gfx_text_width(f, line);
            int lx = align == 1 ? x + (maxw - (lw < maxw ? lw : maxw)) / 2 : x;
            gfx_text_fit(f, lx, y + lines * line_h, maxw, line, c);
        }
        lines++;
        p = end;
        if (*p == '\n') p++; /* explicit line break */
        if (is_last) break;
    }
    return lines;
}

/*
 * Copies an image into the back buffer; pixels in the rounded corners are blended by
 * their coverage so the corners come out anti-aliased.
 */
void gfx_blit(int x, int y, int w, int h, const uint32_t *px, int r)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if ((unsigned)sy >= SCR_H) continue;
        const uint32_t *src = px + (size_t)j * (size_t)w;
        int cy = j < r ? r : (j >= h - r ? h - r : -1); /* corner row? */
        for (int i = 0; i < w; i++) {
            int sx = x + i;
            if ((unsigned)sx >= SCR_W) continue;
            if (cy >= 0) {
                int cx = i < r ? r : (i >= w - r ? w - r : -1);
                if (cx >= 0) {
                    int a = disc_cov((float)i + 0.5f, (float)j + 0.5f, (float)cx, (float)cy, (float)r);
                    if (a < 255) {
                        if (a) gfx_blend_px(sx, sy, src[i], a);
                        continue;
                    }
                }
            }
            backbuf[sy * SCR_W + sx] = src[i];
        }
    }
}

/* Debug: writes the back buffer as a binary PPM. */
int gfx_dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        unsigned char p[3] = {(unsigned char)(backbuf[i] >> 16), (unsigned char)(backbuf[i] >> 8),
                              (unsigned char)backbuf[i]};
        fwrite(p, 1, 3, f);
    }
    fclose(f);
    return 0;
}
