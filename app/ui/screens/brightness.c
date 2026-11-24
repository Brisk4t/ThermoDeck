/* Brightness page: big percentage, draggable slider and -/+ steps. */

#include "core/common.h"
#include "ui/internal.h"

#define SL_X0 50
#define SL_X1 (SCR_W - 50)
#define SL_Y  250

/* Changes brightness live (saved when the finger lifts or a step button is tapped). */
static void brightness_set(int pct)
{
    pct = CLAMP(pct, BRIGHT_MIN, 100);
    if (pct == ui.brightness) return;
    ui.brightness = pct;
    brightness_apply(pct);
    ui.dirty = 1;
}

/* Finger x position on the slider -> brightness. */
void slide_bright(int x)
{
    int pct = BRIGHT_MIN + (x - SL_X0) * (100 - BRIGHT_MIN) / (SL_X1 - SL_X0);
    brightness_set(pct);
}

/* Brightness screen: big percentage, draggable slider, -/+ steps of 10%. */
void render_bright(void)
{
    draw_header("Brightness");

    char v[8];
    snprintf(v, sizeof v, "%d%%", ui.brightness);
    draw_sun(SCR_W / 2, 118, 12, C_TEXT, C_BG);
    gfx_text_center(&font_xl, SCR_W / 2, 196, v, C_TEXT);

    /* slider: track, fill, knob */
    int kx = SL_X0 + (ui.brightness - BRIGHT_MIN) * (SL_X1 - SL_X0) / (100 - BRIGHT_MIN);
    gfx_rrect(SL_X0, SL_Y - 4, SL_X1 - SL_X0, 8, 4, 0x3E3E3E);
    gfx_rrect(SL_X0, SL_Y - 4, kx - SL_X0 + 4, 8, 4, C_GREEN);
    gfx_circle(kx, SL_Y, ui.drag_id == BTN_BRIGHT_SLIDER ? 16 : 13, C_TEXT);
    draw_sun(SL_X0 - 26, SL_Y, 3, C_DIM, C_BG);
    draw_sun(SL_X1 + 26, SL_Y, 5, C_SUB, C_BG);
    add_hit(BTN_BRIGHT_SLIDER, 0, SL_Y - 34, SCR_W, 68);

    /* fine steps */
    button(BTN_BRIGHT_DOWN, 20, 306, 134, 64, NULL, &font_md, ui.brightness > BRIGHT_MIN ? BS_NORMAL : BS_DISABLED);
    gfx_rrect(20 + 67 - 9, 306 + 31, 18, 3, 1, ui.brightness > BRIGHT_MIN ? C_TEXT : C_DIM);
    button(BTN_BRIGHT_UP, 166, 306, 134, 64, NULL, &font_md, ui.brightness < 100 ? BS_NORMAL : BS_DISABLED);
    uint32_t pc = ui.brightness < 100 ? C_TEXT : C_DIM;
    gfx_rrect(166 + 67 - 9, 306 + 31, 18, 3, 1, pc);
    gfx_rrect(166 + 67 - 1, 306 + 23, 3, 18, 1, pc);

    button(BTN_DONE, 20, 412, SCR_W - 40, 60, "Done", &font_mdb, BS_PRIMARY);
}

/* -/+ step to the previous / next multiple of 10%. */
void press_bright(int id)
{
    switch (id) {
    case BTN_BRIGHT_DOWN:
        brightness_set(ui.brightness <= 10 ? BRIGHT_MIN : (ui.brightness - 1) / 10 * 10);
        settings_save();
        break;
    case BTN_BRIGHT_UP:
        brightness_set((ui.brightness / 10 + 1) * 10);
        settings_save();
        break;
    }
}
