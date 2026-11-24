/* Single-value cards ("entity", "sensor", "gauge", "thermostat", "media-control"): name and
 * icon on top, the state large underneath. */

#include "cards/card.h"
#include "gfx/icons.h"
#include "ui/theme.h"

static const char *const TYPES[] = {"entity", "sensor", "gauge", "thermostat", "media-control", NULL};

static void draw(const card_ctx *c)
{
    const ha_entity *e = c->e;
    int on = e && ha_entity_is_on(e);
    card_bg(c);
    gfx_text_fit(&font_sm, c->x + 14, c->y + 26, c->w - 52, card_name(c), C_SUB);
    icon_draw(&icons_24, card_icon(c), c->x + c->w - 24, c->y + 22, on ? ha_entity_color(e) : C_DIM);
    if (!e) return;
    int by = c->y + c->h - 18;
    char num[40];
    if (e->unit[0] && e->has_state && strcmp(e->state, "unavailable")) {
        /* number large, unit small after it */
        ha_entity plain = *e;
        plain.unit[0] = 0;
        ha_entity_state_text(&plain, num, sizeof num);
        const font_t *f = gfx_text_width(&font_xl, num) + 40 < c->w - 28 ? &font_xl : &font_lg;
        int w = gfx_text(f, c->x + 14, by, num, C_TEXT);
        gfx_text_fit(&font_md, c->x + 14 + w + 4, by, c->w - 32 - w, e->unit, C_SUB);
    }
    else {
        ha_entity_state_text(e, num, sizeof num);
        const font_t *f = gfx_text_width(&font_lg, num) < c->w - 28 ? &font_lg : &font_md;
        gfx_text_fit(f, c->x + 14, by, c->w - 28, num, C_TEXT);
    }
}

const card_def card_value = {
    .name = "value",
    .types = TYPES,
    .span = 6,
    .rows = 2,
    .flags = CARD_TAPPABLE,
    .draw = draw,
};
