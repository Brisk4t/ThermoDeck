/* ThermoDeck: Wi-Fi + Spotify remote dashboard for the ecobee (i.MX28) touchscreen.
 *
 * This file only wires the modules together. Threads: UI/main (this loop: framebuffer,
 * touch, wpa_supplicant), network worker (spotify/spotify.c: NTP, OAuth, polling, commands),
 * album art (spotify/art.c), Home Assistant (hass/hass.c: WebSocket client), the web
 * server (web/web.c: login from another device, status page) and the backplate sensor
 * reader (backplate/sensors.c: proximity).
 */
#include <poll.h>
#include <signal.h>

#include "backplate/backplate.h"
#include "core/common.h"
#include "core/config.h"
#include "gfx/gfx.h"
#include "hass/hass.h"
#include "input/touch.h"
#include "spotify/art.h"
#include "spotify/spotify.h"
#include "ui/ui.h"
#include "web/ota.h"
#include "web/web.h"
#include "wifi/wifi.h"

/* Startup: config, display, touch, background threads, Wi-Fi; then the UI loop. */
int main(int argc, char **argv)
{
    if (argc > 1) str_copy(g_dir, sizeof g_dir, argv[1]);
    signal(SIGPIPE, SIG_IGN);
    tdlog("ThermoDeck " TD_VERSION " starting, dir %s", g_dir);

    char cfg[300];
    path_join(cfg, sizeof cfg, "thermodeck.ini");
    if (config_load(cfg) < 0) tdlog("config: %s not found, using defaults", cfg);

    /* ThermoDeck never runs the HVAC: every relay off before anything else, then only
     * listen to the backplate (the proximity sensor wakes the dimmed screen) */
    backplate_start(config_get("backplate", "device", BACKPLATE_DEV));

    if (gfx_init("/dev/fb0") < 0) return 1;
    int web_port = config_int("web", "port", 8080);
    ui_init(web_port);
    ui_splash("Starting...");

    touch_opts topt = {
        .swap_xy = config_int("touch", "swap_xy", 0),
        .invert_x = config_int("touch", "invert_x", 0),
        .invert_y = config_int("touch", "invert_y", 0),
        .debug = config_int("touch", "debug", 0),
    };
    int tfd = touch_open(config_get("touch", "device", "/dev/input/event0"), &topt);

    art_start();
    spotify_start();
    hass_start();
    web_start(web_port);

    ui_splash("Waiting for Wi-Fi driver...");
    wifi_init();
    ui_show_wifi();
    uint32_t wifi_ver = 0; /* last Wi-Fi status version passed on */

    for (;;) {
        /* wait for input, a wakeup from other threads, or the next tick */
        struct pollfd pfd[2];
        int nf = 0;
        if (tfd >= 0) pfd[nf++] = (struct pollfd){tfd, POLLIN, 0};
        pfd[nf++] = (struct pollfd){ui_wake_fd(), POLLIN, 0};
        poll(pfd, (nfds_t)nf, ui_poll_timeout());

        /* input */
        touch_event ev[16];
        if (tfd >= 0) {
            int n = touch_read(ev, ARRAY_LEN(ev));
            for (int i = 0; i < n; i++) ui_handle_touch(&ev[i]);
            if (touch_tick(&ev[0])) ui_handle_touch(&ev[0]);
        }

        ota_tick(); /* keeps a freshly installed update once it has run for a while */

        /* Wi-Fi: tell the network workers whether the network is up, and the UI to repaint */
        wifi_poll();
        const wifi_status *w = wifi_get();
        if (w->version != wifi_ver) {
            wifi_ver = w->version;
            spotify_set_net(w->state == WS_CONNECTED, w->ip, w->ssid, w->signal);
            hass_set_net(w->state == WS_CONNECTED);
            ui_wifi_changed(w);
        }

        ui_update();
    }
    return 0;
}
