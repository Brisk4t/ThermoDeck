/*
 * Home Assistant pages: the dashboard defined in HA, laid out on a 12-column grid like
 * HA's sections view and split into screen-sized pages (swipe to move between them).
 *
 * Layout runs only when the dashboard changes (g_ha.layout_ver); each repaint then draws
 * the boxes of the current page with the latest entity states.
 */

#include "core/common.h"
#include "gfx/icons.h"
#include "hass/hass.h"
#include "ui/internal.h"

/* grid geometry: 12 columns of 19 px with 6 px gaps (x 8..302; the page dots sit right of it) */
#define GRID_X    8
#define COL_W     19
#define GAP       6
#define ROW_H     56 /* one grid row, as in HA */
#define PAGE_TOP  (HEADER_H + 8)
#define PAGE_BOT  (SCR_H - 8)
#define SPAN_W(s) ((s) * (COL_W + GAP) - GAP)
#define C_ICON_BG 0x2C2C2C
#define C_UNAVAIL 0x5A5A5A

typedef struct {
    int16_t item, x, y, w, h;
    uint8_t page;
} box;

static box boxes[HA_MAX_ITEMS];
static int n_boxes, n_pages = 1;
static uint32_t laid_out_ver = (uint32_t)-1;

/* ---------- layout ---------- */

/* Height in px of an item laid out w px wide. */
static int item_height(const ha_item *it, int w)
{
    switch (it->kind) {
    case HI_HEADING: return it->small ? 26 : 34;
    case HI_ROW: return 48;
    case HI_TEXT: return 18 * gfx_text_wrap(&font_sm, 0, 0, w - 24, 12, 18, it->text, 0, 0, 0) + 22;
    case HI_UNSUPPORTED: return 44;
    default: return it->rows * ROW_H + (it->rows - 1) * GAP;
    }
}

/* A row holding only a heading (which shouldn't end a page on its own). */
static int shelf_is_heading(const ha_dash *d, const box *shelf, int n)
{
    return n == 1 && (d->items[shelf[0].item].kind == HI_HEADING || d->items[shelf[0].item].kind == HI_VIEW);
}

/*
 * Flows the items into rows ("shelves") left to right; a row that doesn't fit below the
 * previous one starts a new page, and so does each dashboard view.
 */
static void layout(const ha_dash *d)
{
    n_boxes = 0;
    n_pages = 1;
    int views = 0;
    for (int i = 0; i < d->n_items; i++) views += d->items[i].kind == HI_VIEW;

    int page = 0, y = PAGE_TOP, col = 0, shelf_h = 0, first_view = 1;
    box shelf[12]; /* boxes of the row being filled (placed once the row is done) */
    int n_shelf = 0;

#define FLUSH_SHELF()                                                                                                  \
    do {                                                                                                               \
        if (n_shelf) {                                                                                                 \
            int keep = shelf_is_heading(d, shelf, n_shelf) ? ROW_H + GAP : 0; /* heading + one row */                  \
            if (y + shelf_h + keep > PAGE_BOT && y > PAGE_TOP) {                                                       \
                page++;                                                                                                \
                y = PAGE_TOP;                                                                                          \
            }                                                                                                          \
            for (int k = 0; k < n_shelf; k++) {                                                                        \
                shelf[k].y = (int16_t)y;                                                                               \
                shelf[k].page = (uint8_t)page;                                                                         \
                boxes[n_boxes++] = shelf[k];                                                                           \
            }                                                                                                          \
            y += shelf_h + GAP;                                                                                        \
        }                                                                                                              \
        n_shelf = 0;                                                                                                   \
        col = 0;                                                                                                       \
        shelf_h = 0;                                                                                                   \
    } while (0)

    for (int i = 0; i < d->n_items; i++) {
        const ha_item *it = &d->items[i];
        if (it->kind == HI_VIEW) {
            /* each view starts a page, titled when there are several */
            FLUSH_SHELF();
            if (!first_view && y > PAGE_TOP) {
                page++;
                y = PAGE_TOP;
            }
            first_view = 0;
            if (views > 1) {
                shelf[n_shelf++] = (box){(int16_t)i, GRID_X, 0, SPAN_W(12), 34, 0};
                shelf_h = 34;
                FLUSH_SHELF();
            }
            continue;
        }
        if (it->kind == HI_BREAK) {
            FLUSH_SHELF();
            y += GAP;
            continue;
        }
        int span = it->kind == HI_HEADING ? 12 : it->span;
        if (col + span > 12) FLUSH_SHELF();
        int w = SPAN_W(span), h = item_height(it, w);
        if (n_shelf < 12)
            shelf[n_shelf++] = (box){(int16_t)i, (int16_t)(GRID_X + col * (COL_W + GAP)), 0, (int16_t)w, (int16_t)h, 0};
        col += span;
        if (h > shelf_h) shelf_h = h;
        if (col >= 12 || it->kind == HI_HEADING) FLUSH_SHELF();
    }
    FLUSH_SHELF();
#undef FLUSH_SHELF
    n_pages = page + 1;
}

int hass_page_count(void)
{
    pthread_mutex_lock(&g_ha.mu);
    int ready = g_ha.status == HA_READY;
    if (ready && laid_out_ver != g_ha.layout_ver) {
        layout(&g_ha.d);
        laid_out_ver = g_ha.layout_ver;
    }
    pthread_mutex_unlock(&g_ha.mu);
    return ready ? n_pages : 1;
}

/* ---------- drawing ---------- */

/* c blended over the card color at alpha a/255 (for tinted icon backgrounds). */
static uint32_t tint(uint32_t c, int a)
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int fg = (int)((c >> sh) & 255), bg = (C_CARD >> sh) & 255;
        r |= (uint32_t)((bg * (255 - a) + fg * a) / 255) << sh;
    }
    return r;
}

static const char *item_name(const ha_item *it, const ha_entity *e)
{
    if (it->name[0]) return it->name;
    if (e) return e->name[0] ? e->name : e->id;
    return "";
}

/* The item's icon: its own override, else the entity's icon attribute, else the domain
 * default (also when a custom icon isn't compiled in). */
static int resolve_icon(const ha_item *it, const ha_entity *e)
{
    int ic = icon_find(it->icon);
    if (ic >= 0) return ic;
    if (e) {
        ic = icon_find(ha_entity_icon(e));
        if (ic < 0) {
            /* custom icon that isn't compiled in: use the domain default instead */
            ha_entity plain = *e;
            plain.icon[0] = 0;
            ic = icon_find(ha_entity_icon(&plain));
        }
    }
    return ic >= 0 ? ic : icon_find("bookmark");
}

static int unavailable(const ha_entity *e) { return e && (!strcmp(e->state, "unavailable")); }

/* Rounded on/off switch, 40x22, right edge at rx. */
static void draw_switch(int rx, int cy, int on, uint32_t color)
{
    int x = rx - 40;
    gfx_rrect(x, cy - 11, 40, 22, 11, on ? color : 0x4A4A4A);
    gfx_circle(on ? x + 29 : x + 11, cy, 8, on ? 0xFFFFFF : 0xBDBDBD);
}

static void draw_tile(const box *b, const ha_item *it, const ha_entity *e, int pressed)
{
    int on = e && ha_entity_is_on(e), na = unavailable(e);
    uint32_t color = e ? ha_entity_color(e) : C_GREEN;
    gfx_rrect(b->x, b->y, b->w, b->h, 12, pressed ? C_BTN_PRESSED : C_CARD);
    char st[64] = "";
    if (e) ha_entity_state_text(e, st, sizeof st);
    /* like HA, the content is centered vertically, however many rows the tile has */
    int cy = b->y + b->h / 2;
    if (it->vertical) {
        /* icon above, name and state centered below it */
        int cx = b->x + b->w / 2, iy = cy - 22;
        gfx_circle(cx, iy, 18, on ? tint(color, 60) : C_ICON_BG);
        icon_draw(&icons_24, resolve_icon(it, e), cx, iy, na ? C_UNAVAIL : on ? color : C_SUB);
        const char *name = item_name(it, e);
        int w = b->w - 16;
        if (gfx_text_width(&font_sm, name) > w)
            gfx_text_fit(&font_sm, b->x + 8, cy + 16, w, name, na ? C_UNAVAIL : C_TEXT);
        else
            gfx_text_center(&font_sm, cx, cy + 16, name, na ? C_UNAVAIL : C_TEXT);
        if (gfx_text_width(&font_sm, st) > w)
            gfx_text_fit(&font_sm, b->x + 8, cy + 34, w, st, C_SUB);
        else
            gfx_text_center(&font_sm, cx, cy + 34, st, C_SUB);
        return;
    }
    gfx_circle(b->x + 28, cy, 18, on ? tint(color, 60) : C_ICON_BG);
    icon_draw(&icons_24, resolve_icon(it, e), b->x + 28, cy, na ? C_UNAVAIL : on ? color : C_SUB);
    int tx = b->x + 54, tw = b->w - 54 - 8;
    gfx_text_fit(&font_sm, tx, cy - 3, tw, item_name(it, e), na ? C_UNAVAIL : C_TEXT);
    gfx_text_fit(&font_sm, tx, cy + 15, tw, st, C_SUB);
}

static void draw_button(const box *b, const ha_item *it, const ha_entity *e, int pressed)
{
    int on = e && ha_entity_is_on(e), na = unavailable(e);
    gfx_rrect(b->x, b->y, b->w, b->h, 12, pressed ? C_BTN_PRESSED : C_CARD);
    int cx = b->x + b->w / 2;
    const font_t *set = b->h >= 100 && b->w >= 60 ? &icons_36 : &icons_24;
    uint32_t ic = na ? C_UNAVAIL : on ? ha_entity_color(e) : e ? C_SUB : C_TEXT;
    icon_draw(set, resolve_icon(it, e), cx, b->y + b->h / 2 - 10, ic);
    const char *name = item_name(it, e);
    int nw = gfx_text_width(&font_sm, name);
    if (nw > b->w - 12)
        gfx_text_fit(&font_sm, b->x + 6, b->y + b->h - 14, b->w - 12, name, na ? C_UNAVAIL : C_TEXT);
    else
        gfx_text_center(&font_sm, cx, b->y + b->h - 14, name, na ? C_UNAVAIL : C_TEXT);
}

/* Entity / sensor card: name and icon on top, the value large underneath. */
static void draw_sensor(const box *b, const ha_item *it, const ha_entity *e, int pressed)
{
    int on = e && ha_entity_is_on(e);
    gfx_rrect(b->x, b->y, b->w, b->h, 12, pressed ? C_BTN_PRESSED : C_CARD);
    gfx_text_fit(&font_sm, b->x + 14, b->y + 26, b->w - 52, item_name(it, e), C_SUB);
    icon_draw(&icons_24, resolve_icon(it, e), b->x + b->w - 24, b->y + 22, on ? ha_entity_color(e) : C_DIM);
    if (!e) return;
    int by = b->y + b->h - 18;
    char num[40];
    if (e->unit[0] && e->has_state && strcmp(e->state, "unavailable")) {
        /* number large, unit small after it */
        ha_entity plain = *e;
        plain.unit[0] = 0;
        ha_entity_state_text(&plain, num, sizeof num);
        const font_t *f = gfx_text_width(&font_xl, num) + 40 < b->w - 28 ? &font_xl : &font_lg;
        int w = gfx_text(f, b->x + 14, by, num, C_TEXT);
        gfx_text_fit(&font_md, b->x + 14 + w + 4, by, b->w - 32 - w, e->unit, C_SUB);
    }
    else {
        ha_entity_state_text(e, num, sizeof num);
        const font_t *f = gfx_text_width(&font_lg, num) < b->w - 28 ? &font_lg : &font_md;
        gfx_text_fit(f, b->x + 14, by, b->w - 28, num, C_TEXT);
    }
}

/* A row of an entities card: icon, name, and a switch or the state on the right. */
static void draw_row(const box *b, const ha_item *it, const ha_entity *e, int pressed)
{
    int on = e && ha_entity_is_on(e), na = unavailable(e);
    gfx_rrect(b->x, b->y, b->w, b->h, 10, pressed ? C_BTN_PRESSED : C_CARD);
    int cy = b->y + b->h / 2;
    uint32_t color = e ? ha_entity_color(e) : C_TEXT;
    icon_draw(&icons_24, resolve_icon(it, e), b->x + 26, cy, na ? C_UNAVAIL : on ? color : C_SUB);
    int right = b->x + b->w - 12, name_w;
    if (e && ha_entity_toggles(e) && !na && e->has_state) {
        draw_switch(right, cy, on, color);
        name_w = right - 48 - (b->x + 52);
    }
    else {
        char st[64] = "";
        if (e) ha_entity_state_text(e, st, sizeof st);
        int sw = gfx_text_width(&font_sm, st);
        if (sw > b->w / 2) sw = b->w / 2;
        gfx_text_fit(&font_sm, right - sw, cy + 5, sw, st, C_SUB);
        name_w = right - sw - 12 - (b->x + 52);
    }
    gfx_text_fit(&font_md, b->x + 52, cy + 6, name_w, item_name(it, e), na ? C_UNAVAIL : C_TEXT);
}

static void draw_item(const box *b, const ha_item *it, const ha_entity *e, int pressed)
{
    switch (it->kind) {
    case HI_VIEW: gfx_text_fit(&font_lg, b->x + 4, b->y + 26, b->w - 8, it->text, C_TEXT); break;
    case HI_HEADING:
        if (it->small)
            gfx_text_fit(&font_sm, b->x + 4, b->y + 19, b->w - 8, it->text, C_SUB);
        else
            gfx_text_fit(&font_mdb, b->x + 4, b->y + 26, b->w - 8, it->text, C_TEXT);
        break;
    case HI_TILE: draw_tile(b, it, e, pressed); break;
    case HI_BUTTON: draw_button(b, it, e, pressed); break;
    case HI_SENSOR: draw_sensor(b, it, e, pressed); break;
    case HI_ROW: draw_row(b, it, e, pressed); break;
    case HI_TEXT:
        gfx_rrect(b->x, b->y, b->w, b->h, 12, C_CARD);
        gfx_text_wrap(&font_sm, b->x + 12, b->y + 24, b->w - 24, 12, 18, it->text, C_TEXT, 0, 1);
        break;
    default: {
        /* a card type we can't draw: say which, so it's clear why it's missing */
        char msg[96];
        snprintf(msg, sizeof msg, "%s card", it->text);
        gfx_rrect(b->x, b->y, b->w, b->h, 12, 0x181818);
        gfx_text_fit(&font_sm, b->x + 12, b->y + 27, b->w - 24, msg, C_DIM);
        break;
    }
    }
}

/* Full-page message for the states without a dashboard. */
static void draw_status(ha_status st, const char *msg)
{
    static const char *const TITLES[] = {"Set up Home Assistant", "Waiting for Wi-Fi", "Connecting...",
                                         "Token rejected",        "No dashboard",      "",
                                         "Can't reach HA"};
    int cx = SCR_W / 2;
    icon_draw(&icons_36, icon_find("home-assistant"), cx, 150,
              st == HA_ERROR || st == HA_AUTH_FAILED ? C_RED : 0x41BDF5);
    const font_t *tf = gfx_text_width(&font_lg, TITLES[st]) <= SCR_W - 24 ? &font_lg : &font_mdb;
    gfx_text_center(tf, cx, 210, TITLES[st], C_TEXT);
    gfx_text_wrap(&font_sm, 24, 240, SCR_W - 48, 4, 18, msg, C_SUB, 1, 1);
    const char *hint = NULL;
    if (st == HA_OFF || st == HA_AUTH_FAILED)
        hint = "Create a long-lived access token in Home Assistant: your profile > Security.";
    else if (st == HA_NO_DASHBOARD)
        hint = "In Home Assistant, add a dashboard (Settings > Dashboards) named ThermoDeck, "
               "or set dashboard = <url> under [homeassistant]. Edits there show up here.";
    if (hint) gfx_text_wrap(&font_sm, 24, 330, SCR_W - 48, 5, 18, hint, C_DIM, 1, 1);
}

void render_hass(void)
{
    draw_home_header();
    int pages = hass_page_count(); /* (re)does the layout if the dashboard changed */
    pthread_mutex_lock(&g_ha.mu);
    if (g_ha.status != HA_READY) {
        ha_status st = g_ha.status;
        char msg[160];
        str_copy(msg, sizeof msg, g_ha.msg);
        pthread_mutex_unlock(&g_ha.mu);
        ui.ha_page = 0;
        draw_status(st, msg);
        draw_toast(420);
        return;
    }
    if (ui.ha_page >= pages) ui.ha_page = pages - 1;
    for (int i = 0; i < n_boxes; i++) {
        const box *b = &boxes[i];
        if (b->page != ui.ha_page) continue;
        const ha_item *it = &g_ha.d.items[b->item];
        const ha_entity *e = it->ent >= 0 ? &g_ha.d.ents[it->ent] : NULL;
        draw_item(b, it, e, is_pressed(BTN_HA0 + b->item));
        /* tappable: anything tied to an entity or an explicit action */
        int tappable =
            (e || it->act == HA_ACT_CALL) && it->act != HA_ACT_NONE && it->kind >= HI_TILE && it->kind <= HI_ROW;
        if (tappable) add_hit(BTN_HA0 + b->item, b->x, b->y, b->w, b->h);
    }
    pthread_mutex_unlock(&g_ha.mu);
    draw_toast(430);
}

void press_hass(int id)
{
    if (id >= BTN_HA0) hass_press(id - BTN_HA0);
}
