/* Entities card ("entities": a titled list of rows) and glance card ("glance": a titled
 * grid of small buttons). Both become several items; the rows are card_entity_row. */

#include "cards/card.h"
#include "core/common.h"
#include "gfx/icons.h"
#include "ui/theme.h"

#define C_UNAVAIL 0x5A5A5A

extern const card_def card_button;
extern const card_def card_entity_row;

/* One item per listed entity; rows are "entity_id" strings or {entity, name, icon} objects,
 * {type: section, label} starts a sub-heading, other special rows are skipped. */
static void emit_rows(card_parser *p, int card, const card_def *def, int span, int rows)
{
    const json_doc *d = card_json(p);
    char title[96];
    card_str(p, card, "title", title, sizeof title);
    card_add_heading(p, title, 1);
    int ents = json_get(d, card, "entities");
    for (int i = 0;; i++) {
        int e = json_at(d, ents, i);
        if (e < 0) break;
        if (d->t[e].type == J_STR) {
            char id[64];
            json_str(d, e, id, sizeof id);
            ha_item *it = card_emit_sized(p, def, -1, span, rows);
            if (it) it->ent = (int16_t)card_entity(p, id);
            continue;
        }
        char rtype[24], label[96];
        card_str(p, e, "type", rtype, sizeof rtype);
        if (!strcmp(rtype, "section")) {
            card_str(p, e, "label", label, sizeof label);
            card_add_heading(p, label, 1);
        }
        else if (!rtype[0]) {
            card_emit_sized(p, def, e, span, rows);
        }
    }
}

static void parse_entities(card_parser *p, int card)
{
    emit_rows(p, card, &card_entity_row, card_span(p, &card_entity_row, card), 1);
}

static void parse_glance(card_parser *p, int card)
{
    int cols = (int)json_int(card_json(p), json_get(card_json(p), card, "columns"), 3);
    emit_rows(p, card, &card_button, 12 / CLAMP(cols, 1, 4), 2);
}

/* A row: icon, name, and a switch (toggleable entities) or the state on the right. */
static int row_height(const ha_item *it, int w) { return 48; }

static void draw_row(const card_ctx *c)
{
    const ha_entity *e = c->e;
    int on = e && ha_entity_is_on(e), na = card_unavailable(c);
    gfx_rrect(c->x, c->y, c->w, c->h, 10, c->pressed ? C_BTN_PRESSED : C_CARD);
    int cy = c->y + c->h / 2;
    uint32_t color = e ? ha_entity_color(e) : C_TEXT;
    icon_draw(&icons_24, card_icon(c), c->x + 26, cy, na ? C_UNAVAIL : on ? color : C_SUB);
    int right = c->x + c->w - 12, name_w;
    if (e && ha_entity_toggles(e) && !na && e->has_state) {
        card_switch(right, cy, on, color);
        name_w = right - 48 - (c->x + 52);
    }
    else {
        char st[64] = "";
        if (e) ha_entity_state_text(e, st, sizeof st);
        int sw = gfx_text_width(&font_sm, st);
        if (sw > c->w / 2) sw = c->w / 2;
        gfx_text_fit(&font_sm, right - sw, cy + 5, sw, st, C_SUB);
        name_w = right - sw - 12 - (c->x + 52);
    }
    gfx_text_fit(&font_md, c->x + 52, cy + 6, name_w, card_name(c), na ? C_UNAVAIL : C_TEXT);
}

static const char *const ENTITIES_TYPES[] = {"entities", NULL};
static const char *const GLANCE_TYPES[] = {"glance", NULL};

const card_def card_entities = {
    .name = "entities",
    .types = ENTITIES_TYPES,
    .span = 12,
    .rows = 1,
    .parse = parse_entities,
};

const card_def card_glance = {
    .name = "glance",
    .types = GLANCE_TYPES,
    .span = 12,
    .rows = 2,
    .parse = parse_glance,
};

const card_def card_entity_row = {
    .name = "entity-row",
    .span = 12,
    .rows = 1,
    .flags = CARD_TAPPABLE,
    .height = row_height,
    .draw = draw_row,
};
