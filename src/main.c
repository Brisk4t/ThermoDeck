/* ThermoDeck: Wi-Fi + Spotify remote dashboard for the ecobee (i.MX28) touchscreen.
 *
 * Threads: UI/main (this file: framebuffer, touch, wpa_supplicant), network worker
 * (spotify.c: NTP, OAuth, polling, commands), album art (art.c) and the web server
 * (web.c: login from another device, status page).
 *
 * The UI is immediate-mode: every repaint redraws the current screen from scratch into
 * the back buffer and records the tappable areas ("hits") as it goes; a touch is matched
 * against the hits from the last repaint. Other threads never draw: they update shared
 * state and call ui_wake(), and the main loop repaints when something changed.
 */
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "art.h"
#include "common.h"
#include "config.h"
#include "gfx.h"
#include "qrcodegen.h"
#include "spotify.h"
#include "touch.h"
#include "web.h"
#include "wifi.h"

/* ---------- palette ---------- */
#define C_BG            0x121212
#define C_HEADER        0x1A1A1A
#define C_CARD          0x1E1E1E
#define C_BTN           0x2C2C2C
#define C_BTN_PRESSED   0x4A4A4A
#define C_BTN_DISABLED  0x1F1F1F
#define C_LINE          0x2A2A2A
#define C_TEXT          0xFFFFFF
#define C_SUB           0xB3B3B3
#define C_DIM           0x6E6E6E
#define C_GREEN         0x1DB954
#define C_GREEN_PRESSED 0x3BE477
#define C_RED           0xE5534B
#define C_YELLOW        0xE0B341

/* ---------- UI state ---------- */
/* screens */
enum { SCR_WIFI, SCR_KEYBOARD, SCR_PLAYER, SCR_TZ, SCR_BRIGHT };

enum {
    /* ids for everything tappable */
    BTN_NONE = 0,
    BTN_HEADER_WIFI,
    BTN_HEADER_CLOCK,
    BTN_TZ_24H,
    BTN_HEADER_BRIGHT,
    BTN_BRIGHT_SLIDER,
    BTN_BRIGHT_DOWN,
    BTN_BRIGHT_UP,
    BTN_LIST_UP,
    BTN_LIST_DOWN,
    BTN_RESCAN,
    BTN_DONE,
    BTN_FORGET,
    BTN_PREV,
    BTN_NEXT,
    BTN_PLAY,
    BTN_VOLDN,
    BTN_VOLUP,
    KEY_SHIFT,
    KEY_SYM,
    KEY_BKSP,
    KEY_SPACE,
    KEY_CANCEL,
    KEY_OK,
    KEY_SHOW,
    BTN_ROW0 = 100,  /* + row index on screen */
    KEY_CHAR = 1000, /* + character code */
};

/* scrolling list layout shared by the Wi-Fi and time zone screens */
#define LIST_Y     120
#define LIST_ROW_H 56
#define LIST_ROWS  5

/* a tappable rectangle recorded during the last repaint */
typedef struct {
    int id, x, y, w, h;
} hit;

static struct {
    int screen;
    int dirty;
    hit hits[96];
    int n_hits;
    int pressed_id;
    uint64_t pressed_until;
    uint64_t last_up_ms;
    char toast[96];
    uint64_t toast_until;

    int list_off;

    /* keyboard */
    char kb_ssid[33];
    int kb_secured;
    char kb_text[64];
    int kb_shift, kb_caps, kb_sym, kb_hide;
    uint64_t kb_last_shift;
    char kb_error[64];

    /* change tracking */
    uint32_t wifi_ver, sp_ver;
    int last_progress_s;
    int last_minute;
    wifi_state last_wifi_state;

    /* QR for the auth page */
    char qr_text[64];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int qr_ok;
    int web_port;

    /* user settings (settings.conf overrides thermodeck.ini) */
    char tz[64];
    int clock_24h;
    int brightness; /* backlight percent */
    int drag_id;    /* control being dragged (slider), 0 if none */
} ui;

/* other threads write a byte here to make the main loop repaint */
static int wake_pipe[2] = {-1, -1};

/* Wakes the UI thread (safe to call from any thread). */
void ui_wake(void)
{
    char c = 1;
    if (wake_pipe[1] >= 0) {
        ssize_t r = write(wake_pipe[1], &c, 1);
        (void)r;
    }
}

/* Short message shown at the bottom of the current screen for a few seconds. */
static void ui_toast(const char *msg)
{
    str_copy(ui.toast, sizeof ui.toast, msg);
    ui.toast_until = now_ms() + 3500;
    ui.dirty = 1;
}

/* Registers a tappable area for the frame being drawn. */
static void add_hit(int id, int x, int y, int w, int h)
{
    if (ui.n_hits < (int)ARRAY_LEN(ui.hits)) ui.hits[ui.n_hits++] = (hit){id, x, y, w, h};
}

/* Whether id was just tapped (draws the pressed highlight briefly). */
static int is_pressed(int id) { return ui.pressed_id == id && now_ms() < ui.pressed_until; }

/* ---------- widgets ---------- */

enum { BS_NORMAL, BS_PRIMARY, BS_DISABLED };

/* Rounded button with an optional centred label; disabled buttons aren't tappable. */
static void button(int id, int x, int y, int w, int h, const char *label, const font_t *f, int style)
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
static int signal_bars(int dbm)
{
    if (!dbm) return 3;
    if (dbm >= -55) return 4;
    if (dbm >= -65) return 3;
    if (dbm >= -75) return 2;
    return 1;
}

#define BARS_W 18 /* 4 bars of 3px with 2px gaps */

/* Four rising bars, the first `level` in `on`, the rest in `off`. */
static void draw_bars(int x, int y_bottom, int level, uint32_t on, uint32_t off)
{
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        gfx_fill(x + i * 5, y_bottom - h, 3, h, i < level ? on : off);
    }
}

/* Anti-aliased thick line as a quad (two triangles). */
static void draw_line(float x0, float y0, float x1, float y1, float w, uint32_t c)
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

/* Padlock icon for secured networks. */
static void draw_lock(int x, int y, uint32_t c, uint32_t bg)
{
    /* 10x13 padlock: shackle ring + body */
    gfx_ring(x + 5, y + 5, 4, 2, c);
    gfx_fill(x + 1, y + 5, 2, 2, c);
    gfx_fill(x + 7, y + 5, 2, 2, c);
    gfx_rrect(x, y + 6, 10, 7, 2, c);
    (void)bg;
}

/* Sun: disc of radius r with 8 rays. */
static void draw_sun(int cx, int cy, int r, uint32_t c, uint32_t bg)
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
static void draw_header(const char *title)
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
static void draw_toast(int y)
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

/* ---------- clock / time zone ---------- */

/* POSIX TZ strings (no zoneinfo needed). Ordered west to east. */
static const struct {
    const char *name, *places, *tz;
} ZONES[] = {
    {"Hawaii", "Honolulu", "HST10"},
    {"Alaska", "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific", "Vancouver, Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Mountain", "Edmonton, Calgary, Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Mountain (no DST)", "Arizona", "MST7"},
    {"Central", "Winnipeg, Chicago, Mexico City", "CST6CDT,M3.2.0,M11.1.0"},
    {"Central (no DST)", "Saskatchewan", "CST6"},
    {"Eastern", "Toronto, Montreal, New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Atlantic", "Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"Newfoundland", "St. John's", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Brazil / Argentina", "Sao Paulo, Buenos Aires", "<-03>3"},
    {"UTC", "Coordinated Universal Time", "UTC0"},
    {"UK / Ireland", "London, Dublin, Lisbon", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "Paris, Berlin, Madrid, Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "Athens, Helsinki, Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow / Istanbul", "Moscow, Istanbul, Riyadh", "<+03>-3"},
    {"Gulf", "Dubai, Abu Dhabi", "<+04>-4"},
    {"Pakistan", "Karachi, Tashkent", "<+05>-5"},
    {"India", "Delhi, Mumbai, Colombo", "IST-5:30"},
    {"Bangladesh", "Dhaka", "<+06>-6"},
    {"Indochina", "Bangkok, Jakarta, Hanoi", "<+07>-7"},
    {"China / Singapore", "Beijing, Singapore, Perth", "<+08>-8"},
    {"Japan / Korea", "Tokyo, Seoul", "JST-9"},
    {"Australia East", "Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Queensland", "Brisbane", "AEST-10"},
    {"New Zealand", "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
#define N_ZONES ((int)ARRAY_LEN(ZONES))

/* Applies the selected zone to this process (localtime uses TZ). */
static void clock_apply(void)
{
    setenv("TZ", ui.tz[0] ? ui.tz : "UTC0", 1);
    tzset();
}

#define BRIGHT_MIN 5

/* Sets the backlight (skips the sysfs writes if nothing changed). */
static void brightness_apply(int pct)
{
    static int applied = -1;
    pct = CLAMP(pct, BRIGHT_MIN, 100);
    if (pct != applied) {
        gfx_set_backlight(pct);
        applied = pct;
    }
}

/* User settings changed on the device live in settings.conf and override thermodeck.ini.
 * (Older builds wrote clock.conf; it's read once if settings.conf doesn't exist yet.) */
static void settings_load(void)
{
    str_copy(ui.tz, sizeof ui.tz, config_get("display", "timezone", "UTC0"));
    ui.clock_24h = config_int("display", "clock_24h", 0);
    ui.brightness = config_int("display", "brightness", 80);
    char p[300];
    path_join(p, sizeof p, "settings.conf");
    char *d = file_read(p, NULL);
    if (!d) {
        path_join(p, sizeof p, "clock.conf");
        d = file_read(p, NULL);
    }
    if (d) {
        for (char *line = strtok(d, "\n"); line; line = strtok(NULL, "\n")) {
            if (!strncmp(line, "tz=", 3))
                str_copy(ui.tz, sizeof ui.tz, line + 3);
            else if (!strncmp(line, "clock_24h=", 10))
                ui.clock_24h = atoi(line + 10);
            else if (!strncmp(line, "brightness=", 11))
                ui.brightness = atoi(line + 11);
        }
        free(d);
    }
    ui.brightness = CLAMP(ui.brightness, BRIGHT_MIN, 100);
    clock_apply();
    tdlog("settings: TZ=%s, %d-hour, brightness %d%%", ui.tz, ui.clock_24h ? 24 : 12, ui.brightness);
}

static void settings_save(void)
{
    char p[300], buf[160];
    path_join(p, sizeof p, "settings.conf");
    int n = snprintf(buf, sizeof buf, "tz=%s\nclock_24h=%d\nbrightness=%d\n", ui.tz, ui.clock_24h, ui.brightness);
    if (file_write_atomic(p, buf, (size_t)n, 0644) < 0) tdlog("settings: could not save %s", p);
}

/* "6:45 AM" / "06:45" for time t in the current TZ. */
static void fmt_clock(time_t t, char *out, size_t outlen)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (ui.clock_24h)
        strftime(out, outlen, "%H:%M", &tm);
    else {
        strftime(out, outlen, "%I:%M %p", &tm);
        if (out[0] == '0') memmove(out, out + 1, strlen(out));
    }
}

/* Same, but in another zone (briefly switches TZ; only the UI thread uses local time). */
static void fmt_clock_in(const char *tz, time_t t, char *out, size_t outlen)
{
    setenv("TZ", tz, 1);
    tzset();
    fmt_clock(t, out, outlen);
}

/* Index of tz in ZONES, or -1 for a custom zone from the ini. */
static int zone_index(const char *tz)
{
    for (int i = 0; i < N_ZONES; i++)
        if (!strcmp(ZONES[i].tz, tz)) return i;
    return -1;
}

/* Opens the time zone screen scrolled to the current zone. */
static void open_tz(void)
{
    ui.screen = SCR_TZ;
    int cur = zone_index(ui.tz);
    ui.list_off = cur < 0 ? 0 : cur - LIST_ROWS / 2;
}

/*
 * Time zone picker: current time + 12/24 h toggle, then the zone list with each zone's
 * local time so the right one is easy to spot.
 */
static void render_tz(void)
{
    draw_header("Time zone");
    time_t now = time(NULL);
    int cur = zone_index(ui.tz);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    ui.last_minute = now_tm.tm_min;

    /* current setting */
    char t[16];
    fmt_clock(now, t, sizeof t);
    gfx_rrect(8, 52, SCR_W - 16, 60, 12, C_CARD);
    gfx_text(&font_lg, 22, 81, t, C_TEXT);
    gfx_text_fit(&font_sm, 22, 101, SCR_W - 104 - 8 - 22, cur >= 0 ? ZONES[cur].name : ui.tz, C_SUB);
    button(BTN_TZ_24H, SCR_W - 104, 62, 88, 40, ui.clock_24h ? "24-hour" : "12-hour", &font_mdb, BS_NORMAL);

    /* zones, each with its local time right now */
    int n = N_ZONES;
    if (ui.list_off > n - LIST_ROWS) ui.list_off = n - LIST_ROWS;
    if (ui.list_off < 0) ui.list_off = 0;
    for (int r = 0; r < LIST_ROWS && ui.list_off + r < n; r++) {
        int i = ui.list_off + r;
        int y = LIST_Y + r * LIST_ROW_H;
        int sel = i == cur;
        if (is_pressed(BTN_ROW0 + r)) gfx_fill(0, y, SCR_W, LIST_ROW_H, 0x262626);
        gfx_fill(16, y + LIST_ROW_H - 1, SCR_W - 32, 1, C_LINE);
        fmt_clock_in(ZONES[i].tz, now, t, sizeof t);
        int tw = gfx_text_width(&font_mdb, t);
        gfx_text_right(&font_mdb, SCR_W - 18, y + 35, t, sel ? C_GREEN : C_SUB);
        int maxw = SCR_W - 18 - tw - 12 - 18;
        gfx_text_fit(&font_md, 18, y + 27, maxw, ZONES[i].name, sel ? C_GREEN : C_TEXT);
        gfx_text_fit(&font_sm, 18, y + 46, maxw, ZONES[i].places, C_DIM);
        add_hit(BTN_ROW0 + r, 0, y, SCR_W, LIST_ROW_H);
    }
    clock_apply(); /* back to the selected zone */
    int track = LIST_ROWS * LIST_ROW_H;
    gfx_rrect(SCR_W - 6, LIST_Y + track * ui.list_off / n, 3, track * LIST_ROWS / n, 1, 0x555555);

    int by = 412, bh = 60;
    button(BTN_LIST_UP, 6, by, 72, bh, NULL, &font_md, ui.list_off > 0 ? BS_NORMAL : BS_DISABLED);
    gfx_tri(42 - 10, by + 36, 42 + 10, by + 36, 42, by + 22, ui.list_off > 0 ? C_TEXT : C_DIM);
    button(BTN_DONE, 86, by, 148, bh, "Done", &font_mdb, BS_PRIMARY);
    int can_down = ui.list_off + LIST_ROWS < n;
    button(BTN_LIST_DOWN, 242, by, 72, bh, NULL, &font_md, can_down ? BS_NORMAL : BS_DISABLED);
    gfx_tri(278 - 10, by + 24, 278 + 10, by + 24, 278, by + 38, can_down ? C_TEXT : C_DIM);
}

/* ---------- brightness ---------- */

#define SL_X0 50
#define SL_X1 (SCR_W - 50)
#define SL_Y  250

/* Changes brightness live (saved when the finger lifts or a step button is tapped). */
static void brightness_set(int pct)
{
    pct = CLAMP(pct, BRIGHT_MIN, 100);
    if (pct == ui.brightness) return;
    ui.brightness = pct;
    brightness_apply(pct);
    ui.dirty = 1;
}

/* Finger x position on the slider -> brightness. */
static void slider_touch(int x)
{
    int pct = BRIGHT_MIN + (x - SL_X0) * (100 - BRIGHT_MIN) / (SL_X1 - SL_X0);
    brightness_set(pct);
}

/* Brightness screen: big percentage, draggable slider, -/+ steps of 10%. */
static void render_bright(void)
{
    draw_header("Brightness");

    char v[8];
    snprintf(v, sizeof v, "%d%%", ui.brightness);
    draw_sun(SCR_W / 2, 118, 12, C_TEXT, C_BG);
    gfx_text_center(&font_xl, SCR_W / 2, 196, v, C_TEXT);

    /* slider: track, fill, knob */
    int kx = SL_X0 + (ui.brightness - BRIGHT_MIN) * (SL_X1 - SL_X0) / (100 - BRIGHT_MIN);
    gfx_rrect(SL_X0, SL_Y - 4, SL_X1 - SL_X0, 8, 4, 0x3E3E3E);
    gfx_rrect(SL_X0, SL_Y - 4, kx - SL_X0 + 4, 8, 4, C_GREEN);
    gfx_circle(kx, SL_Y, ui.drag_id == BTN_BRIGHT_SLIDER ? 16 : 13, C_TEXT);
    draw_sun(SL_X0 - 26, SL_Y, 3, C_DIM, C_BG);
    draw_sun(SL_X1 + 26, SL_Y, 5, C_SUB, C_BG);
    add_hit(BTN_BRIGHT_SLIDER, 0, SL_Y - 34, SCR_W, 68);

    /* fine steps */
    button(BTN_BRIGHT_DOWN, 20, 306, 134, 64, NULL, &font_md, ui.brightness > BRIGHT_MIN ? BS_NORMAL : BS_DISABLED);
    gfx_rrect(20 + 67 - 9, 306 + 31, 18, 3, 1, ui.brightness > BRIGHT_MIN ? C_TEXT : C_DIM);
    button(BTN_BRIGHT_UP, 166, 306, 134, 64, NULL, &font_md, ui.brightness < 100 ? BS_NORMAL : BS_DISABLED);
    uint32_t pc = ui.brightness < 100 ? C_TEXT : C_DIM;
    gfx_rrect(166 + 67 - 9, 306 + 31, 18, 3, 1, pc);
    gfx_rrect(166 + 67 - 1, 306 + 23, 3, 18, 1, pc);

    button(BTN_DONE, 20, 412, SCR_W - 40, 60, "Done", &font_mdb, BS_PRIMARY);
}

/* ---------- Wi-Fi screen ---------- */

/* Wi-Fi screen: status card, scrollable list of networks, scroll / rescan / done buttons. */
static void render_wifi(void)
{
    const wifi_status *w = wifi_get();
    draw_header("Wi-Fi");

    /* status card */
    gfx_rrect(8, 52, SCR_W - 16, 60, 12, C_CARD);
    const char *l1 = "Not connected", *dotc = NULL;
    char l2[96] = "Choose a network below";
    uint32_t dc = C_RED;
    switch (w->state) {
    case WS_CONNECTED:
        l1 = "Connected";
        dc = C_GREEN;
        snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s", w->ssid, w->ip);
        break;
    case WS_DHCP:
        l1 = "Getting IP address...";
        dc = C_YELLOW;
        str_copy(l2, sizeof l2, w->ssid);
        break;
    case WS_CONNECTING:
        l1 = "Connecting...";
        dc = C_YELLOW;
        str_copy(l2, sizeof l2, w->ssid);
        break;
    case WS_FAILED:
        l1 = "Connection failed";
        snprintf(l2, sizeof l2, "%s%s%s", w->ssid, w->ssid[0] ? ": " : "", w->message);
        break;
    default: break;
    }
    (void)dotc;
    gfx_circle(28, 82, 6, dc);
    int has_forget = w->ssid[0] && wifi_is_saved(w->ssid) && w->state != WS_IDLE;
    int textw = has_forget ? 190 : 260;
    gfx_text_fit(&font_mdb, 44, 77, textw, l1, C_TEXT);
    gfx_text_fit(&font_sm, 44, 99, textw, l2, C_SUB);
    if (has_forget) button(BTN_FORGET, SCR_W - 90, 64, 74, 36, "Forget", &font_sm, BS_NORMAL);

    /* network list */
    int n = w->n_aps;
    if (ui.list_off > n - LIST_ROWS) ui.list_off = n - LIST_ROWS;
    if (ui.list_off < 0) ui.list_off = 0;
    if (n == 0) {
        gfx_text_center(&font_md, SCR_W / 2, LIST_Y + 110, w->scanning ? "Scanning..." : "No networks found", C_SUB);
    }
    for (int r = 0; r < LIST_ROWS && ui.list_off + r < n; r++) {
        const wifi_ap *ap = &w->aps[ui.list_off + r];
        int y = LIST_Y + r * LIST_ROW_H;
        int current = !strcmp(ap->ssid, w->ssid) && w->state != WS_IDLE;
        if (is_pressed(BTN_ROW0 + r)) gfx_fill(0, y, SCR_W, LIST_ROW_H, 0x262626);
        gfx_fill(16, y + LIST_ROW_H - 1, SCR_W - 32, 1, C_LINE);
        uint32_t tc = ap->secured == 3 ? C_DIM : current ? C_GREEN : C_TEXT;
        gfx_text_fit(&font_md, 18, y + 35, 218, ap->ssid, tc);
        if (wifi_is_saved(ap->ssid) && !current) gfx_text(&font_sm, 18, y + 51, "Saved", C_DIM);
        if (ap->secured) draw_lock(250, y + 20, ap->secured == 3 ? C_DIM : C_SUB, C_BG);
        draw_bars(272, y + 36, signal_bars(ap->signal), current ? C_GREEN : C_TEXT, 0x444444);
        add_hit(BTN_ROW0 + r, 0, y, SCR_W, LIST_ROW_H);
    }
    if (n > LIST_ROWS) {
        /* scroll position indicator */
        int track = LIST_ROWS * LIST_ROW_H;
        int th = track * LIST_ROWS / n;
        int ty = LIST_Y + track * ui.list_off / n;
        gfx_rrect(SCR_W - 6, ty, 3, th, 1, 0x555555);
    }

    /* bottom bar */
    int by = 412, bh = 60;
    button(BTN_LIST_UP, 6, by, 52, bh, NULL, &font_md, ui.list_off > 0 ? BS_NORMAL : BS_DISABLED);
    gfx_tri(32 - 10, by + 36, 32 + 10, by + 36, 32, by + 22, ui.list_off > 0 ? C_TEXT : C_DIM);
    button(BTN_RESCAN, 64, by, 94, bh, w->scanning ? "Scanning" : "Rescan", &font_mdb,
           w->scanning ? BS_DISABLED : BS_NORMAL);
    button(BTN_DONE, 164, by, 94, bh, "Done", &font_mdb, w->state == WS_CONNECTED ? BS_PRIMARY : BS_DISABLED);
    int can_down = ui.list_off + LIST_ROWS < n;
    button(BTN_LIST_DOWN, 264, by, 52, bh, NULL, &font_md, can_down ? BS_NORMAL : BS_DISABLED);
    gfx_tri(290 - 10, by + 24, 290 + 10, by + 24, 290, by + 38, can_down ? C_TEXT : C_DIM);

    draw_toast(372);
}

/* ---------- keyboard screen ---------- */

static const char *KB_ROWS[2][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",./?`", "~<>"},
};
#define KB_Y     216
#define KB_KEY_H 52

/* One keyboard key. */
static void key(int id, int x, int y, int w, const char *label, const font_t *f, uint32_t bg)
{
    int pressed = is_pressed(id);
    gfx_rrect(x + 2, y + 3, w - 4, KB_KEY_H - 6, 7, pressed ? C_BTN_PRESSED : bg);
    int ty = y + (KB_KEY_H + f->ascent - f->descent) / 2;
    gfx_text_center(f, x + w / 2, ty, label, C_TEXT);
    add_hit(id, x, y, w, KB_KEY_H);
}

/*
 * On-screen keyboard for the Wi-Fi password: text field (optionally masked), letters /
 * symbols, shift (double tap = caps lock), delete, space, cancel and join.
 */
static void render_keyboard(void)
{
    draw_header("Wi-Fi password");
    gfx_text(&font_sm, 14, 66, "Network", C_SUB);
    gfx_text_fit(&font_mdb, 14, 90, SCR_W - 28, ui.kb_ssid, C_TEXT);

    /* text field */
    gfx_rrect(10, 102, SCR_W - 20, 50, 10, C_GREEN);
    gfx_rrect(12, 104, SCR_W - 24, 46, 9, 0x232323);
    char shown[200];
    size_t len = strlen(ui.kb_text);
    if (ui.kb_hide) {
        size_t o = 0;
        for (size_t i = 0; i < len && o + 3 < sizeof shown; i++) {
            shown[o++] = (char)0xC2; /* U+00B7 middle dot */
            shown[o++] = (char)0xB7;
        }
        shown[o] = 0;
    }
    else {
        str_copy(shown, sizeof shown, ui.kb_text);
    }
    /* show the tail if it doesn't fit */
    const char *disp = shown;
    int maxw = 214;
    while (*disp && gfx_text_width(&font_md, disp) > maxw) {
        const char *p = disp;
        utf8_next(&p);
        disp = p;
    }
    int tw = gfx_text(&font_md, 24, 134, disp, C_TEXT);
    if ((now_ms() / 500) % 2 == 0) gfx_fill(24 + tw + 1, 114, 2, 26, C_GREEN);
    button(KEY_SHOW, SCR_W - 76, 111, 60, 32, ui.kb_hide ? "Show" : "Hide", &font_sm, BS_NORMAL);

    if (ui.kb_error[0])
        gfx_text(&font_sm, 14, 176, ui.kb_error, C_RED);
    else
        gfx_text(&font_sm, 14, 176,
                 ui.kb_secured == 2 ? "WEP key (5/13 chars or 10/26 hex)" : "WPA password (8-63 characters)", C_DIM);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%zu", len);
    gfx_text_right(&font_sm, SCR_W - 14, 176, cnt, C_DIM);

    /* keys */
    gfx_fill(0, KB_Y - 6, SCR_W, SCR_H - KB_Y + 6, 0x0C0C0C);
    int upper = ui.kb_shift || ui.kb_caps;
    for (int r = 0; r < 4; r++) {
        const char *row = KB_ROWS[ui.kb_sym][r];
        int nk = (int)strlen(row);
        int y = KB_Y + r * KB_KEY_H;
        int x0 = r == 2 ? 16 : r == 3 ? 48 : 0;
        for (int i = 0; i < nk; i++) {
            char c = row[i];
            if (upper && !ui.kb_sym && c >= 'a' && c <= 'z') c = (char)(c - 32);
            char lab[2] = {c, 0};
            key(KEY_CHAR + (unsigned char)c, x0 + i * 32, y, 32, lab, &font_md, 0x333333);
        }
    }
    int y3 = KB_Y + 3 * KB_KEY_H;
    key(KEY_SHIFT, 0, y3, 48, ui.kb_caps ? "CAPS" : "Shift", &font_sm, upper && !ui.kb_sym ? 0x2E6B45 : 0x262626);
    key(KEY_BKSP, 272, y3, 48, "Del", &font_sm, 0x262626);
    int y4 = KB_Y + 4 * KB_KEY_H;
    key(KEY_SYM, 0, y4, 64, ui.kb_sym ? "ABC" : "?123", &font_sm, 0x262626);
    key(KEY_SPACE, 64, y4, 128, "space", &font_sm, 0x333333);
    key(KEY_CANCEL, 192, y4, 64, "Cancel", &font_sm, 0x262626);
    gfx_rrect(256 + 2, y4 + 3, 60, KB_KEY_H - 6, 7, is_pressed(KEY_OK) ? C_GREEN_PRESSED : C_GREEN);
    gfx_text_center(&font_mdb, 288, y4 + 32, "Join", 0x000000);
    add_hit(KEY_OK, 256, y4, 64, KB_KEY_H);
}

/* ---------- player / Spotify screen ---------- */

/* "m:ss" for a track position. */
static void fmt_time(int ms, char *out, size_t len)
{
    int s = ms / 1000;
    if (s >= 3600)
        snprintf(out, len, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, len, "%d:%02d", s / 60, s % 60);
}

/* Transport and volume icons, centred on (cx, cy). */
static void draw_prev_icon(int cx, int cy, uint32_t c)
{
    gfx_fill(cx - 13, cy - 12, 4, 24, c);
    gfx_tri((float)cx + 13, (float)cy - 12, (float)cx + 13, (float)cy + 12, (float)cx - 9, (float)cy, c);
}

static void draw_next_icon(int cx, int cy, uint32_t c)
{
    gfx_fill(cx + 9, cy - 12, 4, 24, c);
    gfx_tri((float)cx - 13, (float)cy - 12, (float)cx - 13, (float)cy + 12, (float)cx + 9, (float)cy, c);
}

/* Speaker (box + cone) followed by a + or - sign, centred on (cx, cy). ~34x20. */
static void draw_volume_icon(int cx, int cy, int plus, uint32_t c)
{
    int x = cx - 17;
    gfx_rrect(x, cy - 4, 6, 8, 1, c); /* magnet box */
    gfx_tri((float)x + 5, (float)cy - 4, (float)x + 15, (float)cy - 10, (float)x + 15, (float)cy + 10, c); /* cone */
    gfx_tri((float)x + 5, (float)cy - 4, (float)x + 15, (float)cy + 10, (float)x + 5, (float)cy + 4, c);
    int sx = x + 21; /* sign, 13x13 */
    gfx_rrect(sx, cy - 1, 13, 3, 1, c);
    if (plus) gfx_rrect(sx + 5, cy - 6, 3, 13, 1, c);
}

static void draw_play_icon(int cx, int cy, uint32_t c)
{
    gfx_tri((float)cx - 9, (float)cy - 14, (float)cx - 9, (float)cy + 14, (float)cx + 15, (float)cy, c);
}

static void draw_pause_icon(int cx, int cy, uint32_t c)
{
    gfx_rrect(cx - 11, cy - 13, 8, 26, 2, c);
    gfx_rrect(cx + 3, cy - 13, 8, 26, 2, c);
}

/* Placeholder while art loads / for items without art: a dim eighth note. */
static void draw_art_placeholder(int x, int y, int s)
{
    gfx_rrect(x, y, s, s, 10, C_CARD);
    int cx = x + s / 2, cy = y + s / 2;
    gfx_circle(cx - 8, cy + 18, 11, 0x3A3A3A);
    gfx_fill(cx + 1, cy - 26, 5, 44, 0x3A3A3A);
    gfx_tri((float)cx + 6, (float)cy - 26, (float)cx + 6, (float)cy - 12, (float)cx + 22, (float)cy - 10, 0x3A3A3A);
}

/* Regenerates the QR code for the web page URL when the IP changes. */
static void update_qr(const char *ip)
{
    char text[64];
    snprintf(text, sizeof text, "http://%s:%d/", ip, ui.web_port);
    if (!strcmp(text, ui.qr_text)) return;
    str_copy(ui.qr_text, sizeof ui.qr_text, text);
    static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    ui.qr_ok = qrcodegen_encodeText(text, tmp, ui.qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 10,
                                    qrcodegen_Mask_AUTO, true);
}

/* Draws the QR code on a white card, as large as fits in max_px. */
static void draw_qr(int cx, int y, int max_px)
{
    if (!ui.qr_ok) return;
    int size = qrcodegen_getSize(ui.qr);
    int quiet = 3;
    int scale = max_px / (size + 2 * quiet);
    if (scale < 1) scale = 1;
    int px = (size + 2 * quiet) * scale;
    int x0 = cx - px / 2;
    gfx_rrect(x0 - 4, y - 4, px + 8, px + 8, 10, 0xFFFFFF);
    for (int my = 0; my < size; my++)
        for (int mx = 0; mx < size; mx++)
            if (qrcodegen_getModule(ui.qr, mx, my))
                gfx_fill(x0 + (mx + quiet) * scale, y + (my + quiet) * scale, scale, scale, 0x000000);
}

/*
 * Main screen. Depending on the Spotify state it shows: waiting for Wi-Fi, missing
 * config, the login screen (QR code + PIN), or the player itself.
 */
static void render_player(void)
{
    /* header with clock */
    char title[16] = "ThermoDeck";
    pthread_mutex_lock(&g_sp.mu);
    int synced = g_sp.time_synced;
    pthread_mutex_unlock(&g_sp.mu);
    if (synced) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fmt_clock(t, title, sizeof title);
        ui.last_minute = tm.tm_min;
    }
    draw_header(title);

    /* snapshot the shared state */
    pthread_mutex_lock(&g_sp.mu);
    sp_auth_state auth = g_sp.auth;
    char msg[128], t[256], artist[256], album[256], device[64], ip[16];
    str_copy(msg, sizeof msg, g_sp.auth_msg);
    str_copy(t, sizeof t, g_sp.title);
    str_copy(artist, sizeof artist, g_sp.artist);
    str_copy(album, sizeof album, g_sp.album);
    str_copy(device, sizeof device, g_sp.device);
    str_copy(ip, sizeof ip, g_sp.ip);
    int has_item = g_sp.has_item, playing = g_sp.is_playing, duration = g_sp.duration_ms, volume = g_sp.volume;
    int sup_vol = g_sp.supports_volume;
    int poll_ok = g_sp.last_poll_ok;
    pthread_mutex_unlock(&g_sp.mu);

    if (auth == SP_WAIT_NETWORK) {
        const wifi_status *w = wifi_get();
        gfx_text_center(&font_lg, SCR_W / 2, 200, "Waiting for Wi-Fi", C_TEXT);
        gfx_text_center(&font_sm, SCR_W / 2, 228,
                        w->state == WS_CONNECTED ? "Starting..." : "Tap the Wi-Fi status above to set up", C_SUB);
        return;
    }
    if (auth == SP_NO_CONFIG) {
        gfx_rrect(12, 70, SCR_W - 24, 220, 14, C_CARD);
        gfx_text(&font_lg, 28, 110, "Spotify not set up", C_TEXT);
        char path[300];
        snprintf(path, sizeof path,
                 "Add your Spotify app's client_id and client_secret to %s/thermodeck.ini, then restart.", g_dir);
        gfx_text_wrap(&font_md, 28, 146, SCR_W - 56, 6, 22, path, C_SUB, 0, 1);
        return;
    }
    if (auth == SP_NEED_AUTH || auth == SP_AUTHORIZING) {
        update_qr(ip);
        gfx_text_center(&font_lg, SCR_W / 2, 78, "Connect Spotify", C_TEXT);
        gfx_text_center(&font_sm, SCR_W / 2, 100, "Scan or open on your phone / computer:", C_SUB);
        draw_qr(SCR_W / 2, 112, 148);
        char url[64];
        snprintf(url, sizeof url, "http://%s:%d", ip, ui.web_port);
        gfx_text_center(&font_mdb, SCR_W / 2, 290, url, C_GREEN);
        char pin[7], pin_disp[16];
        web_get_pin(pin);
        snprintf(pin_disp, sizeof pin_disp, "%.3s %.3s", pin, pin + 3);
        gfx_text_center(&font_sm, SCR_W / 2, 316, "PIN", C_SUB);
        gfx_text_center(&font_xl, SCR_W / 2, 352, pin_disp, C_TEXT);
        gfx_text_wrap(&font_sm, 20, 382, SCR_W - 40, 2, 17,
                      "Enter the PIN, log in with Spotify, then paste the address you end up on.", C_SUB, 1, 1);
        gfx_text_center(&font_sm, SCR_W / 2, 424, "Console: /config/thermodeck/auth.sh", C_DIM);
        if (auth == SP_AUTHORIZING)
            gfx_text_center(&font_mdb, SCR_W / 2, 458, "Authorizing...", C_YELLOW);
        else if (msg[0])
            gfx_text_wrap(&font_sm, 16, 450, SCR_W - 32, 2, 16, msg, C_RED, 1, 1);
        return;
    }

    /* READY: now playing */
    int y = 74;
    if (!poll_ok)
        gfx_text(&font_sm, 20, y, "RECONNECTING...", C_YELLOW);
    else if (has_item)
        gfx_text(&font_sm, 20, y, playing ? "NOW PLAYING" : "PAUSED", playing ? C_GREEN : C_SUB);
    if (device[0]) {
        char on[80];
        snprintf(on, sizeof on, "on %s", device);
        int w = gfx_text_width(&font_sm, on);
        if (w > 170) w = 170;
        gfx_text_fit(&font_sm, SCR_W - 20 - w, y, 170, on, C_DIM);
    }
    const int ax = (SCR_W - ART_PX) / 2, ay = 86;
    if (has_item) {
        if (!art_draw(ax, ay, 10)) draw_art_placeholder(ax, ay, ART_PX);
        gfx_text_wrap(&font_lg, 16, 258, SCR_W - 32, 1, 29, t, C_TEXT, 1, 1);
        gfx_text_wrap(&font_md, 16, 280, SCR_W - 32, 1, 22, artist, C_SUB, 1, 1);
        if (album[0]) gfx_text_wrap(&font_sm, 16, 298, SCR_W - 32, 1, 17, album, C_DIM, 1, 1);
    }
    else {
        draw_art_placeholder(ax, ay, ART_PX);
        gfx_text_center(&font_lg, SCR_W / 2, 262, "Nothing playing", C_TEXT);
        gfx_text_center(&font_sm, SCR_W / 2, 286, "Start Spotify on any device", C_SUB);
    }

    /* progress: last sync + local stopwatch */
    int prog = has_item ? spotify_progress_now() : 0;
    ui.last_progress_s = prog / 1000;
    int bx = 20, bw = SCR_W - 40, by = 307;
    gfx_rrect(bx, by, bw, 6, 3, 0x3E3E3E);
    if (has_item && duration > 0) {
        int fw = (int)((long long)bw * prog / duration);
        if (fw > 0) gfx_rrect(bx, by, fw < 6 ? 6 : fw, 6, 3, playing ? C_TEXT : C_SUB);
        gfx_circle(bx + fw, by + 3, 7, C_TEXT);
        char a[16], b[16];
        fmt_time(prog, a, sizeof a);
        fmt_time(duration, b, sizeof b);
        gfx_text(&font_sm, bx, 331, a, C_SUB);
        gfx_text_right(&font_sm, bx + bw, 331, b, C_SUB);
    }
    else {
        gfx_text(&font_sm, bx, 331, "-:--", C_DIM);
        gfx_text_right(&font_sm, bx + bw, 331, "-:--", C_DIM);
    }

    /* controls */
    const int cy = 342, ch = 72;
    button(BTN_PREV, 20, cy, 88, ch, NULL, &font_md, BS_NORMAL);
    draw_prev_icon(20 + 44, cy + ch / 2, C_TEXT);
    button(BTN_PLAY, 116, cy, 88, ch, NULL, &font_md, has_item ? BS_PRIMARY : BS_DISABLED);
    if (playing)
        draw_pause_icon(116 + 44, cy + ch / 2, has_item ? 0x000000 : C_DIM);
    else
        draw_play_icon(116 + 44, cy + ch / 2, has_item ? 0x000000 : C_DIM);
    button(BTN_NEXT, 212, cy, 88, ch, NULL, &font_md, BS_NORMAL);
    draw_next_icon(212 + 44, cy + ch / 2, C_TEXT);

    /* Volume: some devices (phones, some speakers) don't allow remote volume. */
    int vs = sup_vol ? BS_NORMAL : BS_DISABLED;
    button(BTN_VOLDN, 20, 422, 84, 50, NULL, &font_mdb, vs);
    draw_volume_icon(20 + 42, 422 + 25, 0, sup_vol ? C_TEXT : C_DIM);
    button(BTN_VOLUP, 216, 422, 84, 50, NULL, &font_mdb, vs);
    draw_volume_icon(216 + 42, 422 + 25, 1, sup_vol ? C_TEXT : C_DIM);
    if (!sup_vol) {
        /* still tappable, to explain why it's greyed out */
        add_hit(BTN_VOLDN, 20, 422, 84, 50);
        add_hit(BTN_VOLUP, 216, 422, 84, 50);
    }
    char vl[24];
    if (volume >= 0)
        snprintf(vl, sizeof vl, "%d%%", volume);
    else
        str_copy(vl, sizeof vl, "--");
    gfx_text_center(&font_lg, SCR_W / 2, 452, vl, sup_vol ? C_TEXT : C_DIM);
    gfx_text_center(&font_sm, SCR_W / 2, 468, sup_vol ? "volume" : "fixed", C_DIM);
    /* volume level bar under the number */
    if (volume >= 0 && sup_vol) {
        gfx_fill(116, 426, 88, 3, 0x3E3E3E);
        gfx_fill(116, 426, 88 * volume / 100, 3, C_GREEN);
    }

    draw_toast(196);
}

/* ---------- render + input ---------- */

/* Repaints the current screen and pushes it to the display. */
static void render(void)
{
    ui.n_hits = 0;
    gfx_clear(C_BG);
    switch (ui.screen) {
    case SCR_WIFI: render_wifi(); break;
    case SCR_KEYBOARD: render_keyboard(); break;
    case SCR_TZ: render_tz(); break;
    case SCR_BRIGHT: render_bright(); break;
    default: render_player(); break;
    }
    gfx_flush();
    ui.dirty = 0;
}

/* Opens the password keyboard for a network. */
static void open_keyboard(const char *ssid, int secured)
{
    ui.screen = SCR_KEYBOARD;
    str_copy(ui.kb_ssid, sizeof ui.kb_ssid, ssid);
    ui.kb_secured = secured;
    ui.kb_text[0] = 0;
    ui.kb_shift = ui.kb_caps = ui.kb_sym = 0;
    ui.kb_hide = 0;
    ui.kb_error[0] = 0;
}

/* Validates the password length for the security type, then starts connecting. */
static void kb_submit(void)
{
    size_t l = strlen(ui.kb_text);
    if (ui.kb_secured == 1 && (l < 8 || l > 63)) {
        str_copy(ui.kb_error, sizeof ui.kb_error, "Password must be 8-63 characters");
        return;
    }
    if (ui.kb_secured == 2 && !(l == 5 || l == 13 || l == 10 || l == 26)) {
        str_copy(ui.kb_error, sizeof ui.kb_error, "WEP key must be 5, 10, 13 or 26 chars");
        return;
    }
    /* deriving the WPA key takes a moment on this CPU: show feedback first */
    ui.screen = SCR_WIFI;
    ui_toast("Joining network...");
    render();
    wifi_connect(ui.kb_ssid, ui.kb_text, ui.kb_secured);
    memset(ui.kb_text, 0, sizeof ui.kb_text);
}

/* Acts on a tap. Keys and list rows are id ranges; everything else is a single id. */
static void on_press(int id)
{
    const wifi_status *w = wifi_get();
    if (id >= KEY_CHAR) {
        size_t l = strlen(ui.kb_text);
        if (l + 1 < sizeof ui.kb_text) {
            ui.kb_text[l] = (char)(id - KEY_CHAR);
            ui.kb_text[l + 1] = 0;
        }
        ui.kb_error[0] = 0;
        if (ui.kb_shift && !ui.kb_caps) ui.kb_shift = 0;
        return;
    }
    if (ui.screen == SCR_TZ && id >= BTN_ROW0 && id < BTN_ROW0 + LIST_ROWS) {
        int idx = ui.list_off + (id - BTN_ROW0);
        if (idx < N_ZONES && strcmp(ui.tz, ZONES[idx].tz)) {
            str_copy(ui.tz, sizeof ui.tz, ZONES[idx].tz);
            clock_apply();
            settings_save();
            tdlog("clock: time zone set to %s (%s)", ZONES[idx].name, ui.tz);
        }
        return;
    }
    if (id >= BTN_ROW0 && id < BTN_ROW0 + LIST_ROWS) {
        int idx = ui.list_off + (id - BTN_ROW0);
        if (idx >= w->n_aps) return;
        wifi_ap ap = w->aps[idx];
        if (ap.secured == 3) {
            ui_toast("Enterprise / WPA3-only networks aren't supported");
            return;
        }
        int failed_here = w->state == WS_FAILED && !strcmp(w->ssid, ap.ssid);
        if (!strcmp(ap.ssid, w->ssid) && (w->state == WS_CONNECTED || w->state == WS_DHCP)) {
            ui_toast("Already connected");
        }
        else if (ap.secured == 0) {
            wifi_connect(ap.ssid, NULL, 0);
        }
        else if (wifi_is_saved(ap.ssid) && !failed_here) {
            wifi_connect_saved(ap.ssid);
        }
        else {
            open_keyboard(ap.ssid, ap.secured);
        }
        return;
    }
    switch (id) {
    case BTN_HEADER_WIFI:
        if (ui.screen == SCR_PLAYER) {
            ui.screen = SCR_WIFI;
            ui.list_off = 0;
            if (w->state != WS_CONNECTING) wifi_scan();
        }
        break;
    case BTN_HEADER_CLOCK: open_tz(); break;
    case BTN_HEADER_BRIGHT: ui.screen = SCR_BRIGHT; break;
    case BTN_BRIGHT_DOWN:
        brightness_set(ui.brightness <= 10 ? BRIGHT_MIN : (ui.brightness - 1) / 10 * 10);
        settings_save();
        break;
    case BTN_BRIGHT_UP:
        brightness_set((ui.brightness / 10 + 1) * 10);
        settings_save();
        break;
    case BTN_TZ_24H:
        ui.clock_24h = !ui.clock_24h;
        settings_save();
        break;
    case BTN_LIST_UP: ui.list_off -= LIST_ROWS; break;
    case BTN_LIST_DOWN: ui.list_off += LIST_ROWS; break;
    case BTN_RESCAN: wifi_scan(); break;
    case BTN_DONE: ui.screen = SCR_PLAYER; break;
    case BTN_FORGET:
        wifi_forget(w->ssid);
        ui_toast("Network forgotten");
        break;
    case BTN_PREV: spotify_command(CMD_PREV); break;
    case BTN_NEXT: spotify_command(CMD_NEXT); break;
    case BTN_PLAY: spotify_toggle_play(); break;
    case BTN_VOLDN:
    case BTN_VOLUP: {
        pthread_mutex_lock(&g_sp.mu);
        int sv = g_sp.supports_volume;
        pthread_mutex_unlock(&g_sp.mu);
        if (sv)
            spotify_volume_step(id == BTN_VOLUP ? +1 : -1);
        else
            ui_toast("This device doesn't allow remote volume");
        break;
    }
    case KEY_SHIFT: {
        uint64_t now = now_ms();
        if (ui.kb_caps) {
            ui.kb_caps = 0;
            ui.kb_shift = 0;
        }
        else if (ui.kb_shift && now - ui.kb_last_shift < 500)
            ui.kb_caps = 1;
        else
            ui.kb_shift = !ui.kb_shift;
        ui.kb_last_shift = now;
        break;
    }
    case KEY_SYM: ui.kb_sym = !ui.kb_sym; break;
    case KEY_BKSP: {
        size_t l = strlen(ui.kb_text);
        if (l) ui.kb_text[l - 1] = 0;
        break;
    }
    case KEY_SPACE: {
        size_t l = strlen(ui.kb_text);
        if (l + 1 < sizeof ui.kb_text) {
            ui.kb_text[l] = ' ';
            ui.kb_text[l + 1] = 0;
        }
        break;
    }
    case KEY_CANCEL:
        memset(ui.kb_text, 0, sizeof ui.kb_text);
        ui.screen = SCR_WIFI;
        break;
    case KEY_OK: kb_submit(); break;
    case KEY_SHOW: ui.kb_hide = !ui.kb_hide; break;
    }
}

/*
 * Routes touch events: a press hits the topmost matching area; sliders follow the finger
 * until it lifts.
 */
static void handle_touch(const touch_event *e)
{
    if (e->type == TOUCH_UP) {
        ui.last_up_ms = now_ms();
        if (ui.drag_id == BTN_BRIGHT_SLIDER) settings_save();
        if (ui.drag_id) ui.dirty = 1;
        ui.drag_id = 0;
        return;
    }
    if (e->type == TOUCH_MOVE) {
        if (ui.drag_id == BTN_BRIGHT_SLIDER && ui.screen == SCR_BRIGHT) slider_touch(e->x);
        return;
    }
    if (e->type != TOUCH_DOWN) return;
    if (now_ms() - ui.last_up_ms < 60) return; /* bounce */
    for (int i = ui.n_hits - 1; i >= 0; i--) {
        const hit *h = &ui.hits[i];
        if (e->x >= h->x && e->x < h->x + h->w && e->y >= h->y && e->y < h->y + h->h) {
            if (h->id == BTN_BRIGHT_SLIDER) {
                /* sliders track the finger instead of firing once */
                ui.drag_id = h->id;
                slider_touch(e->x);
                ui.dirty = 1;
                return;
            }
            ui.pressed_id = h->id;
            ui.pressed_until = now_ms() + 160;
            on_press(h->id);
            ui.dirty = 1;
            return;
        }
    }
}

/* Full-screen startup message. */
static void splash(const char *line)
{
    gfx_clear(C_BG);
    gfx_text_center(&font_xl, SCR_W / 2, 220, "ThermoDeck", C_TEXT);
    gfx_text_center(&font_sm, SCR_W / 2, 252, line, C_SUB);
    gfx_flush();
}

/* Startup: config, display, touch, background threads, Wi-Fi; then the UI loop. */
int main(int argc, char **argv)
{
    if (argc > 1) str_copy(g_dir, sizeof g_dir, argv[1]);
    signal(SIGPIPE, SIG_IGN);
    tdlog("ThermoDeck " TD_VERSION " starting, dir %s", g_dir);

    char cfg[300];
    path_join(cfg, sizeof cfg, "thermodeck.ini");
    if (config_load(cfg) < 0) tdlog("config: %s not found, using defaults", cfg);
    settings_load();

    if (gfx_init("/dev/fb0") < 0) return 1;
    brightness_apply(ui.brightness);
    splash("Starting...");

    touch_opts topt = {
        .swap_xy = config_int("touch", "swap_xy", 0),
        .invert_x = config_int("touch", "invert_x", 0),
        .invert_y = config_int("touch", "invert_y", 0),
        .debug = config_int("touch", "debug", 0),
    };
    int tfd = touch_open(config_get("touch", "device", "/dev/input/event0"), &topt);

    if (pipe(wake_pipe) == 0) {
        fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    }

    ui.web_port = config_int("web", "port", 8080);
    art_start();
    spotify_start();
    web_start(ui.web_port);

    splash("Waiting for Wi-Fi driver...");
    wifi_init();
    ui.screen = SCR_WIFI;
    ui.dirty = 1;
    ui.last_wifi_state = wifi_get()->state;

    for (;;) {
        /* wait for input, a wakeup from other threads, or the next tick */
        struct pollfd pfd[2];
        int nf = 0;
        if (tfd >= 0) pfd[nf++] = (struct pollfd){tfd, POLLIN, 0};
        pfd[nf++] = (struct pollfd){wake_pipe[0], POLLIN, 0};
        int timeout = 250;
        if (ui.pressed_id && ui.pressed_until > now_ms()) {
            int t = (int)(ui.pressed_until - now_ms()) + 5;
            if (t < timeout) timeout = t;
        }
        poll(pfd, (nfds_t)nf, timeout);

        /* input */
        touch_event ev[16];
        if (tfd >= 0) {
            int n = touch_read(ev, ARRAY_LEN(ev));
            for (int i = 0; i < n; i++) handle_touch(&ev[i]);
            if (touch_tick(&ev[0])) handle_touch(&ev[0]);
        }
        char junk[64];
        while (read(wake_pipe[0], junk, sizeof junk) > 0) {}

        /* Wi-Fi */
        wifi_poll();
        const wifi_status *w = wifi_get();
        if (w->version != ui.wifi_ver) {
            ui.wifi_ver = w->version;
            ui.dirty = 1;
            spotify_set_net(w->state == WS_CONNECTED, w->ip, w->ssid, w->signal);
            if (w->state == WS_CONNECTED && ui.last_wifi_state != WS_CONNECTED && ui.screen == SCR_WIFI)
                ui.screen = SCR_PLAYER;
            ui.last_wifi_state = w->state;
        }

        /* Spotify */
        pthread_mutex_lock(&g_sp.mu);
        uint32_t sv = g_sp.version;
        int ticking = g_sp.auth == SP_READY && g_sp.has_item && g_sp.is_playing;
        pthread_mutex_unlock(&g_sp.mu);
        if (sv != ui.sp_ver) {
            ui.sp_ver = sv;
            if (ui.screen == SCR_PLAYER) ui.dirty = 1;
        }
        /* clock tick / progress bar: repaint once a second while playing, once a minute otherwise */
        if (ui.screen == SCR_PLAYER || ui.screen == SCR_TZ) {
            if (ui.screen == SCR_PLAYER && ticking && spotify_progress_now() / 1000 != ui.last_progress_s) ui.dirty = 1;
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_min != ui.last_minute) ui.dirty = 1;
        }
        uint64_t now = now_ms();
        /* clear the pressed highlight and expired toasts */
        if (ui.pressed_id && now >= ui.pressed_until) {
            ui.pressed_id = 0;
            ui.dirty = 1;
        }
        static uint64_t last_toast_check, last_blink;
        if (now - last_toast_check > 500) {
            last_toast_check = now;
            if ((ui.toast_until && now > ui.toast_until) || (g_sp.toast_until && now > g_sp.toast_until)) {
                ui.toast_until = 0;
                pthread_mutex_lock(&g_sp.mu);
                if (g_sp.toast_until && now > g_sp.toast_until) g_sp.toast_until = 0;
                pthread_mutex_unlock(&g_sp.mu);
                ui.dirty = 1;
            }
        }
        /* blinking cursor / connecting indicator */
        if ((ui.screen == SCR_KEYBOARD || w->state == WS_CONNECTING || w->state == WS_DHCP) &&
            now - last_blink >= 500) {
            last_blink = now;
            ui.dirty = 1;
        }
        if (ui.dirty) render();
    }
    return 0;
}
