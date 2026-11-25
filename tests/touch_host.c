/* Host checks for the touch driver (lib/input), fed protocol-A frames through a FIFO:
 * make tests && ./build-host/tests/touch_host */
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/common.h"
#include "input/touch.h"

#ifndef input_event_sec
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
#endif

static int tw; /* write end */

static uint32_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u);
}

static void put_ev(uint32_t ms, int type, int code, int value)
{
    struct input_event e = {0};
    e.input_event_sec = ms / 1000;
    e.input_event_usec = (ms % 1000) * 1000;
    e.type = (uint16_t)type;
    e.code = (uint16_t)code;
    e.value = value;
    assert(write(tw, &e, sizeof e) == sizeof e);
}

/* one frame with a finger at (x, y), or no finger if x < 0 */
static void put_frame(uint32_t ms, int x, int y)
{
    if (x >= 0) {
        put_ev(ms, EV_ABS, ABS_MT_POSITION_X, x);
        put_ev(ms, EV_ABS, ABS_MT_POSITION_Y, y);
    }
    put_ev(ms, EV_SYN, SYN_MT_REPORT, 0);
    put_ev(ms, EV_SYN, SYN_REPORT, 0);
}

/* reads everything queued, then (after the bridge time) any held-back lift */
static int get_events(touch_event *ev, int max)
{
    int n = touch_read(ev, max);
    usleep(70 * 1000);
    if (n < max && touch_tick(&ev[n])) n++;
    return n;
}

int main(void)
{
    char fifo[] = "/tmp/ui_host_touchXXXXXX";
    assert(mkdtemp(fifo));
    char path[64];
    snprintf(path, sizeof path, "%s/ev", fifo);
    assert(!mkfifo(path, 0600));
    touch_opts o = {0};
    assert(touch_open(path, &o) >= 0);
    tw = open(path, O_WRONLY);
    assert(tw >= 0);
    touch_event ev[32];

    /* a 20 ms contact gap mid-swipe stays one touch */
    uint32_t t = wall_ms() - 200;
    put_frame(t, 160, 300);
    put_frame(t + 10, 160, 280);
    put_frame(t + 20, -1, 0);
    put_frame(t + 40, 160, 240);
    put_frame(t + 50, -1, 0);
    int n = get_events(ev, 32);
    assert(n == 4 && ev[0].type == TOUCH_DOWN && ev[1].type == TOUCH_MOVE && ev[2].type == TOUCH_MOVE &&
           ev[2].y == 240 && ev[3].type == TOUCH_UP && ev[3].y == 240 && ev[3].ms == t + 50);
    assert(touch_timeout() == -1);

    /* a longer gap is a new touch */
    t = wall_ms() - 300;
    put_frame(t, 100, 100);
    put_frame(t + 10, -1, 0);
    put_frame(t + 150, 200, 200);
    put_frame(t + 160, -1, 0);
    n = get_events(ev, 32);
    assert(n == 4 && ev[0].type == TOUCH_DOWN && ev[1].type == TOUCH_UP && ev[1].ms == t + 10 &&
           ev[2].type == TOUCH_DOWN && ev[3].type == TOUCH_UP);

    /* a backlog bigger than the buffer: moves merge, the lift survives */
    t = wall_ms() - 500;
    for (int i = 0; i < 60; i++) put_frame(t + (uint32_t)i * 5, 160, 400 - i * 4);
    put_frame(t + 300, -1, 0);
    n = get_events(ev, 16);
    assert(n <= 16 && ev[0].type == TOUCH_DOWN && ev[n - 1].type == TOUCH_UP && ev[n - 2].type == TOUCH_MOVE &&
           ev[n - 2].y == 400 - 59 * 4);
    close(tw);
    unlink(path);
    rmdir(fifo);
    puts("touch OK");
    return 0;
}
