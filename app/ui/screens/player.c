/* Main screen: Spotify login (QR code + PIN) or the now-playing view with controls. */

#include <time.h>

#include "core/common.h"
#include "qrcodegen.h"
#include "spotify/art.h"
#include "spotify/spotify.h"
#include "ui/internal.h"
#include "web/web.h"
#include "wifi/wifi.h"

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
void render_player(void)
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

/* Transport and volume buttons. */
void press_player(int id)
{
    switch (id) {
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
    }
}
