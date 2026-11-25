/*
 * Backplate sensor reader: a thread that parses the frames the backplate sends on its own
 * and turns the proximity readings into "someone is near the screen" events. It never
 * writes to the port.
 *
 * The proximity message (0x42) carries [detected, raw u16, threshold u16] once a second.
 * On this unit the backplate's own `detected` flag stays at 1 (the raw reading sits above
 * its threshold with nothing in front), so nearness is judged from the raw reading: a jump
 * of more than prox_delta above its resting level. The resting level follows drops at once
 * and rises slowly. A 0 -> 1 change of the flag also counts. The sensor sometimes sits
 * pinned near 48000 (something close in front); after 60 s that becomes the resting level.
 */

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "backplate/backplate.h"
#include "core/common.h"
#include "core/config.h"

#define RX_PROXIMITY 0x42
#define RX_OCCUPANCY 0x43
#define REBASE_MS    60000 /* "near" this long without a change: treat it as the new normal */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t near_ms; /* guarded by mu */

static char dev_path[128];
static int prox_delta, debug;

uint64_t backplate_near_ms(void)
{
    pthread_mutex_lock(&mu);
    uint64_t t = near_ms;
    pthread_mutex_unlock(&mu);
    return t;
}

/* Proximity reading -> near / not near, tracking the empty-room baseline. */
static void on_proximity(int detected, int raw, int thr)
{
    static int have_base, base, last_detected = 1, was_near;
    static uint64_t near_since;
    uint64_t now = now_ms();
    if (!have_base) {
        base = raw;
        have_base = 1;
    }
    int near = raw > base + prox_delta || (detected && !last_detected);
    last_detected = detected;

    if (near) {
        if (!was_near) near_since = now;
        /* something parked in front (or a sensor drift step): stop counting it as a person */
        if (now - near_since > REBASE_MS) {
            base = raw;
            near = 0;
        }
    }
    else if (raw < base)
        base = raw; /* follow drops at once (e.g. something moved away from the sensor) */
    else
        base += (raw - base) / 16; /* and rises slowly, ~16 s at one reading per second */

    if (near) {
        pthread_mutex_lock(&mu);
        near_ms = now;
        pthread_mutex_unlock(&mu);
    }
    if (debug || near != was_near)
        tdlog("backplate: proximity %d (base %d, threshold %d, flag %d)%s", raw, base, thr, detected,
              near ? " near" : "");
    was_near = near;
}

/* Occupancy (motion) samples: only logged for now, to compare with proximity. */
static void on_occupancy(const uint8_t *p, int n)
{
    int max = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        int v = p[i] << 8 | p[i + 1];
        if (v > max) max = v;
    }
    if (debug) tdlog("backplate: motion %d", max);
}

/* One complete frame (type + payload, CRC already checked). */
static void on_frame(const uint8_t *body, int len)
{
    if (body[0] == RX_PROXIMITY && len == 6) on_proximity(body[1], body[2] << 8 | body[3], body[4] << 8 | body[5]);
    if (body[0] == RX_OCCUPANCY) on_occupancy(body + 1, len - 1);
}

/* Splits the byte stream into frames: F2 86 <n> <n bytes>, the last two being the CRC. */
static void feed(uint8_t *buf, int *fill)
{
    int i = 0;
    while (*fill - i >= 3) {
        if (buf[i] != 0xF2 || buf[i + 1] != 0x86) {
            i++;
            continue;
        }
        int n = buf[i + 2];
        if (n < 3 || n > 125) { /* not a real length: resync after this header */
            i += 2;
            continue;
        }
        if (*fill - i < 3 + n) break; /* rest of the frame hasn't arrived yet */
        const uint8_t *body = buf + i + 3;
        int blen = n - 2;
        uint16_t crc = (uint16_t)(body[blen] << 8 | body[blen + 1]);
        if (backplate_crc(body, (size_t)blen) == crc)
            on_frame(body, blen);
        else if (debug)
            tdlog("backplate: bad CRC on type %02X", body[0]);
        i += 3 + n;
    }
    memmove(buf, buf + i, (size_t)(*fill - i));
    *fill -= i;
}

/* Reads the port forever; reopens it after errors. */
static void *reader(void *arg)
{
    static uint8_t buf[512];
    for (;;) {
        int fd = backplate_open(dev_path);
        if (fd < 0) {
            tdlog("backplate: can't open %s (%s), no proximity", dev_path, strerror(errno));
            sleep(30);
            continue;
        }
        int fill = 0;
        for (;;) {
            ssize_t r = read(fd, buf + fill, sizeof buf - (size_t)fill);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                break;
            }
            fill += (int)r;
            feed(buf, &fill);
            if (fill == (int)sizeof buf) fill = 0; /* garbage only: drop it */
        }
        close(fd);
        sleep(5);
    }
    return NULL;
}

void backplate_start(const char *dev)
{
    str_copy(dev_path, sizeof dev_path, dev ? dev : BACKPLATE_DEV);
    prox_delta = config_int("backplate", "prox_delta", 400);
    debug = config_int("backplate", "debug", 0);
    backplate_relays_off(dev_path);
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) == 0) pthread_detach(t);
}
