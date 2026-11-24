/* Shared drawing helpers: buttons, signal bars, icons, the top bar and toasts. */

#include <math.h>

#include "core/common.h"
#include "spotify/spotify.h"
#include "ui/internal.h"
#include "wifi/wifi.h"

/* Rounded button with an optional centred label; disabled buttons aren't tappable. */
void button(int id, int x, int y, int w, int h, const char *label, const font_t *f, int style)
{
    uint32_t bg, fg;
    int pressed = is_pressed(id);
    switch (style) {
    case BS_PRIMARY:
        bg = pressed ? C_GREEN_PRESSED : C_GREEN;
        fg = 0x000000;
        break;
    case BS_DISABLED:
        bg = C_BTN_DISABLED;
        fg = C_DIM;
        break;
    default:
        bg = pressed ? C_BTN_PRESSED : C_BTN;
        fg = C_TEXT;
        break;
    }
    gfx_rrect(x, y, w, h, h < 40 ? 8 : 12, bg);
    if (label) {
        int ty = y + (h + f->ascent - f->descent) / 2;
        int tw = gfx_text_width(f, label);
        if (tw > w - 8)
            gfx_text_fit(f, x + 4, ty, w - 8, label, fg);
        else
            gfx_text(f, x + (w - tw) / 2, ty, label, fg);
    }
    if (style != BS_DISABLED) add_hit(id, x, y, w, h);
}

/* Signal strength in dBm -> 1..4 bars (0 = unknown, shown as 3). */
int signal_bars(int dbm)
{
    if (!dbm) return 3;
    if (dbm >= -55) return 4;
    if (dbm >= -65) return 3;
    if (dbm >= -75) return 2;
    return 1;
}

#define BARS_W 18 /* 4 bars of 3px with 2px gaps */

/* Four rising bars, the first `level` in `on`, the rest in `off`. */
void draw_bars(int x, int y_bottom, int level, uint32_t on, uint32_t off)
{
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        gfx_fill(x + i * 5, y_bottom - h, 3, h, i < level ? on : off);
    }
}

/* Anti-aliased thick line as a quad (two triangles). */
void draw_line(float x0, float y0, float x1, float y1, float w, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    gfx_tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    gfx_tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

/* Signal bars with a slash through them: no connectivity. */
static void draw_bars_crossed(int x, int y_bottom, uint32_t bars, uint32_t slash, uint32_t bg)
{
    draw_bars(x, y_bottom, 4, bars, bars);
    float x0 = (float)x - 1, y0 = (float)y_bottom - 15, x1 = (float)x + BARS_W + 1, y1 = (float)y_bottom + 1;
    draw_line(x0, y0, x1, y1, 5.0f, bg); /* gap around the slash so it reads over the bars */
    draw_line(x0, y0, x1, y1, 2.0f, slash);
}

/* Sun: disc of radius r with 8 rays. */
void draw_sun(int cx, int cy, int r, uint32_t c, uint32_t bg)
{
    (void)bg;
    gfx_circle(cx, cy, r, c);
    float fx = (float)cx + 0.5f, fy = (float)cy + 0.5f;
    float r0 = (float)r + 2.5f, r1 = (float)r + 2.5f + (float)r * 0.8f;
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.785398f;
        float ca = cosf(a), sa = sinf(a);
        draw_line(fx + ca * r0, fy + sa * r0, fx + ca * r1, fy + sa * r1, r >= 8 ? 2.4f : 1.8f, c);
    }
}

/*
 * Top bar: title (the clock on the player screen), brightness shortcut, and the network
 * status (name + signal icon) which opens the Wi-Fi screen.
 */
void draw_header(const char *title)
{
    gfx_fill(0, 0, SCR_W, 44, C_HEADER);
    gfx_fill(0, 44, SCR_W, 1, C_LINE);
    gfx_text(&font_mdb, 14, 29, title, C_TEXT);

    /* Network status, right-aligned: "<label> <bars>". */
    const wifi_status *w = wifi_get();
    pthread_mutex_lock(&g_sp.mu);
    int internet_down = w->state == WS_CONNECTED && g_sp.auth == SP_READY && !g_sp.last_poll_ok;
    pthread_mutex_unlock(&g_sp.mu);

    const int bx = SCR_W - 14 - BARS_W, by = 30;
    const char *label = w->ssid;
    uint32_t label_c = C_SUB;
    switch (w->state) {
    case WS_CONNECTED:
        if (internet_down)
            draw_bars_crossed(bx, by, 0x4A4A4A, C_SUB, C_HEADER);
        else
            draw_bars(bx, by, signal_bars(w->signal), C_TEXT, 0x4A4A4A);
        break;
    case WS_CONNECTING:
    case WS_DHCP:
        /* sweeping bars while joining */
        draw_bars(bx, by, 1 + (int)((now_ms() / 500) % 4), C_SUB, 0x4A4A4A);
        label = "Connecting";
        break;
    case WS_FAILED:
        if (w->auth_failed) {
            draw_bars(bx, by, 4, C_RED, C_RED);
            label = "Wrong password";
            label_c = C_RED;
            break;
        }
        /* fall through */
    default:
        draw_bars_crossed(bx, by, 0x4A4A4A, C_SUB, C_HEADER);
        label = w->state == WS_IDLE ? "No Wi-Fi" : "Offline";
        break;
    }
    int right = bx - 8;
    int left_limit = 14 + gfx_text_width(&font_mdb, title) + 16;
    int clock_right = left_limit - 8;
    if (ui.screen == SCR_PLAYER) {
        /* brightness shortcut next to the clock */
        int sx = left_limit + 10;
        draw_sun(sx, 22, 5, is_pressed(BTN_HEADER_BRIGHT) ? C_TEXT : C_SUB, C_HEADER);
        add_hit(BTN_HEADER_BRIGHT, clock_right, 0, 36, 45);
        left_limit = clock_right + 36 + 8;
    }
    int maxw = right - left_limit;
    if (label[0] && maxw > 30) {
        int tw = gfx_text_width(&font_sm, label);
        if (tw > maxw)
            gfx_text_fit(&font_sm, right - maxw, 27, maxw, label, label_c);
        else
            gfx_text(&font_sm, right - tw, 27, label, label_c);
    }
    add_hit(BTN_HEADER_WIFI, left_limit - 8, 0, SCR_W - left_limit + 8, 45);
    if (ui.screen == SCR_PLAYER) add_hit(BTN_HEADER_CLOCK, 0, 0, clock_right, 45);
}

/* Shows the current toast (UI or Spotify) as a pill at height y. */
void draw_toast(int y)
{
    const char *msg = NULL;
    uint64_t now = now_ms();
    char buf[96];
    if (now < ui.toast_until) msg = ui.toast;
    pthread_mutex_lock(&g_sp.mu);
    if (!msg && now < g_sp.toast_until) {
        str_copy(buf, sizeof buf, g_sp.toast);
        msg = buf;
    }
    pthread_mutex_unlock(&g_sp.mu);
    if (!msg) return;
    int w = gfx_text_width(&font_sm, msg) + 28;
    if (w > SCR_W - 16) w = SCR_W - 16;
    gfx_rrect((SCR_W - w) / 2, y, w, 30, 15, 0x3A2020);
    gfx_text_fit(&font_sm, (SCR_W - w) / 2 + 14, y + 20, w - 28, msg, 0xFFB4AE);
}
