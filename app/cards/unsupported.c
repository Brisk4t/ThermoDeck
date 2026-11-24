/* Placeholder for card types ThermoDeck can't draw (item text = the card's type). */

#include "cards/card.h"
#include "ui/theme.h"

static int height(const ha_item *it, int w) { return 44; }

static void draw(const card_ctx *c)
{
    char msg[96];
    snprintf(msg, sizeof msg, "%s card", c->it->text);
    gfx_rrect(c->x, c->y, c->w, c->h, 12, 0x181818);
    gfx_text_fit(&font_sm, c->x + 12, c->y + 27, c->w - 24, msg, C_DIM);
}

const card_def card_unsupported = {
    .name = "unsupported",
    .span = 12,
    .rows = 1,
    .height = height,
    .draw = draw,
};
