/*
 * The home screens as a vertical stack of pages: the Spotify player on top, then the Home
 * Assistant dashboard pages. Swiping up / down moves between them, with the page
 * following the finger.
 *
 * On these screens a tap fires when the finger lifts (so a swipe that starts on a button
 * doesn't press it). Once a drag is clearly vertical, the current page and its neighbour
 * are rendered once into off-screen frames and then only composited at the finger's
 * offset, which is cheap enough to keep up with the touch events.
 */

#include <stdlib.h>

#include "core/common.h"
#include "ui/internal.h"

#define SLOP        16 /* px of movement before a touch counts as a drag */
#define COMMIT_DIST 90 /* px: a slow drag this far switches the page */
#define FLICK_DIST  30 /* px: ... or a quick flick this far */
#define FLICK_MS    300
#define SETTLE_MS   180 /* animation to the final position after lifting the finger */
#define CONTENT_H   (SCR_H - HEADER_H)

static struct {
    int down;    /* finger is on the screen */
    int swiping; /* the current drag moves pages */
    int hit;     /* tappable id under the finger at touch-down */
    int x0, y0, x, y;
    uint64_t t0;
    int dirty;  /* the drag moved since the last frame */
    int target; /* neighbour page shown in the gap, -1 if none */
} g;

/* off-screen frames: the current page and the neighbour being revealed */
static uint32_t *frame_cur, *frame_next;

int is_home(void) { return ui.screen == SCR_PLAYER || ui.screen == SCR_HASS; }

int pager_count(void) { return 1 + hass_page_count(); }

int pager_current(void) { return ui.screen == SCR_HASS ? 1 + ui.ha_page : 0; }

void pager_show(int page)
{
    if (page <= 0) {
        ui.screen = SCR_PLAYER;
    }
    else {
        ui.screen = SCR_HASS;
        ui.ha_page = page - 1;
    }
    ui.home = ui.screen;
    ui.dirty = 1;
}

int pager_busy(void) { return g.swiping; }

/* Renders page p into dst without showing it (the current page is restored afterwards). */
static void render_page_into(int p, uint32_t *dst)
{
    int screen = ui.screen, ha_page = ui.ha_page, home = ui.home;
    pager_show(p);
    ui_draw();
    memcpy(dst, gfx_buffer(), sizeof(uint32_t) * SCR_W * SCR_H);
    ui.screen = screen;
    ui.ha_page = ha_page;
    ui.home = home;
}

/* Drag offset to display: follows the finger, with resistance past the first / last page. */
static int drag_offset(void)
{
    int dy = g.y - g.y0;
    if (g.target < 0) dy /= 3;
    return CLAMP(dy, -CONTENT_H, CONTENT_H);
}

static void present(int off) { gfx_present_slide(frame_cur, g.target >= 0 ? frame_next : NULL, HEADER_H, off, C_BG); }

/* The finger moved far enough vertically: freeze the current page and render the one being revealed. */
static void begin_swipe(void)
{
    if (!frame_cur) frame_cur = malloc(sizeof(uint32_t) * SCR_W * SCR_H);
    if (!frame_next) frame_next = malloc(sizeof(uint32_t) * SCR_W * SCR_H);
    if (!frame_cur || !frame_next) return;
    g.swiping = 1;
    ui.pressed_id = 0;
    render_page_into(pager_current(), frame_cur);
    g.target = -1;
}

/* Picks (and renders) the neighbour on the side the finger is moving toward. */
static void update_target(void)
{
    int dy = g.y - g.y0, cur = pager_current();
    int want = dy < 0 ? cur + 1 : cur - 1; /* finger moving up reveals the page below */
    if (want < 0 || want >= pager_count()) want = -1;
    if (want != g.target) {
        g.target = want;
        if (want >= 0) render_page_into(want, frame_next);
    }
}

/* Finger lifted mid-swipe: animate to the next page or back, then show it for real. */
static void finish_swipe(void)
{
    int off = drag_offset(), dy = g.y - g.y0;
    uint64_t dt = now_ms() - g.t0;
    int commit = g.target >= 0 && (abs(dy) >= COMMIT_DIST || (abs(dy) >= FLICK_DIST && dt < FLICK_MS));
    int to = commit ? (dy < 0 ? -CONTENT_H : CONTENT_H) : 0;
    uint64_t start = now_ms();
    for (;;) {
        float f = (float)(now_ms() - start) / SETTLE_MS;
        if (f > 1) f = 1;
        float e = 1 - (1 - f) * (1 - f) * (1 - f); /* ease out */
        present(off + (int)((float)(to - off) * e));
        if (f >= 1) break;
    }
    if (commit) pager_show(g.target);
    g.swiping = 0;
    ui_render();
}

void pager_touch(const touch_event *e)
{
    if (e->type == TOUCH_DOWN) {
        if (now_ms() - ui.last_up_ms < 60) return; /* bounce */
        g.down = 1;
        g.swiping = 0;
        g.x0 = g.x = e->x;
        g.y0 = g.y = e->y;
        g.t0 = now_ms();
        g.hit = hit_at(e->x, e->y);
        /* highlight what's under the finger while it's down */
        ui.pressed_id = g.hit;
        ui.pressed_until = now_ms() + 10000;
        ui.dirty = 1;
        return;
    }
    if (!g.down) return;
    if (e->type == TOUCH_MOVE) {
        g.x = e->x;
        g.y = e->y;
        int dx = g.x - g.x0, dy = g.y - g.y0;
        if (!g.swiping && abs(dy) > SLOP && abs(dy) > abs(dx)) begin_swipe();
        if (!g.swiping && abs(dx) > SLOP) {
            /* sideways: not a tap and not a page swipe */
            g.hit = 0;
            ui.pressed_id = 0;
            ui.dirty = 1;
        }
        if (g.swiping) {
            update_target();
            g.dirty = 1;
        }
        return;
    }
    if (e->type == TOUCH_UP) {
        /* (synthesized lifts carry no position: keep the last one) */
        ui.last_up_ms = now_ms();
        g.down = 0;
        if (g.swiping) {
            finish_swipe();
            return;
        }
        if (g.hit && abs(g.x - g.x0) <= SLOP && abs(g.y - g.y0) <= SLOP) {
            ui.pressed_until = now_ms() + 160; /* keep the highlight a moment */
            ui_press(g.hit);
        }
        else {
            ui.pressed_id = 0;
        }
        ui.dirty = 1;
    }
}

void pager_update(void)
{
    if (!g.swiping || !g.dirty) return;
    g.dirty = 0;
    present(drag_offset());
}
