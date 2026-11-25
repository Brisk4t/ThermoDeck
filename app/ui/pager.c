/*
 * The home screens as a vertical stack of pages: the Spotify player on top, then the Home
 * Assistant dashboard pages. Swiping up / down moves between them, with the page
 * following the finger.
 *
 * On these screens a tap fires when the finger lifts (so a swipe that starts on a button
 * doesn't press it). Once a drag is clearly vertical, the current page and its neighbour
 * are rendered once into off-screen frames and then only composited at the finger's
 * offset, which is cheap enough to keep up with the touch events.
 *
 * Whether a lift switches pages depends on the drag distance and the finger's speed just
 * before the lift, both taken from the touch controller's timestamps, so a busy UI loop
 * that handles the events late still sees the gesture as it was made.
 */

#include <stdlib.h>

#include "core/common.h"
#include "core/config.h"
#include "ui/internal.h"

#define SLOP        16  /* px of movement before a touch counts as a drag */
#define COMMIT_DIST 80  /* px: a drag this far switches the page */
#define FLICK_DIST  20  /* px: ... or a flick this far */
#define FLICK_SPEED 300 /* px/s at the lift that counts as a flick */
#define VEL_WINDOW  100 /* ms of finger movement the speed is measured over */
#define SETTLE_MS   200 /* animation over a whole page after the lift (shorter for less) */
#define CONTENT_H   (SCR_H - HEADER_H)
#define N_SAMPLES   16

static struct {
    int down;    /* finger is on the screen */
    int swiping; /* the current drag moves pages */
    int hit;     /* tappable id under the finger at touch-down */
    int x0, y0, x, y;
    int ys;     /* finger y at which the page is at rest (where the swipe started) */
    int dirty;  /* the drag moved since the last frame */
    int target; /* neighbour page shown in the gap, -1 if none, -2 not picked yet */
    struct {
        uint32_t ms;
        int y;
    } s[N_SAMPLES]; /* recent finger positions, for the speed at the lift */
    int n_s;
    uint64_t render_ms; /* time spent rendering pages for this swipe (debug log) */
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
    uint64_t t = now_ms();
    int screen = ui.screen, ha_page = ui.ha_page, home = ui.home;
    pager_show(p);
    ui_draw();
    memcpy(dst, gfx_buffer(), sizeof(uint32_t) * SCR_W * SCR_H);
    ui.screen = screen;
    ui.ha_page = ha_page;
    ui.home = home;
    g.render_ms += now_ms() - t;
}

/* Drag offset to display: follows the finger, with resistance past the first / last page. */
static int drag_offset(void)
{
    int dy = g.y - g.ys;
    if (g.target < 0) dy /= 3;
    return CLAMP(dy, -CONTENT_H, CONTENT_H);
}

static void present(int off) { gfx_present_slide(frame_cur, g.target >= 0 ? frame_next : NULL, HEADER_H, off, C_BG); }

/* Remembers where the finger was at event time ms, keeping the last N_SAMPLES. */
static void add_sample(uint32_t ms, int y)
{
    if (g.n_s == N_SAMPLES) {
        memmove(&g.s[0], &g.s[1], sizeof g.s[0] * (N_SAMPLES - 1));
        g.n_s--;
    }
    g.s[g.n_s].ms = ms;
    g.s[g.n_s].y = y;
    g.n_s++;
}

/* Finger speed over the last VEL_WINDOW ms in px/s (positive = moving down). A finger that
 * stopped before lifting has speed 0. */
static int lift_speed(void)
{
    if (g.n_s < 2) return 0;
    int last = g.n_s - 1, k = last;
    while (k > 0 && g.s[last].ms - g.s[k - 1].ms <= VEL_WINDOW) k--;
    uint32_t dt = g.s[last].ms - g.s[k].ms;
    if (dt < 10) return 0;
    return (g.s[last].y - g.s[k].y) * 1000 / (int)dt;
}

/* The finger moved far enough vertically: freeze the current page; update_target renders the
 * one being revealed. */
static void begin_swipe(void)
{
    if (!frame_cur) frame_cur = malloc(sizeof(uint32_t) * SCR_W * SCR_H);
    if (!frame_next) frame_next = malloc(sizeof(uint32_t) * SCR_W * SCR_H);
    if (!frame_cur || !frame_next) return;
    g.swiping = 1;
    g.render_ms = 0;
    /* the page starts moving from here rather than jumping by the slop */
    g.ys = g.y0 + (g.y > g.y0 ? SLOP : -SLOP);
    ui.pressed_id = 0;
    render_page_into(pager_current(), frame_cur);
    g.target = -2;
}

/* Picks (and renders) the neighbour on the side the finger is moving toward. */
static void update_target(void)
{
    int dy = g.y - g.ys, cur = pager_current();
    if (dy == 0 && g.target != -2) return;
    int want = dy < 0 ? cur + 1 : cur - 1; /* finger moving up reveals the page below */
    if (want < 0 || want >= pager_count()) want = -1;
    if (want != g.target) {
        g.target = want;
        if (want >= 0) render_page_into(want, frame_next);
    }
}

/* Finger lifted mid-swipe: a long enough drag, or a flick, moves on to the neighbour (unless
 * the finger was heading back at the end). Animates there or back, then shows it for real. */
static void finish_swipe(void)
{
    int off = drag_offset(), dy = g.y - g.ys, v = lift_speed();
    int toward = (dy < 0 && v < 0) || (dy > 0 && v > 0);
    int fast = abs(v) >= FLICK_SPEED;
    int commit =
        g.target >= 0 && ((abs(dy) >= COMMIT_DIST && (toward || !fast)) || (abs(dy) >= FLICK_DIST && toward && fast));
    int to = commit ? (dy < 0 ? -CONTENT_H : CONTENT_H) : 0;
    if (config_int("touch", "debug", 0))
        tdlog("pager: drag %d px, %d px/s at the lift -> %s (rendering took %llu ms)", dy, v,
              commit ? "next page" : "stays", (unsigned long long)g.render_ms);
    /* ease out to the final position */
    uint64_t start = now_ms(), dur = 60 + (uint64_t)(SETTLE_MS - 60) * (uint64_t)abs(to - off) / CONTENT_H;
    for (;;) {
        float f = (float)(now_ms() - start) / (float)dur;
        if (f > 1) f = 1;
        float e = 1 - (1 - f) * (1 - f) * (1 - f);
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
        /* (contact bounces are merged into one touch by the touch driver) */
        g.down = 1;
        g.swiping = 0;
        g.x0 = g.x = e->x;
        g.y0 = g.y = e->y;
        g.n_s = 0;
        add_sample(e->ms, e->y);
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
        add_sample(e->ms, e->y);
        int dx = g.x - g.x0, dy = g.y - g.y0;
        /* up to ~60 degrees off vertical counts: nothing on these screens moves sideways */
        if (!g.swiping && abs(dy) > SLOP && 2 * abs(dy) > abs(dx)) begin_swipe();
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
            add_sample(e->ms, g.y); /* a finger that rested before lifting has no speed */
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
