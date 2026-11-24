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
#include "qrcodegen.h"
#include "wifi/wifi.h"

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

/* ---------- state ---------- */
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

#define BRIGHT_MIN 5 /* lowest backlight percent (0 would look like the screen is off) */

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
} ui_state;

extern ui_state ui;

/* ---------- core.c ---------- */
void ui_render(void);
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
void draw_toast(int y);

/* ---------- settings.c ---------- */
void settings_load(void);
void settings_save(void);
void clock_apply(void);
void brightness_apply(int pct);
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

void render_bright(void);
void press_bright(int id);
void slide_bright(int x); /* finger x on the brightness slider */

#endif
