/*
 * Home Assistant pages: the dashboard defined in HA, laid out on a 12-column grid like
 * HA's sections view and split into screen-sized pages (swipe to move between them).
 *
 * Layout runs only when the dashboard changes (g_ha.layout_ver); each repaint then draws
 * the boxes of the current page with the latest entity states.
 */

#include "cards/cards.h"
#include "core/common.h"
#include "gfx/icons.h"
#include "hass/hass.h"
#include "ui/internal.h"

/* grid geometry: 12 columns of 19 px with 6 px gaps (x 8..302; the page dots sit right of it) */
#define GRID_X    8
#define COL_W     19
#define GAP       CARD_HGAP  /* between columns */
#define VGAP      CARD_VGAP  /* between rows */
#define ROW_H     CARD_ROW_H /* one grid row */
#define PAGE_TOP  (HEADER_H + 8)
#define PAGE_BOT  (SCR_H - 8)
#define SPAN_W(s) ((s) * (COL_W + GAP) - GAP)

typedef struct {
    int16_t item; /* dashboard item, or BOX_OVERFLOW */
    int16_t x, y, w, h;
    uint8_t page;
} box;

#define BOX_OVERFLOW (-1) /* "section too tall" note at the top of a continuation page */
#define OVERFLOW_H   22
#define MAX_BOXES    (HA_MAX_ITEMS + 32)

static box boxes[MAX_BOXES];
static int n_boxes, n_pages = 1;
static uint32_t laid_out_ver = (uint32_t)-1;

/* ---------- layout ---------- */

/* Height in px of a card laid out w px wide: its own height(), else its grid rows. */
static int item_height(const ha_item *it, int w)
{
    const card_def *def = card_get(it->card);
    if (def->height) return def->height(it, w);
    return CARD_ROWS_H(it->rows);
}

/* layout state: the page being filled and the row ("shelf") being built on it */
static struct {
    const ha_dash *d;
    int page, y;
    int paged; /* current view puts one section per page: overflowing is worth a note */
    box shelf[12];
    int n_shelf, col, shelf_h;
} L;

static void add_box(box b)
{
    if (n_boxes < MAX_BOXES) boxes[n_boxes++] = b;
}

static void new_page(void)
{
    L.page++;
    L.y = PAGE_TOP;
}

/* A row holding only a heading-like card (which shouldn't end a page on its own). */
static int shelf_is_heading(void)
{
    if (L.n_shelf != 1) return 0;
    const ha_item *it = &L.d->items[L.shelf[0].item];
    return it->kind == HI_VIEW || (card_get(it->card)->flags & CARD_OWN_ROW);
}

/* Places the finished row below the previous one, moving to a new page if it doesn't fit. */
static void flush_shelf(void)
{
    if (L.n_shelf) {
        int keep = shelf_is_heading() ? ROW_H + VGAP : 0; /* keep a heading with the row after it */
        if (L.y + L.shelf_h + keep > PAGE_BOT && L.y > PAGE_TOP) {
            new_page();
            if (L.paged) {
                /* the section didn't fit its page: say so where it continues */
                add_box((box){BOX_OVERFLOW, GRID_X, (int16_t)L.y, SPAN_W(12), OVERFLOW_H, (uint8_t)L.page});
                L.y += OVERFLOW_H + VGAP;
            }
        }
        for (int k = 0; k < L.n_shelf; k++) {
            L.shelf[k].y = (int16_t)L.y;
            L.shelf[k].page = (uint8_t)L.page;
            add_box(L.shelf[k]);
        }
        L.y += L.shelf_h + VGAP;
    }
    L.n_shelf = L.col = L.shelf_h = 0;
}

static void add_to_shelf(int item, int span, int h)
{
    if (L.col + span > 12) flush_shelf();
    if (L.n_shelf < 12)
        L.shelf[L.n_shelf++] =
            (box){(int16_t)item, (int16_t)(GRID_X + L.col * (COL_W + GAP)), 0, (int16_t)SPAN_W(span), (int16_t)h, 0};
    L.col += span;
    if (h > L.shelf_h) L.shelf_h = h;
}

/*
 * Flows the items into rows left to right; a row that doesn't fit below the previous one
 * starts a new page. Each view starts a page, and in sections views so does each section.
 */
static void layout(const ha_dash *d)
{
    memset(&L, 0, sizeof L);
    L.d = d;
    L.y = PAGE_TOP;
    n_boxes = 0;
    int views = 0, first_view = 1;
    for (int i = 0; i < d->n_items; i++) views += d->items[i].kind == HI_VIEW;

    for (int i = 0; i < d->n_items; i++) {
        const ha_item *it = &d->items[i];
        switch (it->kind) {
        case HI_VIEW:
            flush_shelf();
            if (!first_view && L.y > PAGE_TOP) new_page();
            first_view = 0;
            L.paged = it->paged;
            /* view titles only where pages flow freely: a section page keeps its full height */
            if (views > 1 && !it->paged) {
                add_to_shelf(i, 12, 34);
                flush_shelf();
            }
            break;
        case HI_PAGE:
            flush_shelf();
            if (L.y > PAGE_TOP) new_page();
            break;
        case HI_BREAK:
            flush_shelf();
            L.y += VGAP;
            break;
        default: {
            int own_row = card_get(it->card)->flags & CARD_OWN_ROW;
            int span = own_row ? 12 : it->span;
            if (own_row) flush_shelf();
            add_to_shelf(i, span, item_height(it, SPAN_W(span)));
            if (L.col >= 12 || own_row) flush_shelf();
        }
        }
    }
    flush_shelf();
    n_pages = L.page + 1;
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
        if (b->item == BOX_OVERFLOW) {
            icon_draw(&icons_24, icon_find("alert"), b->x + 12, b->y + b->h / 2, C_YELLOW);
            gfx_text_fit(&font_sm, b->x + 30, b->y + 16, b->w - 30, "Continued: section too tall", C_YELLOW);
            continue;
        }
        const ha_item *it = &g_ha.d.items[b->item];
        if (it->kind == HI_VIEW) {
            gfx_text_fit(&font_lg, b->x + 4, b->y + 26, b->w - 8, it->text, C_TEXT);
            continue;
        }
        const card_def *def = card_get(it->card);
        card_ctx c = {b->x,        b->y,
                      b->w,        b->h,
                      it,          it->ent >= 0 ? &g_ha.d.ents[it->ent] : NULL,
                      &g_ha.d,     is_pressed(BTN_HA0 + b->item),
                      ui.clock_24h};
        def->draw(&c);
        /* tappable: cards that act on taps, tied to an entity or an explicit action */
        if ((def->flags & CARD_TAPPABLE) && (c.e || it->act == HA_ACT_CALL) && it->act != HA_ACT_NONE)
            add_hit(BTN_HA0 + b->item, b->x, b->y, b->w, b->h);
    }
    pthread_mutex_unlock(&g_ha.mu);
    draw_toast(430);
}

void press_hass(int id)
{
    if (id >= BTN_HA0) hass_press(id - BTN_HA0);
}
