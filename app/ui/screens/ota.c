/* Software update screen: shown over everything while an OTA update is waiting for its PIN
 * or being installed (web/ota.c). */

#include "core/common.h"
#include "ui/internal.h"

#define CANCEL_X 20
#define CANCEL_Y 400
#define CANCEL_W (SCR_W - 40)
#define CANCEL_H 60

void render_ota(const ota_view *v)
{
    /* plain title bar: none of the home screen's shortcuts apply here */
    gfx_fill(0, 0, SCR_W, 44, C_HEADER);
    gfx_fill(0, 44, SCR_W, 1, C_LINE);
    gfx_text(&font_mdb, 14, 29, "Software update", C_TEXT);
    if (v->phase == OTA_INSTALLING) {
        gfx_text_center(&font_lg, SCR_W / 2, 220, "Installing update...", C_TEXT);
        gfx_text_center(&font_sm, SCR_W / 2, 252, "ThermoDeck restarts in a moment.", C_SUB);
        return;
    }
    char line[64], pin_disp[16], left[24];
    snprintf(line, sizeof line, "Requested from %s", v->from);
    gfx_text_center(&font_md, SCR_W / 2, 110, line, C_SUB);
    gfx_text_wrap(&font_sm, 24, 150, SCR_W - 48, 3, 18,
                  "To install it, enter this PIN on the computer sending the update. If you didn't "
                  "start an update, tap Cancel.",
                  C_SUB, 1, 1);
    snprintf(pin_disp, sizeof pin_disp, "%.3s %.3s", v->pin, v->pin + 3);
    gfx_text_center(&font_sm, SCR_W / 2, 250, "PIN", C_SUB);
    gfx_text_center(&font_xl, SCR_W / 2, 292, pin_disp, C_TEXT);
    snprintf(left, sizeof left, "Expires in %d:%02d", v->seconds_left / 60, v->seconds_left % 60);
    gfx_text_center(&font_sm, SCR_W / 2, 340, left, C_DIM);
    button(BTN_OTA_CANCEL, CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, "Cancel", &font_md, BS_NORMAL);
}

/* Whether (x, y) is on the Cancel button (taps go straight here while the screen is up). */
int ota_cancel_hit(int x, int y)
{
    return x >= CANCEL_X && x < CANCEL_X + CANCEL_W && y >= CANCEL_Y && y < CANCEL_Y + CANCEL_H;
}
