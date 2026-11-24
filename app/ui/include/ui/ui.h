#ifndef UI_UI_H
#define UI_UI_H

/*
 * The touchscreen UI, run from the main thread. Other threads never draw: they update
 * shared state and call ui_wake(); the main loop then calls ui_update() to repaint.
 */

#include <stdint.h>

#include "input/touch.h"
#include "wifi/wifi.h"

/* Loads settings and applies the backlight. Call after gfx_init(). */
void ui_init(int web_port);
/* Full-screen startup message. */
void ui_splash(const char *line);
/* Switches to the Wi-Fi screen (first screen after startup). */
void ui_show_wifi(void);

/* Wakes the UI thread (safe to call from any thread). */
void ui_wake(void);
/* Readable when ui_wake() was called; poll it in the main loop. */
int ui_wake_fd(void);
/* Longest the main loop may sleep before ui_update() is due (ms). */
int ui_poll_timeout(void);

/* The screen may be showing a secret (Spotify login PIN, a Wi-Fi password being typed),
 * so it must not be shared (web preview). Safe to call from any thread. */
int ui_screen_private(void);

/* Renders Home Assistant page `page` (0-based) into out (SCR_W * SCR_H, 0xRRGGBB) without
 * showing it, for the web preview. Call from another thread: the UI thread does the
 * drawing. Returns the number of HA pages, or -1 if the UI didn't answer in time. */
int ui_render_hass_page(int page, uint32_t *out, int timeout_ms);

void ui_handle_touch(const touch_event *e);
/* Wi-Fi status changed (w->version moved). */
void ui_wifi_changed(const wifi_status *w);
/* Picks up wakeups and timers, repaints if anything changed. */
void ui_update(void);

#endif
