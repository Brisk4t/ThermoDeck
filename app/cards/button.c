/* Button card ("button", and "light" drawn the same way): big icon and name. */

#include "cards/card.h"
#include "gfx/icons.h"
#include "ui/theme.h"

#define C_UNAVAIL 0x5A5A5A

static const char *const TYPES[] = {"button", "light", NULL};

static void draw(const card_ctx *c)
{
    const ha_entity *e = c->e;
    int on = e && ha_entity_is_on(e), na = card_unavailable(c);
    card_bg(c);
    int cx = c->x + c->w / 2;
    const font_t *set = c->h >= 100 && c->w >= 60 ? &icons_36 : &icons_24;
    uint32_t ic = na ? C_UNAVAIL : on ? ha_entity_color(e) : e ? C_SUB : C_TEXT;
    icon_draw(set, card_icon(c), cx, c->y + c->h / 2 - 10, ic);
    card_text_center(&font_sm, cx, c->y + c->h - 14, c->w - 12, card_name(c), na ? C_UNAVAIL : C_TEXT);
}

const card_def card_button = {
    .name = "button",
    .types = TYPES,
    .span = 3,
    .rows = 2,
    .flags = CARD_TAPPABLE,
    .draw = draw,
};
