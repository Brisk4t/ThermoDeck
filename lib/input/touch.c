/*
 * Touchscreen input via evdev. The ecobee's Cypress controller speaks multi-touch
 * protocol A (contacts separated by SYN_MT_REPORT, frames ended by SYN_REPORT); plain
 * single-touch ABS_X/ABS_Y devices work too. Only the first contact is tracked, and raw
 * coordinates are scaled to screen pixels.
 */

#include "input/touch.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/common.h"
#include "gfx/gfx.h"

#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#endif
#ifndef SYN_MT_REPORT
#define SYN_MT_REPORT 2
#endif

/* device state: raw coordinate range and which protocol it uses */
static int fd = -1;
static touch_opts opt;
static int minx, maxx = SCR_W - 1, miny, maxy = SCR_H - 1;
static int use_mt = 1;

/* per-frame accumulation */
static int f_x = -1, f_y = -1, f_contacts, f_have_contact, f_btn = -1, f_zero;
static int cur_x = -1, cur_y = -1; /* position of the current (first) contact */
static int down;
static int seen_explicit_release;
static uint64_t last_event_ms;

int touch_fd(void) { return fd; }

/* Opens the device and reads its coordinate range (multi-touch axes preferred). */
int touch_open(const char *dev, const touch_opts *o)
{
    opt = *o;
    fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        tdlog("touch: open %s failed: %s", dev, strerror(errno));
        return -1;
    }
    struct input_absinfo a;
    if (!ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &a) && a.maximum > a.minimum) {
        minx = a.minimum;
        maxx = a.maximum;
        if (!ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) && a.maximum > a.minimum) {
            miny = a.minimum;
            maxy = a.maximum;
        }
        use_mt = 1;
    }
    else if (!ioctl(fd, EVIOCGABS(ABS_X), &a) && a.maximum > a.minimum) {
        minx = a.minimum;
        maxx = a.maximum;
        if (!ioctl(fd, EVIOCGABS(ABS_Y), &a) && a.maximum > a.minimum) {
            miny = a.minimum;
            maxy = a.maximum;
        }
        use_mt = 0;
    }
    tdlog("touch: %s mt=%d x[%d..%d] y[%d..%d] swap=%d inv=%d,%d", dev, use_mt, minx, maxx, miny, maxy, opt.swap_xy,
          opt.invert_x, opt.invert_y);
    return fd;
}

/* Raw device coordinates -> screen pixels, applying swap/invert options. */
static void map(int rx, int ry, int *sx, int *sy)
{
    int ax = rx, ay = ry;
    int aminx = minx, amaxx = maxx, aminy = miny, amaxy = maxy;
    if (opt.swap_xy) {
        ax = ry;
        ay = rx;
        aminx = miny;
        amaxx = maxy;
        aminy = minx;
        amaxy = maxx;
    }
    int x = (int)((long)(ax - aminx) * (SCR_W - 1) / (amaxx - aminx));
    int y = (int)((long)(ay - aminy) * (SCR_H - 1) / (amaxy - aminy));
    if (opt.invert_x) x = SCR_W - 1 - x;
    if (opt.invert_y) y = SCR_H - 1 - y;
    *sx = CLAMP(x, 0, SCR_W - 1);
    *sy = CLAMP(y, 0, SCR_H - 1);
}

/* Appends one event to the caller's buffer (if there's room). */
static int emit(touch_event *ev, int *n, int max, int type, int x, int y)
{
    if (*n >= max) return 0;
    ev[*n].type = type;
    ev[*n].x = x;
    ev[*n].y = y;
    (*n)++;
    if (opt.debug) tdlog("touch: %s %d,%d", type == TOUCH_DOWN ? "down" : type == TOUCH_UP ? "up" : "move", x, y);
    return 1;
}

static void end_contact(void)
{
    /* SYN_MT_REPORT: close the current contact record */
    if (f_x >= 0 && f_y >= 0 && !f_zero) {
        if (!f_have_contact) {
            cur_x = f_x;
            cur_y = f_y;
        }
        f_have_contact = 1;
        f_contacts++;
    }
    f_zero = 0;
}

/* Drains pending input events and turns each completed frame into DOWN / MOVE / UP. */
int touch_read(touch_event *ev, int max)
{
    struct input_event ie[32];
    int n = 0;
    for (;;) {
        ssize_t r = read(fd, ie, sizeof ie);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        int cnt = (int)(r / (ssize_t)sizeof ie[0]);
        for (int i = 0; i < cnt; i++) {
            struct input_event *e = &ie[i];
            last_event_ms = now_ms();
            /* collect this frame's position; zero pressure / tracking id -1 mean "lifted" */
            if (e->type == EV_ABS) {
                if (opt.debug > 1) tdlog("touch raw: abs 0x%02x=%d", e->code, e->value);
                if ((use_mt && e->code == ABS_MT_POSITION_X) || (!use_mt && e->code == ABS_X))
                    f_x = e->value;
                else if ((use_mt && e->code == ABS_MT_POSITION_Y) || (!use_mt && e->code == ABS_Y))
                    f_y = e->value;
                else if ((e->code == ABS_MT_TOUCH_MAJOR || e->code == ABS_MT_PRESSURE || e->code == ABS_PRESSURE) &&
                         e->value == 0)
                    f_zero = 1;
                else if (e->code == ABS_MT_TRACKING_ID && e->value < 0)
                    f_zero = 1;
            }
            else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
                f_btn = e->value;
            }
            else if (e->type == EV_SYN && e->code == SYN_MT_REPORT) {
                end_contact();
                f_x = f_y = -1;
                /* end of frame: decide whether a finger is down and emit the transition */
            }
            else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                if (!use_mt) {
                    /* single-touch: position persists, BTN_TOUCH tells up/down */
                    if (f_x >= 0) cur_x = f_x;
                    if (f_y >= 0) cur_y = f_y;
                    f_have_contact = (f_btn == 1) || (f_btn == -1 && down);
                    if (f_btn == 0) f_have_contact = 0;
                }
                else if (f_x >= 0 && f_y >= 0) {
                    /* contact data without a trailing SYN_MT_REPORT */
                    end_contact();
                }
                /* finger present -> DOWN (first frame) or MOVE; no finger after being down -> UP */
                int release = !f_have_contact || f_btn == 0;
                if (!release && cur_x >= 0 && cur_y >= 0) {
                    int sx, sy;
                    map(cur_x, cur_y, &sx, &sy);
                    emit(ev, &n, max, down ? TOUCH_MOVE : TOUCH_DOWN, sx, sy);
                    down = 1;
                }
                else if (release && down) {
                    int sx = 0, sy = 0;
                    if (cur_x >= 0) map(cur_x, cur_y, &sx, &sy);
                    emit(ev, &n, max, TOUCH_UP, sx, sy);
                    down = 0;
                    seen_explicit_release = 1;
                }
                /* reset per-frame state */
                f_contacts = 0;
                f_have_contact = 0;
                f_btn = -1;
                f_zero = 0;
                if (use_mt) f_x = f_y = -1;
            }
        }
    }
    return n;
}

int touch_tick(touch_event *ev)
{
    /* Some protocol-A drivers stop sending events while the finger rests and
     * never report the lift. Until we've seen a real release, time out. */
    if (down && !seen_explicit_release && now_ms() - last_event_ms > 300) {
        down = 0;
        ev->type = TOUCH_UP;
        ev->x = ev->y = 0;
        return 1;
    }
    return 0;
}
