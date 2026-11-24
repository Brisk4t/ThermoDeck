/* Tile card ("tile"): icon in a tinted circle, name and state. */

#include "cards/card.h"
#include "gfx/icons.h"
#include "ui/theme.h"

enum { TILE_VERTICAL = 1 };

#define C_ICON_BG 0x2C2C2C
#define C_UNAVAIL 0x5A5A5A

static const char *const TYPES[] = {"tile", NULL};

static void options(card_parser *p, int card, ha_item *it)
{
    if (card_bool(p, card, "vertical", 0)) it->flags |= TILE_VERTICAL;
}

static void draw(const card_ctx *c)
{
    const ha_entity *e = c->e;
    int on = e && ha_entity_is_on(e), na = card_unavailable(c);
    uint32_t color = e ? ha_entity_color(e) : C_GREEN;
    uint32_t icon_c = na ? C_UNAVAIL : on ? color : C_SUB, name_c = na ? C_UNAVAIL : C_TEXT;
    card_bg(c);
    char st[64] = "";
    if (e) ha_entity_state_text(e, st, sizeof st);
    /* like HA, the content is centered vertically, however many rows the tile has */
    int cy = c->y + c->h / 2;
    if (c->it->flags & TILE_VERTICAL) {
        /* icon above, name and state centered below it */
        int cx = c->x + c->w / 2, iy = cy - 22;
        gfx_circle(cx, iy, 18, on ? card_tint(color, 60) : C_ICON_BG);
        icon_draw(&icons_24, card_icon(c), cx, iy, icon_c);
        card_text_center(&font_sm, cx, cy + 16, c->w - 16, card_name(c), name_c);
        card_text_center(&font_sm, cx, cy + 34, c->w - 16, st, C_SUB);
        return;
    }
    gfx_circle(c->x + 28, cy, 18, on ? card_tint(color, 60) : C_ICON_BG);
    icon_draw(&icons_24, card_icon(c), c->x + 28, cy, icon_c);
    int tx = c->x + 54, tw = c->w - 54 - 8;
    gfx_text_fit(&font_sm, tx, cy - 3, tw, card_name(c), name_c);
    gfx_text_fit(&font_sm, tx, cy + 15, tw, st, C_SUB);
}

const card_def card_tile = {
    .name = "tile",
    .types = TYPES,
    .span = 6,
    .rows = 1,
    .flags = CARD_TAPPABLE,
    .options = options,
    .draw = draw,
};
