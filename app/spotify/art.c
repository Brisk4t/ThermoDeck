/*
 * Album art. The Spotify thread calls art_request() with the current cover URL on every
 * poll; a dedicated thread downloads and decodes new covers so slow image fetches never
 * delay playback controls. The last few covers are cached, already scaled for the screen.
 */

#include "spotify/art.h"

#include <pthread.h>
#include <stdlib.h>

#include "core/common.h"
#include "gfx/gfx.h"
#include "net/https.h"
#include "spotify/spotify.h"
#include "ui/ui.h"

/* stb_image (JPEG/PNG only), with its warnings silenced */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"
#pragma GCC diagnostic pop

/* cache of decoded covers, least recently used is evicted */
#define CACHE_N 6
#define URL_MAX 256

typedef struct {
    char url[URL_MAX];
    uint32_t *px; /* ART_PX*ART_PX 0xRRGGBB */
    uint64_t used;
} entry;

/* shared between the Spotify thread (requests), art thread (fetches) and UI (drawing) */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char want[URL_MAX];
static int pending;
static const uint32_t *cur; /* art for `want`, or NULL */
static entry cache[CACHE_N];
static https_conn conn;

/* Tells the UI thread to repaint (same mechanism the Spotify state uses). */
static void notify_ui(void)
{
    pthread_mutex_lock(&g_sp.mu);
    g_sp.version++;
    pthread_mutex_unlock(&g_sp.mu);
    ui_wake();
}

/* Cached entry for url, or NULL. Caller holds mu. */
static entry *lookup(const char *url)
{
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].px && !strcmp(cache[i].url, url)) return &cache[i];
    return NULL;
}

/*
 * Called on every poll; does nothing unless the URL changed. Cached covers show
 * immediately, otherwise the art thread is woken to fetch it.
 */
void art_request(const char *url)
{
    if (!url) url = "";
    if (strlen(url) >= URL_MAX) url = "";
    pthread_mutex_lock(&mu);
    if (strcmp(url, want)) {
        str_copy(want, sizeof want, url);
        entry *e = url[0] ? lookup(url) : NULL;
        if (e) e->used = now_ms();
        cur = e ? e->px : NULL;
        if (url[0] && !e) {
            pending = 1;
            pthread_cond_signal(&cv);
        }
    }
    pthread_mutex_unlock(&mu);
}

/* Draws the cover for the current URL, if it has been loaded. */
int art_draw(int x, int y, int radius)
{
    pthread_mutex_lock(&mu);
    int ok = cur != NULL;
    if (ok) gfx_blit(x, y, ART_PX, ART_PX, cur, radius);
    pthread_mutex_unlock(&mu);
    return ok;
}

/* Center-crop to a square and box-filter down (or nearest up) to ART_PX. */
static uint32_t *scale(const unsigned char *rgb, int w, int h)
{
    uint32_t *out = malloc(ART_PX * ART_PX * sizeof *out);
    if (!out) return NULL;
    int s = w < h ? w : h;
    int ox = (w - s) / 2, oy = (h - s) / 2;
    for (int dy = 0; dy < ART_PX; dy++) {
        int y0 = dy * s / ART_PX, y1 = (dy + 1) * s / ART_PX;
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < ART_PX; dx++) {
            int x0 = dx * s / ART_PX, x1 = (dx + 1) * s / ART_PX;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = rgb + ((size_t)(oy + yy) * (size_t)w + (size_t)(ox + x0)) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            }
            out[dy * ART_PX + dx] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return out;
}

/* Downloads one image (reusing a kept-alive connection to its host) and decodes it. */
static uint32_t *fetch(const char *url)
{
    const char *p = url;
    if (strncmp(p, "https://", 8)) {
        tdlog("art: unsupported url %.80s", url);
        return NULL;
    }
    p += 8;
    const char *slash = strchr(p, '/');
    if (!slash || slash - p >= 64) return NULL;
    char host[64];
    memcpy(host, p, (size_t)(slash - p));
    host[slash - p] = 0;
    if (strcmp(host, conn.host)) {
        https_close(&conn);
        https_init(&conn, host);
    }
    uint64_t t0 = now_ms();
    http_resp r;
    if (https_request(&conn, "GET", slash, "Accept: image/*\r\n", NULL, 0, &r) < 0) {
        tdlog("art: fetch failed: %s", r.error);
        https_close(&conn);
        return NULL;
    }
    uint32_t *px = NULL;
    if (r.status != 200) {
        tdlog("art: HTTP %d for %.80s", r.status, url);
    }
    else {
        uint64_t t1 = now_ms();
        int w, h, n;
        unsigned char *rgb = stbi_load_from_memory((const unsigned char *)r.body, (int)r.body_len, &w, &h, &n, 3);
        if (!rgb) {
            tdlog("art: decode failed (%s), %zu bytes", stbi_failure_reason(), r.body_len);
        }
        else {
            px = scale(rgb, w, h);
            stbi_image_free(rgb);
            tdlog("art: %dx%d, %zu bytes, fetch %llums, decode+scale %llums", w, h, r.body_len,
                  (unsigned long long)(t1 - t0), (unsigned long long)(now_ms() - t1));
        }
    }
    http_resp_free(&r);
    return px;
}

/*
 * Art thread: waits for a new URL, fetches it, stores it in the cache and repaints if
 * it's still the one wanted (the track may have changed in the meantime).
 */
static void *worker(void *arg)
{
    (void)arg;
    thread_background();
    for (;;) {
        char url[URL_MAX];
        pthread_mutex_lock(&mu);
        while (!pending) pthread_cond_wait(&cv, &mu);
        pending = 0;
        str_copy(url, sizeof url, want);
        int have = lookup(url) != NULL;
        pthread_mutex_unlock(&mu);
        if (!url[0] || have) continue;

        uint32_t *px = fetch(url);
        if (!px) continue; /* retried when the url changes again */

        pthread_mutex_lock(&mu);
        /* evict the least recently used slot that isn't on screen */
        entry *slot = NULL;
        for (int i = 0; i < CACHE_N; i++) {
            entry *e = &cache[i];
            if (e->px && e->px == cur) continue;
            if (!e->px) {
                slot = e;
                break;
            }
            if (!slot || e->used < slot->used) slot = e;
        }
        free(slot->px);
        slot->px = px;
        slot->used = now_ms();
        str_copy(slot->url, sizeof slot->url, url);
        int show = !strcmp(url, want);
        if (show) cur = px;
        pthread_mutex_unlock(&mu);
        if (show) notify_ui();
    }
    return NULL;
}

/* Starts the art thread (256 KB stack for the JPEG decoder). */
void art_start(void)
{
    https_init(&conn, "i.scdn.co");
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_create(&t, &at, worker, NULL);
    pthread_detach(t);
}
