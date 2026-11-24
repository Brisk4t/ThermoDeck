/* Wi-Fi screen: connection status and the list of networks to join. */

#include "core/common.h"
#include "ui/internal.h"
#include "wifi/wifi.h"

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

/* Wi-Fi screen: status card, scrollable list of networks, scroll / rescan / done buttons. */
void render_wifi(void)
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

/* A network row (join it, or ask for its password), Rescan and Forget. */
void press_wifi(int id)
{
    const wifi_status *w = wifi_get();
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
    case BTN_RESCAN: wifi_scan(); break;
    case BTN_FORGET:
        wifi_forget(w->ssid);
        ui_toast("Network forgotten");
        break;
    }
}
