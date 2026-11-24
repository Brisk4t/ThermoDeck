#ifndef UI_UI_H
#define UI_UI_H

/*
 * The touchscreen UI, run from the main thread. Other threads never draw: they update
 * shared state and call ui_wake(); the main loop then calls ui_update() to repaint.
 */

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

void ui_handle_touch(const touch_event *e);
/* Wi-Fi status changed (w->version moved). */
void ui_wifi_changed(const wifi_status *w);
/* Picks up wakeups and timers, repaints if anything changed. */
void ui_update(void);

#endif
