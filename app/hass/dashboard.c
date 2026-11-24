/*
 * Lovelace dashboard config -> flat list of items for the UI.
 *
 * Supported: masonry / panel / sidebar views (cards) and sections views (sections of
 * grid cards); tile, button, entity, sensor, gauge, light, thermostat, glance, entities,
 * heading, markdown, media-control and weather-forecast cards; horizontal-stack,
 * vertical-stack, grid and conditional containers. Other cards show as "unsupported".
 * Widths follow HA's 12-column sections grid (grid_options.columns).
 */

#include <stdlib.h>

#include "core/common.h"
#include "hass/hass.h"
#include "json/json.h"

typedef struct {
    const json_doc *d;
    ha_dash *out;
} ctx;

static int is(const char *a, const char *b) { return !strcmp(a, b); }

/* String member of obj (or "" if missing / not a string). */
static void get_str(const json_doc *d, int obj, const char *key, char *out, size_t len)
{
    out[0] = 0;
    if (obj >= 0) json_str(d, json_get(d, obj, key), out, len);
}

/* Index of entity id in the list, adding it if new; -1 if the list is full or id is empty. */
static int add_entity(ha_dash *o, const char *id)
{
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

static ha_item *new_item(ctx *c, int kind, int span, int rows)
{
    ha_dash *o = c->out;
    if (o->n_items >= HA_MAX_ITEMS) return NULL;
    ha_item *it = &o->items[o->n_items++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->span = (uint8_t)CLAMP(span, 3, 12);
    it->rows = (uint8_t)CLAMP(rows, 1, 4);
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
static void parse_action(const json_doc *d, int card, ha_item *it)
{
    int ta = json_get(d, card, "tap_action");
    if (ta < 0) return;
    char action[32];
    get_str(d, ta, "action", action, sizeof action);
    if (is(action, "none"))
        it->act = HA_ACT_NONE;
    else if (is(action, "toggle"))
        it->act = HA_ACT_TOGGLE;
    else if (is(action, "perform-action") || is(action, "call-service")) {
        get_str(d, ta, "perform_action", it->service, sizeof it->service);
        if (!it->service[0]) get_str(d, ta, "service", it->service, sizeof it->service);
        if (!strchr(it->service, '.')) return;
        it->act = HA_ACT_CALL;
        /* target / data objects are passed through untouched */
        char t[256] = "", sd[256] = "";
        int tt = json_get(d, ta, "target");
        int dd = json_get(d, ta, "data");
        if (dd < 0) dd = json_get(d, ta, "service_data");
        if (tt >= 0) raw_json(d, tt, t, sizeof t);
        if (dd >= 0) raw_json(d, dd, sd, sizeof sd);
        snprintf(it->data, sizeof it->data, "%s%s%s%s%s%s", t[0] ? "\"target\":" : "", t, t[0] && sd[0] ? "," : "",
                 sd[0] ? "\"service_data\":" : "", sd, "");
    }
}

/* Width in 12ths from grid_options.columns (sections) or the older layout_options. */
static int card_span(const json_doc *d, int card, int def)
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

static int card_rows(const json_doc *d, int card, int def)
{
    int go = json_get(d, card, "grid_options");
    int r = go >= 0 ? json_get(d, go, "rows") : -1;
    if (r < 0) {
        int lo = json_get(d, card, "layout_options");
        r = lo >= 0 ? json_get(d, lo, "grid_rows") : -1;
    }
    return r >= 0 && d->t[r].type == J_PRIM ? (int)json_int(d, r, def) : def;
}

/* An item for an entity-centric card: entity, name and icon overrides, tap action. */
static ha_item *entity_item(ctx *c, int card, int kind, int span, int rows)
{
    ha_item *it = new_item(c, kind, span, rows);
    if (!it) return NULL;
    char id[64];
    get_str(c->d, card, "entity", id, sizeof id);
    it->ent = (int16_t)add_entity(c->out, id);
    get_str(c->d, card, "name", it->name, sizeof it->name);
    get_str(c->d, card, "icon", it->icon, sizeof it->icon);
    parse_action(c->d, card, it);
    return it;
}

static void heading(ctx *c, const char *text, int small)
{
    if (!text[0]) return;
    ha_item *it = new_item(c, HI_HEADING, 12, 1);
    if (!it) return;
    str_copy(it->text, sizeof it->text, text);
    it->small = (uint8_t)small;
}

/* Markdown -> plain text: drops heading marks, emphasis and link targets. */
static void strip_markdown(const char *in, char *out, size_t len)
{
    size_t o = 0;
    int line_start = 1;
    for (const char *p = in; *p && o + 1 < len; p++) {
        if (line_start) {
            while (*p == '#' || *p == '>') p++;
            while (*p == ' ') p++;
            if (!*p) break;
        }
        line_start = *p == '\n';
        if (*p == '*' || *p == '_' || *p == '`') continue;
        if (*p == ']' && p[1] == '(') {
            /* [text](url): keep text */
            const char *e = strchr(p, ')');
            if (e) p = e;
            continue;
        }
        if (*p == '[') continue;
        out[o++] = *p;
    }
    out[o] = 0;
}

static void flatten(ctx *c, int card, int span_override, int sections);

/* Children of a stack / grid card, each given `span` columns. */
static void flatten_children(ctx *c, int cards, int span, int sections)
{
    for (int i = 0;; i++) {
        int ch = json_at(c->d, cards, i);
        if (ch < 0) break;
        flatten(c, ch, span, sections);
    }
}

static int count(const json_doc *d, int arr)
{
    int n = 0;
    while (json_at(d, arr, n) >= 0) n++;
    return n;
}

static void flatten(ctx *c, int card, int span_override, int sections)
{
    const json_doc *d = c->d;
    char type[48], title[96];
    get_str(d, card, "type", type, sizeof type);
    get_str(d, card, "title", title, sizeof title);
    /* default width: HA's sections defaults, or full width in masonry views */
    int def = 12;
    if (sections)
        def = is(type, "tile") || is(type, "entity") || is(type, "sensor") || is(type, "gauge") ? 6
              : is(type, "button")                                                              ? 3
                                                                                                : 12;
    int span = span_override ? span_override : card_span(d, card, def);

    if (is(type, "tile")) {
        ha_item *it = entity_item(c, card, HI_TILE, span, card_rows(d, card, 1));
        if (it) it->vertical = (uint8_t)json_bool(d, json_get(d, card, "vertical"), 0);
    }
    else if (is(type, "button") || is(type, "light")) {
        ha_item *it = entity_item(c, card, HI_BUTTON, span, card_rows(d, card, 2));
        if (it && !it->name[0] && it->ent < 0) get_str(d, card, "name", it->name, sizeof it->name);
    }
    else if (is(type, "entity") || is(type, "sensor") || is(type, "gauge") || is(type, "thermostat") ||
             is(type, "media-control") || is(type, "weather-forecast")) {
        entity_item(c, card, HI_SENSOR, span, card_rows(d, card, 2));
    }
    else if (is(type, "heading")) {
        char h[96], style[16];
        get_str(d, card, "heading", h, sizeof h);
        get_str(d, card, "heading_style", style, sizeof style);
        heading(c, h, is(style, "subtitle"));
    }
    else if (is(type, "entities") || is(type, "glance")) {
        heading(c, title, 1);
        int ents = json_get(d, card, "entities");
        int glance = is(type, "glance");
        int cols = glance ? (int)json_int(d, json_get(d, card, "columns"), 3) : 1;
        for (int i = 0;; i++) {
            int e = json_at(d, ents, i);
            if (e < 0) break;
            /* rows are "entity_id" strings or {entity, name, icon, type} objects */
            ha_item *it = NULL;
            if (d->t[e].type == J_STR) {
                char id[64];
                json_str(d, e, id, sizeof id);
                it = new_item(c, glance ? HI_BUTTON : HI_ROW, glance ? 12 / CLAMP(cols, 1, 4) : span, glance ? 2 : 1);
                if (it) it->ent = (int16_t)add_entity(c->out, id);
            }
            else {
                char rtype[24], label[96];
                get_str(d, e, "type", rtype, sizeof rtype);
                if (is(rtype, "section")) {
                    get_str(d, e, "label", label, sizeof label);
                    heading(c, label, 1);
                    continue;
                }
                if (rtype[0]) continue; /* dividers, attribute rows, buttons ... */
                it = entity_item(c, e, glance ? HI_BUTTON : HI_ROW, glance ? 12 / CLAMP(cols, 1, 4) : span,
                                 glance ? 2 : 1);
            }
        }
    }
    else if (is(type, "markdown")) {
        heading(c, title, 1);
        char content[512];
        get_str(d, card, "content", content, sizeof content);
        ha_item *it = new_item(c, HI_TEXT, span, 1);
        if (it) strip_markdown(content, it->text, sizeof it->text);
    }
    else if (is(type, "horizontal-stack")) {
        heading(c, title, 1);
        int cards = json_get(d, card, "cards");
        int n = count(d, cards);
        if (n) flatten_children(c, cards, span / n, sections);
    }
    else if (is(type, "vertical-stack")) {
        heading(c, title, 1);
        flatten_children(c, json_get(d, card, "cards"), span, sections);
    }
    else if (is(type, "grid")) {
        heading(c, title, 1);
        int cols = (int)json_int(d, json_get(d, card, "columns"), 3);
        flatten_children(c, json_get(d, card, "cards"), span / CLAMP(cols, 1, 4), sections);
    }
    else if (is(type, "conditional")) {
        int inner = json_get(d, card, "card");
        if (inner >= 0) flatten(c, inner, span_override, sections);
    }
    else {
        ha_item *it = new_item(c, HI_UNSUPPORTED, span, 1);
        if (it) str_copy(it->text, sizeof it->text, type[0] ? type : "card");
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
    int views = json_get(&d, 0, "views");
    ctx c = {&d, out};
    for (int v = 0;; v++) {
        int view = json_at(&d, views, v);
        if (view < 0) break;
        ha_item *vi = new_item(&c, HI_VIEW, 12, 1);
        if (vi) {
            get_str(&d, view, "title", vi->text, sizeof vi->text);
            if (!vi->text[0]) snprintf(vi->text, sizeof vi->text, "%s", v ? "View" : "Home");
        }
        int sections = json_get(&d, view, "sections");
        if (sections >= 0) {
            /* sections view: each section is a 12-column grid of cards */
            for (int s = 0;; s++) {
                int sec = json_at(&d, sections, s);
                if (sec < 0) break;
                if (s) new_item(&c, HI_BREAK, 12, 1);
                char title[96];
                get_str(&d, sec, "title", title, sizeof title); /* pre-2024.8 sections had titles */
                heading(&c, title, 0);
                flatten_children(&c, json_get(&d, sec, "cards"), 0, 1);
            }
        }
        else {
            flatten_children(&c, json_get(&d, view, "cards"), 0, 0);
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
    memset(out, 0, sizeof *out);
    ctx c = {NULL, out};
    ha_item *vi = new_item(&c, HI_VIEW, 12, 1);
    if (vi) str_copy(vi->text, sizeof vi->text, "Home");
    char buf[1024];
    str_copy(buf, sizeof buf, list);
    for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        ha_item *it = new_item(&c, HI_TILE, 6, 1);
        if (it) it->ent = (int16_t)add_entity(out, tok);
    }
}
