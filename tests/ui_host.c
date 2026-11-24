/* Host UI snapshots: renders the real src/main.c screens with faked Wi-Fi / Spotify
 * state and writes each one as a PPM, so layout changes can be checked without the device.
 *
 *   make host && gcc -O1 -g -D_GNU_SOURCE -pthread -Isrc -Ithird_party/qrcodegen -o build-host/ui_host \
 *       tests/ui_host.c src/gfx.c src/util.c src/config.c gen/font_data.c third_party/qrcodegen/qrcodegen.c -lm
 *   ./build-host/ui_host <out dir>        -> <out dir>/<scenario>.ppm  (view with tools/ppm2png.py)
 */
#define main thermodeck_main
#include "../src/main.c"
#undef main

/* ---- fakes for everything main.c talks to ---- */
static wifi_status fake_wifi;
const wifi_status *wifi_get(void) { return &fake_wifi; }
int wifi_init(void) { return 0; }
void wifi_poll(void) {}
void wifi_scan(void) {}
void wifi_connect(const char *ssid, const char *psk, int secured) {}
void wifi_forget(const char *ssid) {}
int wifi_is_saved(const char *ssid) { return 0; }
int wifi_connect_saved(const char *ssid) { return -1; }

sp_shared g_sp = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
void spotify_start(void) {}
void spotify_command(int cmd) {}
void spotify_volume_step(int d) {}
void spotify_toggle_play(void) {}
void spotify_set_net(int up, const char *ip, const char *ssid, int signal) {}
int spotify_progress_now(void) { return g_sp.progress_ms; }

static uint32_t fake_art[ART_PX * ART_PX];
static int have_art;
void art_start(void) {}
int art_draw(int x, int y, int r)
{
    if (have_art) gfx_blit(x, y, ART_PX, ART_PX, fake_art, r);
    return have_art;
}

void web_start(int port) {}
void web_get_pin(char out[7]) { strcpy(out, "123456"); }
int touch_open(const char *dev, const touch_opts *o) { return -1; }
int touch_read(touch_event *ev, int max) { return 0; }
int touch_tick(touch_event *ev) { return 0; }

/* ---- scenarios ---- */
static const char *out_dir;

static void snap(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name);
    render();
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
            fake_art[y * ART_PX + x] = ((uint32_t)(40 + x) << 16) | ((uint32_t)(60 + y / 2) << 8) | (uint32_t)(120 + (x + y) / 3);
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

    wifi_set(WS_CONNECTED, "Poverty", -50, 0);              snap("hdr_connected_strong");
    wifi_set(WS_CONNECTED, "Poverty", -78, 0);              snap("hdr_connected_weak");
    wifi_set(WS_CONNECTED, "A Very Long Network Name 5G", -60, 0); snap("hdr_long_ssid");
    g_sp.last_poll_ok = 0;
    wifi_set(WS_CONNECTED, "Poverty", -50, 0);              snap("hdr_no_internet");
    g_sp.last_poll_ok = 1;
    wifi_set(WS_CONNECTING, "Poverty", 0, 0);               snap("hdr_connecting");
    wifi_set(WS_FAILED, "Poverty", 0, 1);                   snap("hdr_wrong_password");
    wifi_set(WS_FAILED, "Poverty", 0, 0);                   snap("hdr_offline");
    wifi_set(WS_IDLE, "", 0, 0);                            snap("hdr_idle");
    wifi_set(WS_CONNECTED, "Poverty", -50, 0);
    ui.screen = SCR_WIFI;                                   snap("wifi_screen");
    str_copy(ui.tz, sizeof ui.tz, "MST7MDT,M3.2.0,M11.1.0");
    clock_apply();
    ui.screen = SCR_PLAYER;                                 snap("player_mountain");
    open_tz();                                              snap("tz_screen");
    ui.clock_24h = 1; ui.list_off = 20;                     snap("tz_screen_24h_end");
    ui.clock_24h = 0;
    ui.screen = SCR_PLAYER;                                 snap("player_sun");
    ui.brightness = 65; ui.screen = SCR_BRIGHT;             snap("bright_65");
    ui.brightness = 5; ui.drag_id = BTN_BRIGHT_SLIDER;      snap("bright_min_drag");
    ui.drag_id = 0;
    return 0;
}
