#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

/*
 * Shared between the UI's own files (core, widgets, settings, screens/); not for other modules.
 *
 * The UI is immediate-mode: every repaint redraws the current screen from scratch into
 * the back buffer and records the tappable areas ("hits") as it goes; a touch is matched
 * against the hits from the last repaint.
 */

#include <stdint.h>
#include <time.h>

#include "gfx/gfx.h"
#include "input/touch.h"
#include "qrcodegen.h"
#include "ui/theme.h"
#include "wifi/wifi.h"

/* ---------- state ---------- */
/* screens */
enum { SCR_WIFI, SCR_KEYBOARD, SCR_PLAYER, SCR_TZ, SCR_BRIGHT, SCR_HASS };

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
    BTN_HA0 = 2000,  /* + Home Assistant dashboard item index */
};

/* scrolling list layout shared by the Wi-Fi and time zone screens */
#define LIST_Y     120
#define LIST_ROW_H 56
#define LIST_ROWS  5

#define HEADER_H   45 /* top bar height; the pager slides only what's below it */
#define BRIGHT_MIN 5  /* lowest backlight percent (0 would look like the screen is off) */

/* a tappable rectangle recorded during the last repaint */
typedef struct {
    int id, x, y, w, h;
} hit;

typedef struct {
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
    uint32_t sp_ver;
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

    /* screen timeout: dim after dim_after_s without a touch or anyone near the screen */
    int dim_after_s;         /* 0 = never dim */
    int dim_pct;             /* backlight percent while dimmed */
    int dimmed;              /* the backlight is at dim_pct */
    int swallow_touch;       /* the touch that woke the screen: ignore it until it lifts */
    uint64_t last_active_ms; /* last touch or proximity, now_ms() */

    /* home screens: the Spotify page, then the Home Assistant pages below it */
    int home;    /* SCR_PLAYER or SCR_HASS: where Done returns to */
    int ha_page; /* which HA page is showing */
    uint32_t ha_ver;
} ui_state;

extern ui_state ui;

/* ---------- core.c ---------- */
void ui_draw(void); /* repaint into the back buffer only */
void ui_render(void);
void ui_press(int id);
int hit_at(int x, int y); /* id of the topmost hit at (x, y), or 0 */
void ui_toast(const char *msg);
void add_hit(int id, int x, int y, int w, int h);
int is_pressed(int id);

/* ---------- widgets.c ---------- */
enum { BS_NORMAL, BS_PRIMARY, BS_DISABLED };
void button(int id, int x, int y, int w, int h, const char *label, const font_t *f, int style);
int signal_bars(int dbm);
void draw_bars(int x, int y_bottom, int level, uint32_t on, uint32_t off);
void draw_line(float x0, float y0, float x1, float y1, float w, uint32_t c);
void draw_sun(int cx, int cy, int r, uint32_t c, uint32_t bg);
void draw_header(const char *title);
void draw_home_header(void);
void draw_toast(int y);
void draw_page_dots(void);

/* ---------- settings.c ---------- */
void settings_load(void);
void settings_save(void);
void clock_apply(void);
void brightness_apply(int pct);
void screen_activity(void); /* touch or someone near: undim and restart the timeout */
void screen_timeout_tick(void);
void fmt_clock(time_t t, char *out, size_t outlen);
void fmt_clock_in(const char *tz, time_t t, char *out, size_t outlen);

/* ---------- screens/ ---------- */
/* Each screen draws itself and handles taps on its own buttons; the shared header,
 * list scrolling and Done buttons are handled in core.c. */
void render_wifi(void);
void press_wifi(int id);

void open_keyboard(const char *ssid, int secured);
void render_keyboard(void);
void press_keyboard(int id);

void render_player(void);
void press_player(int id);

void open_tz(void);
void render_tz(void);
void press_tz(int id);

void render_hass(void);
void press_hass(int id);
int hass_page_count(void);

void render_bright(void);
void press_bright(int id);
void slide_bright(int x); /* finger x on the brightness slider */

/* ---------- pager.c: swiping between the home screens ---------- */
int is_home(void);     /* showing the Spotify or an HA page */
int pager_count(void); /* pages: Spotify + HA pages */
int pager_current(void);
void pager_show(int page);              /* switch without animation */
void pager_touch(const touch_event *e); /* touch handling on the home screens */
int pager_busy(void);                   /* a swipe is in progress: don't repaint normally */
void pager_update(void);                /* draws the latest drag position */

#endif
