/* Drawing helpers shared by the card files. */

#include <stdlib.h>

#include "cards/card.h"
#include "core/common.h"
#include "gfx/icons.h"
#include "ui/theme.h"

void card_bg(const card_ctx *c) { gfx_rrect(c->x, c->y, c->w, c->h, 12, c->pressed ? C_BTN_PRESSED : C_CARD); }

const char *card_name(const card_ctx *c)
{
    if (c->it->name[0]) return c->it->name;
    if (c->e) return c->e->name[0] ? c->e->name : c->e->id;
    return "";
}

int card_icon(const card_ctx *c)
{
    int ic = icon_find(c->it->icon);
    if (ic >= 0) return ic;
    if (c->e) {
        ic = icon_find(ha_entity_icon(c->e));
        if (ic < 0) {
            /* custom icon that isn't compiled in: use the domain default instead */
            ha_entity plain = *c->e;
            plain.icon[0] = 0;
            ic = icon_find(ha_entity_icon(&plain));
        }
    }
    return ic >= 0 ? ic : icon_find("bookmark");
}

int card_unavailable(const card_ctx *c) { return c->e && !strcmp(c->e->state, "unavailable"); }

uint32_t card_tint(uint32_t color, int alpha)
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int fg = (int)((color >> sh) & 255), bg = (C_CARD >> sh) & 255;
        r |= (uint32_t)((bg * (255 - alpha) + fg * alpha) / 255) << sh;
    }
    return r;
}

void card_switch(int rx, int cy, int on, uint32_t color)
{
    int x = rx - 40;
    gfx_rrect(x, cy - 11, 40, 22, 11, on ? color : 0x4A4A4A);
    gfx_circle(on ? x + 29 : x + 11, cy, 8, on ? 0xFFFFFF : 0xBDBDBD);
}

void card_text_center(const font_t *f, int cx, int y, int w, const char *s, uint32_t c)
{
    if (gfx_text_width(f, s) > w)
        gfx_text_fit(f, cx - w / 2, y, w, s, c);
    else
        gfx_text_center(f, cx, y, s, c);
}

void card_number(const char *lit, int decimals, char *out, size_t len)
{
    char *end;
    double v = strtod(lit, &end);
    if (!lit[0] || end == lit) {
        str_copy(out, len, lit);
        return;
    }
    snprintf(out, len, "%.*f", decimals, v);
    if (!strcmp(out, "-0")) str_copy(out, len, "0");
}

void card_text_shrink(int x, int y, int w, const char *s, uint32_t c)
{
    static const font_t *const SIZES[] = {&font_mdb, &font_md, &font_sm};
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++)
        if (gfx_text_width(SIZES[i], s) <= w) {
            gfx_text(SIZES[i], x, y, s, c);
            return;
        }
    gfx_text_fit(&font_sm, x, y, w, s, c);
}
