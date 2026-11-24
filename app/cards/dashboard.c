/*
 * Lovelace dashboard config -> flat list of items for the UI.
 *
 * This file handles structure only: views (masonry / panel / sidebar views of cards, and
 * sections views of grid sections), and the containers horizontal-stack, vertical-stack,
 * grid and conditional. Every other card is handed to its card_def (registry.c), which
 * turns it into items; unknown types become an "unsupported" placeholder.
 *
 * Widths follow HA's 12-column sections grid (grid_options.columns). In a sections view
 * each section becomes one screen-sized page, so pages can be designed in HA's editor.
 */

#include <stdlib.h>

#include "cards/cards.h"
#include "core/common.h"

struct card_parser {
    const json_doc *d;
    ha_dash *out;
    int sections;      /* inside a sections view (HA's grid defaults apply) */
    int span_override; /* width forced by an enclosing stack / grid, 0 if none */
};

static int is(const char *a, const char *b) { return !strcmp(a, b); }

const json_doc *card_json(card_parser *p) { return p->d; }

void card_str(card_parser *p, int obj, const char *key, char *out, size_t len)
{
    out[0] = 0;
    if (obj >= 0) json_str(p->d, json_get(p->d, obj, key), out, len);
}

int card_bool(card_parser *p, int obj, const char *key, int def)
{
    return obj >= 0 ? json_bool(p->d, json_get(p->d, obj, key), def) : def;
}

/* ---------- entities, attributes and forecasts to track ---------- */

int card_entity(card_parser *p, const char *id)
{
    ha_dash *o = p->out;
    if (!id[0] || !strchr(id, '.')) return -1;
    for (int i = 0; i < o->n_ents; i++)
        if (is(o->ents[i].id, id)) return i;
    if (o->n_ents >= HA_MAX_ENTS) return -1;
    ha_entity *e = &o->ents[o->n_ents];
    memset(e, 0, sizeof *e);
    str_copy(e->id, sizeof e->id, id);
    e->brightness = -1;
    return o->n_ents++;
}

void card_want_attr(card_parser *p, int ent, const char *key)
{
    if (ent < 0) return;
    ha_entity *e = &p->out->ents[ent];
    for (int i = 0; i < e->n_attrs; i++)
        if (is(e->attrs[i].key, key)) return;
    if (e->n_attrs < HA_MAX_ATTRS) str_copy(e->attrs[e->n_attrs++].key, sizeof e->attrs[0].key, key);
}

int card_want_forecast(card_parser *p, int ent, const char *type)
{
    ha_dash *o = p->out;
    if (ent < 0) return -1;
    for (int i = 0; i < o->n_fcs; i++)
        if (o->fcs[i].ent == ent && is(o->fcs[i].type, type)) return i;
    if (o->n_fcs >= HA_MAX_FORECASTS) return -1;
    ha_forecast *f = &o->fcs[o->n_fcs];
    memset(f, 0, sizeof *f);
    f->ent = (int16_t)ent;
    str_copy(f->type, sizeof f->type, type);
    return o->n_fcs++;
}

/* ---------- items ---------- */

static ha_item *new_item(ha_dash *o, int kind)
{
    if (o->n_items >= HA_MAX_ITEMS) return NULL;
    ha_item *it = &o->items[o->n_items++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->span = 12;
    it->rows = 1;
    it->ent = -1;
    return it;
}

/* Raw JSON text of token i (objects include their braces). */
static void raw_json(const json_doc *d, int i, char *out, size_t len)
{
    int n = d->t[i].end - d->t[i].start;
    if (d->t[i].type == J_STR || n < 0 || (size_t)n >= len) {
        out[0] = 0;
        return;
    }
    memcpy(out, d->js + d->t[i].start, (size_t)n);
    out[n] = 0;
}

/* tap_action: toggle / none / perform-action (call-service); anything else uses the default. */
static void parse_action(card_parser *p, int card, ha_item *it)
{
    const json_doc *d = p->d;
    int ta = json_get(d, card, "tap_action");
    if (ta < 0) return;
    char action[32];
    card_str(p, ta, "action", action, sizeof action);
    if (is(action, "none")) {
        it->act = HA_ACT_NONE;
    }
    else if (is(action, "toggle")) {
        it->act = HA_ACT_TOGGLE;
    }
    else if (is(action, "perform-action") || is(action, "call-service")) {
        card_str(p, ta, "perform_action", it->service, sizeof it->service);
        if (!it->service[0]) card_str(p, ta, "service", it->service, sizeof it->service);
        if (!strchr(it->service, '.')) return;
        it->act = HA_ACT_CALL;
        /* target / data objects are passed through untouched */
        char t[256] = "", sd[256] = "";
        int tt = json_get(d, ta, "target");
        int dd = json_get(d, ta, "data");
        if (dd < 0) dd = json_get(d, ta, "service_data");
        if (tt >= 0) raw_json(d, tt, t, sizeof t);
        if (dd >= 0) raw_json(d, dd, sd, sizeof sd);
        snprintf(it->data, sizeof it->data, "%s%s%s%s%s", t[0] ? "\"target\":" : "", t, t[0] && sd[0] ? "," : "",
                 sd[0] ? "\"service_data\":" : "", sd);
    }
}

/* Width in 12ths from grid_options.columns (sections) or the older layout_options. */
static int grid_columns(const json_doc *d, int card, int def)
{
    int go = json_get(d, card, "grid_options");
    if (go >= 0) {
        int c = json_get(d, go, "columns");
        if (c >= 0) {
            char s[8];
            if (json_str(d, c, s, sizeof s) == 0) return is(s, "full") ? 12 : def;
            return (int)json_int(d, c, def);
        }
    }
    int lo = json_get(d, card, "layout_options");
    if (lo >= 0) {
        int c = json_get(d, lo, "grid_columns");
        if (c >= 0) {
            char s[8];
            if (json_str(d, c, s, sizeof s) == 0) return is(s, "full") ? 12 : def;
            return (int)json_int(d, c, def / 3) * 3; /* that grid had 4 columns */
        }
    }
    return def;
}

static int grid_rows(const json_doc *d, int card, int def)
{
    int go = json_get(d, card, "grid_options");
    int r = go >= 0 ? json_get(d, go, "rows") : -1;
    if (r < 0) {
        int lo = json_get(d, card, "layout_options");
        r = lo >= 0 ? json_get(d, lo, "grid_rows") : -1;
    }
    return r >= 0 && d->t[r].type == J_PRIM ? (int)json_int(d, r, def) : def;
}

int card_span(card_parser *p, const card_def *def, int card)
{
    if (def->flags & CARD_OWN_ROW) return 12;
    if (p->span_override) return p->span_override;
    /* HA's defaults in sections views; full width in masonry views */
    return grid_columns(p->d, card, p->sections ? def->span : 12);
}

ha_item *card_emit_sized(card_parser *p, const card_def *def, int card, int span, int rows)
{
    ha_item *it = new_item(p->out, HI_CARD);
    if (!it) return NULL;
    it->card = (uint8_t)card_id(def);
    it->span = (uint8_t)CLAMP(span, 3, 12);
    it->rows = (uint8_t)CLAMP(rows, 1, 8);
    if (card >= 0) {
        char id[64];
        card_str(p, card, "entity", id, sizeof id);
        it->ent = (int16_t)card_entity(p, id);
        card_str(p, card, "name", it->name, sizeof it->name);
        card_str(p, card, "icon", it->icon, sizeof it->icon);
        parse_action(p, card, it);
    }
    return it;
}

ha_item *card_emit(card_parser *p, const card_def *def, int card)
{
    ha_item *it = card_emit_sized(p, def, card, card_span(p, def, card), grid_rows(p->d, card, def->rows));
    if (it && def->options) def->options(p, card, it);
    return it;
}

/* ---------- structure: containers, sections, views ---------- */

static void flatten(card_parser *p, int card);

/* Children of a stack / grid card, each given `span` columns. */
static void flatten_children(card_parser *p, int cards, int span)
{
    int saved = p->span_override;
    p->span_override = span;
    for (int i = 0;; i++) {
        int ch = json_at(p->d, cards, i);
        if (ch < 0) break;
        flatten(p, ch);
    }
    p->span_override = saved;
}

static int count(const json_doc *d, int arr)
{
    int n = 0;
    while (json_at(d, arr, n) >= 0) n++;
    return n;
}

static void flatten(card_parser *p, int card)
{
    const json_doc *d = p->d;
    char type[48], title[96];
    card_str(p, card, "type", type, sizeof type);
    card_str(p, card, "title", title, sizeof title);
    int span = p->span_override ? p->span_override : grid_columns(d, card, 12);

    if (is(type, "horizontal-stack")) {
        card_add_heading(p, title, 1);
        int cards = json_get(d, card, "cards");
        int n = count(d, cards);
        if (n) flatten_children(p, cards, span / n);
    }
    else if (is(type, "vertical-stack")) {
        card_add_heading(p, title, 1);
        flatten_children(p, json_get(d, card, "cards"), span);
    }
    else if (is(type, "grid")) {
        card_add_heading(p, title, 1);
        int cols = (int)json_int(d, json_get(d, card, "columns"), 3);
        flatten_children(p, json_get(d, card, "cards"), span / CLAMP(cols, 1, 4));
    }
    else if (is(type, "conditional")) {
        int inner = json_get(d, card, "card");
        if (inner >= 0) flatten(p, inner);
    }
    else {
        const card_def *def = card_find(type);
        if (!def) {
            /* a card type we can't draw: a placeholder that says which */
            extern const card_def card_unsupported;
            ha_item *it = card_emit(p, &card_unsupported, card);
            if (it) str_copy(it->text, sizeof it->text, type[0] ? type : "card");
        }
        else if (def->parse) {
            def->parse(p, card);
        }
        else {
            card_emit(p, def, card);
        }
    }
}

int ha_dashboard_parse(const char *js, size_t len, ha_dash *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    int max = (int)(len / 3) + 64;
    jtok *t = malloc((size_t)max * sizeof *t);
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    json_doc d;
    int rc = -1;
    if (json_parse(&d, js, len, t, max) <= 0 || d.t[0].type != J_OBJ) {
        snprintf(err, errlen, "dashboard config isn't valid JSON");
        goto done;
    }
    if (json_get(&d, 0, "strategy") >= 0) {
        snprintf(err, errlen, "this dashboard is auto-generated; take control of it in Home Assistant first");
        goto done;
    }
    card_parser p = {&d, out, 0, 0};
    int views = json_get(&d, 0, "views");
    for (int v = 0;; v++) {
        int view = json_at(&d, views, v);
        if (view < 0) break;
        int sections = json_get(&d, view, "sections");
        ha_item *vi = new_item(out, HI_VIEW);
        if (vi) {
            card_str(&p, view, "title", vi->text, sizeof vi->text);
            if (!vi->text[0]) snprintf(vi->text, sizeof vi->text, "%s", v ? "View" : "Home");
            vi->paged = sections >= 0;
        }
        p.sections = sections >= 0;
        if (sections >= 0) {
            /* sections view: each section is a 12-column grid of cards, shown as its own page */
            for (int s = 0;; s++) {
                int sec = json_at(&d, sections, s);
                if (sec < 0) break;
                if (s) new_item(out, HI_PAGE);
                char title[96];
                card_str(&p, sec, "title", title, sizeof title); /* pre-2024.8 sections had titles */
                card_add_heading(&p, title, 0);
                flatten_children(&p, json_get(&d, sec, "cards"), 0);
            }
        }
        else {
            flatten_children(&p, json_get(&d, view, "cards"), 0);
        }
    }
    if (!out->n_items) {
        snprintf(err, errlen, "the dashboard has no views");
        goto done;
    }
    rc = 0;
done:
    free(t);
    return rc;
}

void ha_dashboard_from_list(const char *list, ha_dash *out)
{
    extern const card_def card_tile;
    memset(out, 0, sizeof *out);
    card_parser p = {NULL, out, 1, 0};
    ha_item *vi = new_item(out, HI_VIEW);
    if (vi) str_copy(vi->text, sizeof vi->text, "Home");
    char buf[1024];
    str_copy(buf, sizeof buf, list);
    for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        ha_item *it = card_emit_sized(&p, &card_tile, -1, card_tile.span, card_tile.rows);
        if (it) it->ent = (int16_t)card_entity(&p, tok);
    }
}
