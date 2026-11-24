/* On-screen keyboard for entering a Wi-Fi password. */

#include "core/common.h"
#include "ui/internal.h"
#include "wifi/wifi.h"

static const char *KB_ROWS[2][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",./?`", "~<>"},
};
#define KB_Y     216
#define KB_KEY_H 52

/* One keyboard key. */
static void key(int id, int x, int y, int w, const char *label, const font_t *f, uint32_t bg)
{
    int pressed = is_pressed(id);
    gfx_rrect(x + 2, y + 3, w - 4, KB_KEY_H - 6, 7, pressed ? C_BTN_PRESSED : bg);
    int ty = y + (KB_KEY_H + f->ascent - f->descent) / 2;
    gfx_text_center(f, x + w / 2, ty, label, C_TEXT);
    add_hit(id, x, y, w, KB_KEY_H);
}

/*
 * On-screen keyboard for the Wi-Fi password: text field (optionally masked), letters /
 * symbols, shift (double tap = caps lock), delete, space, cancel and join.
 */
void render_keyboard(void)
{
    draw_header("Wi-Fi password");
    gfx_text(&font_sm, 14, 66, "Network", C_SUB);
    gfx_text_fit(&font_mdb, 14, 90, SCR_W - 28, ui.kb_ssid, C_TEXT);

    /* text field */
    gfx_rrect(10, 102, SCR_W - 20, 50, 10, C_GREEN);
    gfx_rrect(12, 104, SCR_W - 24, 46, 9, 0x232323);
    char shown[200];
    size_t len = strlen(ui.kb_text);
    if (ui.kb_hide) {
        size_t o = 0;
        for (size_t i = 0; i < len && o + 3 < sizeof shown; i++) {
            shown[o++] = (char)0xC2; /* U+00B7 middle dot */
            shown[o++] = (char)0xB7;
        }
        shown[o] = 0;
    }
    else {
        str_copy(shown, sizeof shown, ui.kb_text);
    }
    /* show the tail if it doesn't fit */
    const char *disp = shown;
    int maxw = 214;
    while (*disp && gfx_text_width(&font_md, disp) > maxw) {
        const char *p = disp;
        utf8_next(&p);
        disp = p;
    }
    int tw = gfx_text(&font_md, 24, 134, disp, C_TEXT);
    if ((now_ms() / 500) % 2 == 0) gfx_fill(24 + tw + 1, 114, 2, 26, C_GREEN);
    button(KEY_SHOW, SCR_W - 76, 111, 60, 32, ui.kb_hide ? "Show" : "Hide", &font_sm, BS_NORMAL);

    if (ui.kb_error[0])
        gfx_text(&font_sm, 14, 176, ui.kb_error, C_RED);
    else
        gfx_text(&font_sm, 14, 176,
                 ui.kb_secured == 2 ? "WEP key (5/13 chars or 10/26 hex)" : "WPA password (8-63 characters)", C_DIM);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%zu", len);
    gfx_text_right(&font_sm, SCR_W - 14, 176, cnt, C_DIM);

    /* keys */
    gfx_fill(0, KB_Y - 6, SCR_W, SCR_H - KB_Y + 6, 0x0C0C0C);
    int upper = ui.kb_shift || ui.kb_caps;
    for (int r = 0; r < 4; r++) {
        const char *row = KB_ROWS[ui.kb_sym][r];
        int nk = (int)strlen(row);
        int y = KB_Y + r * KB_KEY_H;
        int x0 = r == 2 ? 16 : r == 3 ? 48 : 0;
        for (int i = 0; i < nk; i++) {
            char c = row[i];
            if (upper && !ui.kb_sym && c >= 'a' && c <= 'z') c = (char)(c - 32);
            char lab[2] = {c, 0};
            key(KEY_CHAR + (unsigned char)c, x0 + i * 32, y, 32, lab, &font_md, 0x333333);
        }
    }
    int y3 = KB_Y + 3 * KB_KEY_H;
    key(KEY_SHIFT, 0, y3, 48, ui.kb_caps ? "CAPS" : "Shift", &font_sm, upper && !ui.kb_sym ? 0x2E6B45 : 0x262626);
    key(KEY_BKSP, 272, y3, 48, "Del", &font_sm, 0x262626);
    int y4 = KB_Y + 4 * KB_KEY_H;
    key(KEY_SYM, 0, y4, 64, ui.kb_sym ? "ABC" : "?123", &font_sm, 0x262626);
    key(KEY_SPACE, 64, y4, 128, "space", &font_sm, 0x333333);
    key(KEY_CANCEL, 192, y4, 64, "Cancel", &font_sm, 0x262626);
    gfx_rrect(256 + 2, y4 + 3, 60, KB_KEY_H - 6, 7, is_pressed(KEY_OK) ? C_GREEN_PRESSED : C_GREEN);
    gfx_text_center(&font_mdb, 288, y4 + 32, "Join", 0x000000);
    add_hit(KEY_OK, 256, y4, 64, KB_KEY_H);
}

/* Opens the password keyboard for a network. */
void open_keyboard(const char *ssid, int secured)
{
    ui.screen = SCR_KEYBOARD;
    str_copy(ui.kb_ssid, sizeof ui.kb_ssid, ssid);
    ui.kb_secured = secured;
    ui.kb_text[0] = 0;
    ui.kb_shift = ui.kb_caps = ui.kb_sym = 0;
    ui.kb_hide = 0;
    ui.kb_error[0] = 0;
}

/* Validates the password length for the security type, then starts connecting. */
static void kb_submit(void)
{
    size_t l = strlen(ui.kb_text);
    if (ui.kb_secured == 1 && (l < 8 || l > 63)) {
        str_copy(ui.kb_error, sizeof ui.kb_error, "Password must be 8-63 characters");
        return;
    }
    if (ui.kb_secured == 2 && !(l == 5 || l == 13 || l == 10 || l == 26)) {
        str_copy(ui.kb_error, sizeof ui.kb_error, "WEP key must be 5, 10, 13 or 26 chars");
        return;
    }
    /* deriving the WPA key takes a moment on this CPU: show feedback first */
    ui.screen = SCR_WIFI;
    ui_toast("Joining network...");
    ui_render();
    wifi_connect(ui.kb_ssid, ui.kb_text, ui.kb_secured);
    memset(ui.kb_text, 0, sizeof ui.kb_text);
}

/* Typed characters and the special keys. */
void press_keyboard(int id)
{
    if (id >= KEY_CHAR) {
        size_t l = strlen(ui.kb_text);
        if (l + 1 < sizeof ui.kb_text) {
            ui.kb_text[l] = (char)(id - KEY_CHAR);
            ui.kb_text[l + 1] = 0;
        }
        ui.kb_error[0] = 0;
        if (ui.kb_shift && !ui.kb_caps) ui.kb_shift = 0;
        return;
    }
    switch (id) {
    case KEY_SHIFT: {
        uint64_t now = now_ms();
        if (ui.kb_caps) {
            ui.kb_caps = 0;
            ui.kb_shift = 0;
        }
        else if (ui.kb_shift && now - ui.kb_last_shift < 500)
            ui.kb_caps = 1;
        else
            ui.kb_shift = !ui.kb_shift;
        ui.kb_last_shift = now;
        break;
    }
    case KEY_SYM: ui.kb_sym = !ui.kb_sym; break;
    case KEY_BKSP: {
        size_t l = strlen(ui.kb_text);
        if (l) ui.kb_text[l - 1] = 0;
        break;
    }
    case KEY_SPACE: {
        size_t l = strlen(ui.kb_text);
        if (l + 1 < sizeof ui.kb_text) {
            ui.kb_text[l] = ' ';
            ui.kb_text[l + 1] = 0;
        }
        break;
    }
    case KEY_CANCEL:
        memset(ui.kb_text, 0, sizeof ui.kb_text);
        ui.screen = SCR_WIFI;
        break;
    case KEY_OK: kb_submit(); break;
    case KEY_SHOW: ui.kb_hide = !ui.kb_hide; break;
    }
}
