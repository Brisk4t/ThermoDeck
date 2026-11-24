/*
 * UI core: shared state, hit testing, the repaint and tap dispatch, and the per-loop
 * update that decides when to repaint. Screens live in screens/, drawing helpers in widgets.c.
 */

#include "ui/ui.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "core/common.h"
#include "gfx/gfx.h"
#include "hass/hass.h"
#include "spotify/spotify.h"
#include "ui/internal.h"
#include "wifi/wifi.h"

ui_state ui;

/* other threads write a byte here to make the main loop repaint */
static int wake_pipe[2] = {-1, -1};

void ui_wake(void)
{
    char c = 1;
    if (wake_pipe[1] >= 0) {
        ssize_t r = write(wake_pipe[1], &c, 1);
        (void)r;
    }
}

int ui_wake_fd(void) { return wake_pipe[0]; }

/* Short message shown at the bottom of the current screen for a few seconds. */
void ui_toast(const char *msg)
{
    str_copy(ui.toast, sizeof ui.toast, msg);
    ui.toast_until = now_ms() + 3500;
    ui.dirty = 1;
}

/* Registers a tappable area for the frame being drawn. */
void add_hit(int id, int x, int y, int w, int h)
{
    if (ui.n_hits < (int)ARRAY_LEN(ui.hits)) ui.hits[ui.n_hits++] = (hit){id, x, y, w, h};
}

/* Whether id was just tapped (draws the pressed highlight briefly). */
int is_pressed(int id) { return ui.pressed_id == id && now_ms() < ui.pressed_until; }

/* Draws the current screen into the back buffer (and records its hits). */
void ui_draw(void)
{
    ui.n_hits = 0;
    gfx_clear(C_BG);
    switch (ui.screen) {
    case SCR_WIFI: render_wifi(); break;
    case SCR_KEYBOARD: render_keyboard(); break;
    case SCR_TZ: render_tz(); break;
    case SCR_BRIGHT: render_bright(); break;
    case SCR_HASS: render_hass(); break;
    default: render_player(); break;
    }
    if (is_home()) draw_page_dots();
}

/* Repaints the current screen and pushes it to the display. */
void ui_render(void)
{
    ui_draw();
    gfx_flush();
    ui.dirty = 0;
}

/* Acts on a tap: buttons shared by several screens here, the rest by the current screen. */
void ui_press(int id)
{
    switch (id) {
    case BTN_HEADER_WIFI:
        if (is_home()) {
            ui.screen = SCR_WIFI;
            ui.list_off = 0;
            if (wifi_get()->state != WS_CONNECTING) wifi_scan();
        }
        return;
    case BTN_HEADER_CLOCK: open_tz(); return;
    case BTN_HEADER_BRIGHT: ui.screen = SCR_BRIGHT; return;
    case BTN_LIST_UP: ui.list_off -= LIST_ROWS; return;
    case BTN_LIST_DOWN: ui.list_off += LIST_ROWS; return;
    case BTN_DONE: ui.screen = ui.home; return;
    }
    switch (ui.screen) {
    case SCR_WIFI: press_wifi(id); break;
    case SCR_KEYBOARD: press_keyboard(id); break;
    case SCR_TZ: press_tz(id); break;
    case SCR_BRIGHT: press_bright(id); break;
    case SCR_HASS: press_hass(id); break;
    default: press_player(id); break;
    }
}

int hit_at(int x, int y)
{
    for (int i = ui.n_hits - 1; i >= 0; i--) {
        const hit *h = &ui.hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return h->id;
    }
    return 0;
}

/*
 * Routes touch events. On the home screens the pager decides between taps and swipes;
 * elsewhere a press hits the topmost matching area right away, and sliders follow the
 * finger until it lifts.
 */
void ui_handle_touch(const touch_event *e)
{
    if (is_home() || pager_busy()) {
        pager_touch(e);
        return;
    }
    if (e->type == TOUCH_UP) {
        ui.last_up_ms = now_ms();
        if (ui.drag_id == BTN_BRIGHT_SLIDER) settings_save();
        if (ui.drag_id) ui.dirty = 1;
        ui.drag_id = 0;
        return;
    }
    if (e->type == TOUCH_MOVE) {
        if (ui.drag_id == BTN_BRIGHT_SLIDER && ui.screen == SCR_BRIGHT) slide_bright(e->x);
        return;
    }
    if (e->type != TOUCH_DOWN) return;
    if (now_ms() - ui.last_up_ms < 60) return; /* bounce */
    int id = hit_at(e->x, e->y);
    if (!id) return;
    if (id == BTN_BRIGHT_SLIDER) {
        /* sliders track the finger instead of firing once */
        ui.drag_id = id;
        slide_bright(e->x);
        ui.dirty = 1;
        return;
    }
    ui.pressed_id = id;
    ui.pressed_until = now_ms() + 160;
    ui_press(id);
    ui.dirty = 1;
}

void ui_init(int web_port)
{
    settings_load();
    brightness_apply(ui.brightness);
    ui.web_port = web_port;
    if (pipe(wake_pipe) == 0) {
        fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    }
}

void ui_splash(const char *line)
{
    gfx_clear(C_BG);
    gfx_text_center(&font_xl, SCR_W / 2, 220, "ThermoDeck", C_TEXT);
    gfx_text_center(&font_sm, SCR_W / 2, 252, line, C_SUB);
    gfx_flush();
}

void ui_show_wifi(void)
{
    ui.home = SCR_PLAYER;
    ui.screen = SCR_WIFI;
    ui.dirty = 1;
    ui.last_wifi_state = wifi_get()->state;
}

/* 250 ms normally; sooner when a pressed highlight is about to expire. */
int ui_poll_timeout(void)
{
    int timeout = 250;
    if (ui.pressed_id && ui.pressed_until > now_ms()) {
        int t = (int)(ui.pressed_until - now_ms()) + 5;
        if (t < timeout) timeout = t;
    }
    return timeout;
}

/* Repaint, and leave the Wi-Fi screen once a connection comes up. */
void ui_wifi_changed(const wifi_status *w)
{
    ui.dirty = 1;
    if (w->state == WS_CONNECTED && ui.last_wifi_state != WS_CONNECTED && ui.screen == SCR_WIFI) ui.screen = ui.home;
    ui.last_wifi_state = w->state;
}

void ui_update(void)
{
    char junk[64];
    while (read(wake_pipe[0], junk, sizeof junk) > 0) {}

    /* Spotify */
    pthread_mutex_lock(&g_sp.mu);
    uint32_t sv = g_sp.version;
    int ticking = g_sp.auth == SP_READY && g_sp.has_item && g_sp.is_playing;
    pthread_mutex_unlock(&g_sp.mu);
    if (sv != ui.sp_ver) {
        ui.sp_ver = sv;
        if (ui.screen == SCR_PLAYER) ui.dirty = 1;
    }
    /* Home Assistant */
    pthread_mutex_lock(&g_ha.mu);
    uint32_t hv = g_ha.version;
    pthread_mutex_unlock(&g_ha.mu);
    if (hv != ui.ha_ver) {
        ui.ha_ver = hv;
        if (ui.screen == SCR_HASS) ui.dirty = 1;
    }
    /* clock tick / progress bar: repaint once a second while playing, once a minute otherwise */
    if (is_home() || ui.screen == SCR_TZ) {
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
        pthread_mutex_lock(&g_ha.mu);
        if (g_ha.toast_until && now > g_ha.toast_until) {
            g_ha.toast_until = 0;
            ui.dirty = 1;
        }
        pthread_mutex_unlock(&g_ha.mu);
    }
    /* blinking cursor / connecting indicator */
    const wifi_status *w = wifi_get();
    if ((ui.screen == SCR_KEYBOARD || w->state == WS_CONNECTING || w->state == WS_DHCP) && now - last_blink >= 500) {
        last_blink = now;
        ui.dirty = 1;
    }
    if (pager_busy())
        pager_update(); /* mid-swipe: the pager owns the screen until the finger lifts */
    else if (ui.dirty)
        ui_render();
}
