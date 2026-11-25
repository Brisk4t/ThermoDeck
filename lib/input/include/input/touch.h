#ifndef INPUT_TOUCH_H
#define INPUT_TOUCH_H
#include <stdint.h>

enum { TOUCH_NONE = 0, TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

typedef struct {
    int type, x, y;
    uint32_t ms; /* when the controller reported it (ms, arbitrary origin: compare within a gesture) */
} touch_event;

typedef struct {
    int swap_xy, invert_x, invert_y;
    int debug; /* log raw coordinates */
} touch_opts;

int touch_open(const char *dev, const touch_opts *opts); /* returns fd */
/* Reads pending input; fills up to max events. Call when fd is readable. */
int touch_read(touch_event *ev, int max);
/* Delivers a lift once it's certain (see touch.c), or synthesizes one if the driver never
 * reports lifts. Call after touch_read() and on every wakeup. */
int touch_tick(touch_event *ev);
/* ms until touch_tick() has a lift to deliver, -1 if none is pending (a poll timeout). */
int touch_timeout(void);
int touch_fd(void);

#endif
