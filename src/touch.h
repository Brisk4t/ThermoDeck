#ifndef TOUCH_H
#define TOUCH_H
#include <stdint.h>

enum { TOUCH_NONE = 0, TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

typedef struct {
    int type, x, y;
} touch_event;

typedef struct {
    int swap_xy, invert_x, invert_y;
    int debug; /* log raw coordinates */
} touch_opts;

int touch_open(const char *dev, const touch_opts *opts); /* returns fd */
/* Reads pending input; fills up to max events. Call when fd is readable. */
int touch_read(touch_event *ev, int max);
/* Synthesizes a release if the driver never reports lifts. Call periodically. */
int touch_tick(touch_event *ev);
int touch_fd(void);

#endif
