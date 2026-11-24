/* Heading card ("heading"), also used for the titles of other cards and sections. */

#include "cards/card.h"
#include "core/common.h"
#include "ui/theme.h"

enum { HEADING_SUBTITLE = 1 };

static const char *const TYPES[] = {"heading", NULL};

static void options(card_parser *p, int card, ha_item *it)
{
    char style[16];
    card_str(p, card, "heading", it->text, sizeof it->text);
    card_str(p, card, "heading_style", style, sizeof style);
    if (!strcmp(style, "subtitle")) it->flags |= HEADING_SUBTITLE;
}

static int height(const ha_item *it, int w) { return it->flags & HEADING_SUBTITLE ? 26 : 34; }

static void draw(const card_ctx *c)
{
    if (c->it->flags & HEADING_SUBTITLE)
        gfx_text_fit(&font_sm, c->x + 4, c->y + 19, c->w - 8, c->it->text, C_SUB);
    else
        gfx_text_fit(&font_mdb, c->x + 4, c->y + 26, c->w - 8, c->it->text, C_TEXT);
}

const card_def card_heading = {
    .name = "heading",
    .types = TYPES,
    .span = 12,
    .rows = 1,
    .flags = CARD_OWN_ROW,
    .options = options,
    .height = height,
    .draw = draw,
};

void card_add_heading(card_parser *p, const char *text, int small)
{
    if (!text[0]) return;
    ha_item *it = card_emit_sized(p, &card_heading, -1, 12, 1);
    if (!it) return;
    str_copy(it->text, sizeof it->text, text);
    if (small) it->flags |= HEADING_SUBTITLE;
}
