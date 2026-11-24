#ifndef CARDS_CARD_H
#define CARDS_CARD_H

/*
 * The card template. Each Lovelace card type ThermoDeck can draw is one file in app/cards/
 * that defines a `const card_def card_<name>` and is listed in registry.c. A card is:
 *
 *   - which Lovelace "type"s it handles and its default size in HA's sections grid,
 *   - optionally, how to read its options from the card config (and which entity
 *     attributes / forecasts it needs the HA client to track),
 *   - optionally, its height when it isn't a whole number of grid rows,
 *   - how to draw it into a box with the current entity state.
 *
 * Everything else (sizing from grid_options, entity / name / icon / tap_action, layout,
 * pages, taps) is done for every card by the framework. See ADDING_CARDS.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  /* snprintf, for card files */
#include <string.h> /* strcmp, strcat ... */

#include "gfx/gfx.h"
#include "hass/hass.h"
#include "json/json.h"

/* ---------- the template ---------- */

typedef struct card_parser card_parser; /* parse state of the dashboard being loaded */

/* Everything a draw function gets. */
typedef struct {
    int x, y, w, h;     /* the box to fill */
    const ha_item *it;  /* this card */
    const ha_entity *e; /* its entity, or NULL */
    const ha_dash *d;   /* the whole dashboard (e.g. for it->opt indexes into d->fcs) */
    int pressed;        /* finger is on it */
    int clock_24h;      /* user's clock setting, for times */
} card_ctx;

enum {
    CARD_TAPPABLE = 1, /* taps run the item's action (entity default or tap_action) */
    CARD_OWN_ROW = 2,  /* always full width, and kept on the same page as the row after it */
};

typedef struct card_def {
    const char *name;         /* short name, for logs */
    const char *const *types; /* Lovelace types handled, NULL-terminated (NULL: only made by other cards) */
    uint8_t span, rows;       /* default size in a sections grid: width in 12ths, height in grid rows */
    uint8_t flags;            /* CARD_* */
    /* Optional. Reads card-specific options into the item card_emit() made for this card. */
    void (*options)(card_parser *p, int card, ha_item *it);
    /* Optional. Replaces the default "one item per card" (e.g. a card that becomes several rows). */
    void (*parse)(card_parser *p, int card);
    /* Optional. Height in px when laid out w px wide; default: it->rows grid rows. */
    int (*height)(const ha_item *it, int w);
    void (*draw)(const card_ctx *c);
} card_def;

/* Grid metrics. Same 12 columns and row counts as HA's sections grid, but rows are 48 px
 * (HA: 56) so that 8 of them fit below the header: a page is 12 columns x 8 rows. */
#define CARD_ROW_H 48
#define CARD_VGAP  4 /* between rows */
#define CARD_HGAP  6 /* between columns */
/* Height of a card `rows` grid rows tall. */
#define CARD_ROWS_H(rows) ((rows) * CARD_ROW_H + ((rows) - 1) * CARD_VGAP)

/* ---------- parsing helpers (dashboard.c) ---------- */

const json_doc *card_json(card_parser *p);
/* One item for `card`: sized from its grid_options (or def's defaults, or the enclosing
 * stack), with entity, name, icon and tap_action read. Returns NULL if the dashboard is full. */
ha_item *card_emit(card_parser *p, const card_def *def, int card);
/* Same, with an explicit size. */
ha_item *card_emit_sized(card_parser *p, const card_def *def, int card, int span, int rows);
/* Width (12ths) that `card` gets where it appears. */
int card_span(card_parser *p, const card_def *def, int card);
/* String / bool member of a JSON object (out = "" / def if missing). */
void card_str(card_parser *p, int obj, const char *key, char *out, size_t len);
int card_bool(card_parser *p, int obj, const char *key, int def);
/* Entity index for an entity_id (added to the subscription), or -1. */
int card_entity(card_parser *p, const char *entity_id);
/* Ask the HA client to track attribute `key` of entity ent (read it with ha_entity_attr()). */
void card_want_attr(card_parser *p, int ent, const char *key);
/* Ask for a forecast of entity ent ("daily", "hourly", "twice_daily"); returns its index in
 * ha_dash.fcs, or -1. */
int card_want_forecast(card_parser *p, int ent, const char *type);
/* Adds a heading item (heading.c); small = subtitle style. */
void card_add_heading(card_parser *p, const char *text, int small);

/* ---------- drawing helpers (kit.c) ---------- */

void card_bg(const card_ctx *c);               /* rounded card background (pressed highlight) */
const char *card_name(const card_ctx *c);      /* name override, else the entity's name / id */
int card_icon(const card_ctx *c);              /* icon index: override, entity's, domain default */
int card_unavailable(const card_ctx *c);       /* entity is unavailable */
uint32_t card_tint(uint32_t color, int alpha); /* color blended over the card background */
/* On/off switch 40x22 with its right edge at rx. */
void card_switch(int rx, int cy, int on, uint32_t color);
/* Text centered on cx, truncated to w. */
void card_text_center(const font_t *f, int cx, int y, int w, const char *s, uint32_t c);
/* Text in the largest of bold 18 / 18 / 14 px that fits w (truncated at 14 px if none does). */
void card_text_shrink(int x, int y, int w, const char *s, uint32_t c);
/* JSON number literal -> text with `decimals` decimals ("21.46" -> "21.5"); "" stays "". */
void card_number(const char *lit, int decimals, char *out, size_t len);

#endif
