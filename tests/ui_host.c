/* Host UI snapshots: renders the real UI module (app/ui) with faked Wi-Fi / Spotify state
 * and writes each screen as a PPM, so layout changes can be checked without the device.
 *
 *   make tests && ./build-host/tests/ui_host <out dir>   -> <out dir>/<scenario>.ppm
 *   (view with tools/ppm2png.py)
 *
 * Also checks the pager's swipe decisions (see tests/touch_host.c for the touch driver).
 */
#include <pthread.h>

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "cards/cards.h"
#include "core/common.h"
#include "hass/hass.h"
#include "json/json.h"
#include "spotify/art.h"
#include "spotify/spotify.h"
#include "ui/ui.h"
#include "ui/internal.h"
#include "web/ota.h"
#include "web/web.h"
#include "wifi/wifi.h"

/* ---- fakes for everything the UI talks to ---- */
static wifi_status fake_wifi;
const wifi_status *wifi_get(void) { return &fake_wifi; }
void wifi_scan(void) {}
void wifi_connect(const char *ssid, const char *psk, int secured) {}
void wifi_forget(const char *ssid) {}
int wifi_is_saved(const char *ssid) { return 0; }
int wifi_connect_saved(const char *ssid) { return -1; }

sp_shared g_sp = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
void spotify_command(int cmd) {}
void spotify_volume_step(int d) {}
void spotify_toggle_play(void) {}
int spotify_progress_now(void) { return g_sp.progress_ms; }

static uint32_t fake_art[ART_PX * ART_PX];
static int have_art;
int art_draw(int x, int y, int r)
{
    if (have_art) gfx_blit(x, y, ART_PX, ART_PX, fake_art, r);
    return have_art;
}

void web_get_pin(char out[7]) { strcpy(out, "123456"); }

static ota_view fake_ota; /* the software update screen, set per scenario */
void ota_get_view(ota_view *v) { *v = fake_ota; }
void ota_cancel(void) { fake_ota.phase = OTA_IDLE; }

/* Loads tests/data/ha_dashboard.json + ha_states.json into g_ha (no server involved). */
static void load_hass_fixture(void)
{
    size_t len;
    char *js = file_read("tests/data/ha_dashboard.json", &len);
    char err[160];
    if (!js || ha_dashboard_parse(js, len, &g_ha.d, err, sizeof err) < 0) {
        printf("dashboard fixture: %s\n", js ? err : "missing");
        exit(1);
    }
    free(js);
    js = file_read("tests/data/ha_states.json", &len);
    static jtok t[4096];
    json_doc d;
    json_parse(&d, js, len, t, 4096);
    for (int i = 0; i < g_ha.d.n_ents; i++) {
        ha_entity *e = &g_ha.d.ents[i];
        int st = json_get(&d, 0, e->id);
        if (st < 0) continue;
        int a = json_get(&d, st, "a");
        json_str(&d, json_get(&d, st, "s"), e->state, sizeof e->state);
        json_str(&d, json_get(&d, a, "friendly_name"), e->name, sizeof e->name);
        json_str(&d, json_get(&d, a, "unit_of_measurement"), e->unit, sizeof e->unit);
        json_str(&d, json_get(&d, a, "device_class"), e->device_class, sizeof e->device_class);
        json_str(&d, json_get(&d, a, "media_title"), e->extra, sizeof e->extra);
        e->brightness = (int)json_int(&d, json_get(&d, a, "brightness"), -1);
        int ct = json_get(&d, a, "current_temperature");
        if (ct >= 0)
            snprintf(e->extra, sizeof e->extra, "%.*s\xC2\xB0", d.t[ct].end - d.t[ct].start, js + d.t[ct].start);
        /* attributes the cards asked to track (card_want_attr) */
        for (int k = 0; k < e->n_attrs; k++) {
            int v = json_get(&d, a, e->attrs[k].key);
            if (v >= 0 && json_str(&d, v, e->attrs[k].val, sizeof e->attrs[k].val) != 0)
                snprintf(e->attrs[k].val, sizeof e->attrs[k].val, "%.*s", d.t[v].end - d.t[v].start, js + d.t[v].start);
        }
        e->has_state = 1;
    }
    free(js);

    /* forecasts the cards subscribed to (tests/data/ha_forecasts.json) */
    js = file_read("tests/data/ha_forecasts.json", &len);
    json_parse(&d, js, len, t, 4096);
    for (int i = 0; i < g_ha.d.n_fcs; i++) {
        ha_forecast *f = &g_ha.d.fcs[i];
        int arr = json_get(&d, json_get(&d, 0, g_ha.d.ents[f->ent].id), f->type);
        for (f->n = 0; f->n < HA_FORECAST_LEN && json_at(&d, arr, f->n) >= 0; f->n++) {
            int fe = json_at(&d, arr, f->n);
            ha_forecast_entry *x = &f->e[f->n];
            char when[40];
            struct tm tm = {0};
            json_str(&d, json_get(&d, fe, "datetime"), when, sizeof when);
            sscanf(when, "%d-%d-%dT%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min);
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            x->time = timegm(&tm);
            json_str(&d, json_get(&d, fe, "condition"), x->condition, sizeof x->condition);
            int tv = json_get(&d, fe, "temperature"), tl = json_get(&d, fe, "templow");
            snprintf(x->temp, sizeof x->temp, "%.*s", tv >= 0 ? d.t[tv].end - d.t[tv].start : 0,
                     js + (tv >= 0 ? d.t[tv].start : 0));
            snprintf(x->templow, sizeof x->templow, "%.*s", tl >= 0 ? d.t[tl].end - d.t[tl].start : 0,
                     js + (tl >= 0 ? d.t[tl].start : 0));
        }
    }
    free(js);
    g_ha.status = HA_READY;
    g_ha.layout_ver++;
}

/* ---- pager: swipes as (ms, y) points, the finger lifting after the last one ---- */
static void swipe(const int (*pts)[2], int n)
{
    uint32_t base = 1000000;
    ui_handle_touch(&(touch_event){TOUCH_DOWN, 160, pts[0][1], base + (uint32_t)pts[0][0]});
    for (int i = 1; i < n; i++) ui_handle_touch(&(touch_event){TOUCH_MOVE, 160, pts[i][1], base + (uint32_t)pts[i][0]});
    ui_handle_touch(&(touch_event){TOUCH_UP, 160, pts[n - 1][1], base + (uint32_t)pts[n - 1][0] + 8});
}

static void pager_tests(void)
{
    ui.dimmed = 0;
    pager_show(1);
    /* a quick 50 px flick up: next page */
    swipe((const int[][2]){{0, 300}, {15, 290}, {30, 275}, {45, 262}, {60, 250}}, 5);
    assert(pager_current() == 2);
    /* the same distance dragged slowly, then held: stays */
    swipe((const int[][2]){{0, 300}, {300, 290}, {600, 275}, {900, 262}, {1200, 250}, {1500, 250}}, 6);
    assert(pager_current() == 2);
    /* a long slow drag down: previous page */
    swipe((const int[][2]){{0, 100}, {200, 140}, {400, 180}, {600, 220}, {900, 230}}, 5);
    assert(pager_current() == 1);
    /* dragged far, then flung back: stays */
    swipe((const int[][2]){{0, 300}, {100, 230}, {200, 160}, {300, 150}, {320, 170}, {340, 200}}, 6);
    assert(pager_current() == 1);
    /* a flick that the UI loop only gets to much later (busy rendering): still a flick */
    uint32_t base = 2000000;
    ui_handle_touch(&(touch_event){TOUCH_DOWN, 160, 300, base});
    ui_handle_touch(&(touch_event){TOUCH_MOVE, 160, 280, base + 15});
    usleep(400 * 1000);
    ui_handle_touch(&(touch_event){TOUCH_MOVE, 160, 262, base + 30});
    ui_handle_touch(&(touch_event){TOUCH_MOVE, 160, 250, base + 45});
    ui_handle_touch(&(touch_event){TOUCH_UP, 160, 250, base + 53});
    assert(pager_current() == 2);
    pager_show(1);
    /* a flick down from the first page: nothing above, stays */
    pager_show(0);
    swipe((const int[][2]){{0, 200}, {15, 215}, {30, 235}, {45, 260}}, 4);
    assert(pager_current() == 0);
    /* a tap still taps (no swipe) */
    swipe((const int[][2]){{0, 300}, {40, 303}}, 2);
    assert(pager_current() == 0 && !pager_busy());
    puts("pager OK");
}

/* ---- scenarios ---- */
static const char *out_dir;

static void snap(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name);
    ui_render();
    gfx_dump_ppm(path);
    printf("%s\n", path);
}

static void wifi_set(wifi_state s, const char *ssid, int signal, int auth_failed)
{
    memset(&fake_wifi, 0, sizeof fake_wifi);
    fake_wifi.state = s;
    str_copy(fake_wifi.ssid, sizeof fake_wifi.ssid, ssid);
    fake_wifi.signal = signal;
    fake_wifi.auth_failed = auth_failed;
    if (s == WS_CONNECTED) str_copy(fake_wifi.ip, sizeof fake_wifi.ip, "192.168.1.238");
}

int main(int argc, char **argv)
{
    out_dir = argc > 1 ? argv[1] : ".";
    for (int y = 0; y < ART_PX; y++)
        for (int x = 0; x < ART_PX; x++)
            fake_art[y * ART_PX + x] =
                ((uint32_t)(40 + x) << 16) | ((uint32_t)(60 + y / 2) << 8) | (uint32_t)(120 + (x + y) / 3);
    have_art = 1;

    /* a playing track */
    g_sp.auth = SP_READY;
    g_sp.time_synced = 1;
    g_sp.last_poll_ok = 1;
    g_sp.has_item = 1;
    g_sp.is_playing = 1;
    g_sp.supports_volume = 1;
    g_sp.volume = 65;
    g_sp.duration_ms = 207000;
    g_sp.progress_ms = 83000;
    strcpy(g_sp.title, "WILDFIRE");
    strcpy(g_sp.artist, "Aries");
    strcpy(g_sp.album, "GLASS JAW");
    strcpy(g_sp.device, "BRISK4T");
    strcpy(g_sp.ip, "192.168.1.238");
    ui.screen = SCR_PLAYER;
    ui.web_port = 8080;

    wifi_set(WS_CONNECTED, "Poverty", -50, 0);
    snap("hdr_connected_strong");
    wifi_set(WS_CONNECTED, "Poverty", -78, 0);
    snap("hdr_connected_weak");
    wifi_set(WS_CONNECTED, "A Very Long Network Name 5G", -60, 0);
    snap("hdr_long_ssid");
    g_sp.last_poll_ok = 0;
    wifi_set(WS_CONNECTED, "Poverty", -50, 0);
    snap("hdr_no_internet");
    g_sp.last_poll_ok = 1;
    wifi_set(WS_CONNECTING, "Poverty", 0, 0);
    snap("hdr_connecting");
    wifi_set(WS_FAILED, "Poverty", 0, 1);
    snap("hdr_wrong_password");
    wifi_set(WS_FAILED, "Poverty", 0, 0);
    snap("hdr_offline");
    wifi_set(WS_IDLE, "", 0, 0);
    snap("hdr_idle");
    wifi_set(WS_CONNECTED, "Poverty", -50, 0);
    ui.screen = SCR_WIFI;
    snap("wifi_screen");
    str_copy(ui.tz, sizeof ui.tz, "MST7MDT,M3.2.0,M11.1.0");
    clock_apply();
    ui.screen = SCR_PLAYER;
    snap("player_mountain");
    open_tz();
    snap("tz_screen");
    ui.clock_24h = 1;
    ui.list_off = 20;
    snap("tz_screen_24h_end");
    ui.clock_24h = 0;
    ui.screen = SCR_PLAYER;
    snap("player_sun");
    ui.brightness = 65;
    ui.screen = SCR_BRIGHT;
    snap("bright_65");
    ui.brightness = 5;
    ui.drag_id = BTN_BRIGHT_SLIDER;
    snap("bright_min_drag");
    ui.drag_id = 0;

    /* software update: PIN screen over whatever is showing, then installing */
    ui.screen = SCR_PLAYER;
    fake_ota = (ota_view){OTA_WAITING_PIN, "482913", "192.168.1.245", 107};
    snap("ota_pin");
    fake_ota.phase = OTA_INSTALLING;
    snap("ota_installing");
    fake_ota.phase = OTA_IDLE;

    /* Home Assistant: status pages, then the dashboard fixture page by page */
    ui.brightness = 80;
    g_ha.status = HA_OFF;
    str_copy(g_ha.msg, sizeof g_ha.msg, "Add url and token under [homeassistant] in /config/thermodeck/thermodeck.ini");
    pager_show(1);
    snap("ha_off");
    g_ha.status = HA_NO_DASHBOARD;
    str_copy(g_ha.msg, sizeof g_ha.msg, "No dashboard dashboard-thermodeck in Home Assistant");
    snap("ha_no_dashboard");
    load_hass_fixture();
    pager_show(0);
    snap("pager_player");
    for (int p = 1; p < pager_count(); p++) {
        char name[32];
        snprintf(name, sizeof name, "ha_page%d", p);
        pager_show(p);
        snap(name);
    }
    /* pressed tile + a toast */
    pager_show(1);
    ui.pressed_id = BTN_HA0 + 2;
    ui.pressed_until = now_ms() + 100000;
    str_copy(g_ha.toast, sizeof g_ha.toast, "Service light.explode not found.");
    g_ha.toast_until = now_ms() + 100000;
    snap("ha_pressed_toast");
    ui.pressed_id = 0;
    g_ha.toast_until = 0;

    pager_tests();
    return 0;
}
