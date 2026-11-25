/*
 * Spotify Web API client, running on its own "network" thread. It owns the OAuth tokens,
 * polls /v1/me/player for what's playing, and sends the UI's commands (next, previous,
 * play/pause, volume). Everything the UI shows goes through g_sp, guarded by g_sp.mu;
 * publish() bumps g_sp.version and wakes the UI thread to repaint.
 */

#include "spotify/spotify.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/common.h"
#include "core/config.h"
#include "json/json.h"
#include "net/https.h"
#include "net/sntp.h"
#include "spotify/art.h"
#include "ui/ui.h"

#define SCOPES "user-read-playback-state user-modify-playback-state user-read-currently-playing"

sp_shared g_sp = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .volume = -1, .supports_volume = 1};

/* connections, tokens and settings: only touched by the worker thread */
static https_conn accounts, api;
static char access_token[512];
static char refresh_token[512];
static uint64_t token_expiry_ms;
static char client_id[128], client_secret[128], redirect_uri[256];
static int poll_ms, idle_poll_ms, volume_step;

/* JSON token storage for the current response (worker thread only) */
static jtok toks[3000];

/* ---------- small helpers ---------- */

/* Marks the shared state as changed. Caller holds g_sp.mu. */
static void bump_locked(void) { g_sp.version++; }

/* Marks the state changed and wakes the UI to repaint. */
static void publish(void)
{
    /* call with lock held */
    bump_locked();
    ui_wake();
}

/* Updates the auth state / message shown on the auth screen (only repaints on change). */
static void set_auth(sp_auth_state s, const char *msg)
{
    pthread_mutex_lock(&g_sp.mu);
    if (g_sp.auth != s || (msg && strcmp(g_sp.auth_msg, msg))) {
        g_sp.auth = s;
        if (msg) str_copy(g_sp.auth_msg, sizeof g_sp.auth_msg, msg);
        publish();
    }
    pthread_mutex_unlock(&g_sp.mu);
}

/* Shows a short message on the player screen for 4 seconds. */
static void toast(const char *msg)
{
    pthread_mutex_lock(&g_sp.mu);
    str_copy(g_sp.toast, sizeof g_sp.toast, msg);
    g_sp.toast_until = now_ms() + 4000;
    publish();
    pthread_mutex_unlock(&g_sp.mu);
}

/* Whether client_id/secret are set (and not still the example placeholders). */
int spotify_configured(void) { return client_id[0] && client_secret[0] && client_id[0] != '<'; }

const char *spotify_redirect_uri(void) { return redirect_uri; }

/* Builds the accounts.spotify.com login URL (with our anti-CSRF state) for the web page. */
int spotify_authorize_url(char *out, size_t outlen)
{
    if (!spotify_configured()) return -1;
    char cid[256], ru[512], sc[256], state[64];
    url_encode(client_id, cid, sizeof cid);
    url_encode(redirect_uri, ru, sizeof ru);
    url_encode(SCOPES, sc, sizeof sc);
    pthread_mutex_lock(&g_sp.mu);
    str_copy(state, sizeof state, g_sp.oauth_state);
    pthread_mutex_unlock(&g_sp.mu);
    snprintf(out, outlen,
             "https://accounts.spotify.com/authorize?response_type=code&client_id=%s&scope=%s&redirect_uri=%s&state=%s",
             cid, sc, ru, state);
    return 0;
}

static void token_path(char *p, size_t l) { path_join(p, l, "spotify_token"); }

/* The refresh token is the only persistent credential (spotify_token, mode 0600). */
static void load_refresh_token(void)
{
    char p[300];
    token_path(p, sizeof p);
    char *d = file_read(p, NULL);
    refresh_token[0] = 0;
    if (d) {
        d[strcspn(d, "\r\n")] = 0;
        str_copy(refresh_token, sizeof refresh_token, d);
        free(d);
    }
}

static void save_refresh_token(void)
{
    char p[300];
    token_path(p, sizeof p);
    if (!refresh_token[0]) {
        unlink(p);
        return;
    }
    char buf[600];
    int n = snprintf(buf, sizeof buf, "%s\n", refresh_token);
    if (file_write_atomic(p, buf, (size_t)n, 0600) < 0) tdlog("spotify: could not save token to %s", p);
}

/* ---------- accounts.spotify.com ---------- */

/*
 * POSTs to /api/token with the app's client credentials and stores the tokens returned.
 * Returns 0 on success, -1 if the grant was rejected (re-auth needed), -2 on transient
 * errors (network, 5xx, rate limit) worth retrying.
 */
static int token_request(const char *form)
{
    char creds[300], b64[420], hdr[600];
    snprintf(creds, sizeof creds, "%s:%s", client_id, client_secret);
    base64_encode((const unsigned char *)creds, strlen(creds), b64, sizeof b64);
    snprintf(hdr, sizeof hdr, "Authorization: Basic %s\r\nContent-Type: application/x-www-form-urlencoded\r\n", b64);
    http_resp r;
    if (https_request(&accounts, "POST", "/api/token", hdr, form, strlen(form), &r) < 0) {
        char m[128];
        snprintf(m, sizeof m, "Network error: %s", r.error);
        set_auth(g_sp.auth, m);
        return -2; /* transient */
    }
    json_doc d;
    int rc = -1;
    if (json_parse(&d, r.body, r.body_len, toks, ARRAY_LEN(toks)) > 0) {
        if (r.status == 200) {
            char at[sizeof access_token], rt[sizeof refresh_token];
            if (json_str(&d, json_get(&d, 0, "access_token"), at, sizeof at) == 0 && at[0]) {
                str_copy(access_token, sizeof access_token, at);
                long long exp = json_int(&d, json_get(&d, 0, "expires_in"), 3600);
                token_expiry_ms = now_ms() + (uint64_t)exp * 1000;
                if (json_str(&d, json_get(&d, 0, "refresh_token"), rt, sizeof rt) == 0 && rt[0]) {
                    str_copy(refresh_token, sizeof refresh_token, rt);
                    save_refresh_token();
                }
                rc = 0;
            }
        }
        else {
            char err[64] = "", desc[128] = "";
            json_str(&d, json_get(&d, 0, "error"), err, sizeof err);
            json_str(&d, json_get(&d, 0, "error_description"), desc, sizeof desc);
            tdlog("spotify: token error %d %s: %s", r.status, err, desc);
            char m[128];
            snprintf(m, sizeof m, "Spotify: %s%s%s", err, desc[0] ? " - " : "", desc);
            set_auth(g_sp.auth, m);
            rc = (r.status >= 500 || r.status == 429) ? -2 : -1;
        }
    }
    else {
        tdlog("spotify: token response %d unparsable", r.status);
        rc = r.status >= 500 ? -2 : -1;
    }
    http_resp_free(&r);
    return rc;
}

/* Authorization code (from the login redirect) -> access + refresh token. */
static int exchange_code(const char *code)
{
    char c[1024], ru[512], form[1700];
    url_encode(code, c, sizeof c);
    url_encode(redirect_uri, ru, sizeof ru);
    snprintf(form, sizeof form, "grant_type=authorization_code&code=%s&redirect_uri=%s", c, ru);
    return token_request(form);
}

/* Uses the refresh token to get a new access token (they last an hour). */
static int refresh_access(void)
{
    char rt[1024], form[1200];
    url_encode(refresh_token, rt, sizeof rt);
    snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s", rt);
    return token_request(form);
}

/* ---------- api.spotify.com ---------- */

/*
 * Authenticated request to api.spotify.com. A 401 forces a token refresh; a 429 waits out
 * the rate limit (up to a minute).
 */
static int api_call(const char *method, const char *path, http_resp *r)
{
    char hdr[700];
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", access_token);
    if (https_request(&api, method, path, hdr, NULL, 0, r) < 0) return -1;
    if (r->status == 401) token_expiry_ms = 0; /* force refresh */
    if (r->status == 429) {
        int ra = r->retry_after > 0 ? r->retry_after : 10;
        tdlog("spotify: rate limited for %ds", ra);
        sleep((unsigned)(ra > 60 ? 60 : ra));
    }
    return 0;
}

/* Comma-separated artist names from an "artists" array. */
static void join_artists(json_doc *d, int arr, char *out, size_t outlen)
{
    out[0] = 0;
    size_t o = 0;
    for (int i = 0;; i++) {
        int a = json_at(d, arr, i);
        if (a < 0) break;
        char name[128];
        if (json_str(d, json_get(d, a, "name"), name, sizeof name) < 0) continue;
        int n = snprintf(out + o, outlen - o, "%s%s", o ? ", " : "", name);
        if (n < 0 || (size_t)n >= outlen - o) break;
        o += (size_t)n;
    }
}

/* Picks the smallest image at least ART_PX wide (Spotify lists 640, 300, 64). */
static void pick_image(json_doc *d, int arr, char *out, size_t outlen)
{
    out[0] = 0;
    int best_w = 0;
    for (int i = 0;; i++) {
        int im = json_at(d, arr, i);
        if (im < 0) break;
        char url[256];
        if (json_str(d, json_get(d, im, "url"), url, sizeof url) < 0 || !url[0]) continue;
        int w = (int)json_int(d, json_get(d, im, "width"), 0);
        int better = !out[0] || (w >= ART_PX && (best_w < ART_PX || w < best_w)) || (best_w < ART_PX && w > best_w);
        if (better) {
            str_copy(out, outlen, url);
            best_w = w;
        }
    }
}

/*
 * GET /v1/me/player and publish track, progress, device and volume.
 * Returns 0 on success, -1 on failure.
 */
static int poll_player(void)
{
    http_resp r;
    uint64_t t0 = now_ms();
    if (api_call("GET", "/v1/me/player?market=from_token&additional_types=episode", &r) < 0) {
        pthread_mutex_lock(&g_sp.mu);
        if (g_sp.last_poll_ok) {
            g_sp.last_poll_ok = 0;
            publish();
        }
        pthread_mutex_unlock(&g_sp.mu);
        return -1;
    }
    uint64_t t1 = now_ms();
    int rc = 0;
    /* nothing is playing anywhere */
    if (r.status == 204 || (r.status == 200 && r.body_len == 0)) {
        pthread_mutex_lock(&g_sp.mu);
        art_request("");
        if (g_sp.has_item || !g_sp.last_poll_ok || g_sp.is_playing) {
            g_sp.has_item = 0;
            g_sp.is_playing = 0;
            g_sp.device[0] = 0;
            g_sp.last_poll_ok = 1;
            publish();
        }
        pthread_mutex_unlock(&g_sp.mu);
    }
    else if (r.status == 200) {
        json_doc d;
        int n = json_parse(&d, r.body, r.body_len, toks, ARRAY_LEN(toks));
        if (n <= 0) {
            tdlog("spotify: player JSON parse error %d (len %zu)", n, r.body_len);
            rc = -1;
        }
        else {
            /* track (or podcast episode) details */
            int item = json_get(&d, 0, "item");
            char title[256] = "", artist[256] = "", album[256] = "", device[64] = "";
            int has_item = !json_is_null(&d, item);
            int duration = 0;
            char image[256] = "";
            if (has_item) {
                json_str(&d, json_get(&d, item, "name"), title, sizeof title);
                duration = (int)json_int(&d, json_get(&d, item, "duration_ms"), 0);
                int artists = json_get(&d, item, "artists");
                if (artists >= 0) {
                    join_artists(&d, artists, artist, sizeof artist);
                    json_str(&d, json_path(&d, item, "album.name"), album, sizeof album);
                    pick_image(&d, json_path(&d, item, "album.images"), image, sizeof image);
                }
                else { /* podcast episode */
                    json_str(&d, json_path(&d, item, "show.name"), artist, sizeof artist);
                    json_str(&d, json_path(&d, item, "show.publisher"), album, sizeof album);
                    pick_image(&d, json_get(&d, item, "images"), image, sizeof image);
                    if (!image[0]) pick_image(&d, json_path(&d, item, "show.images"), image, sizeof image);
                }
            }
            /* active device and playback position */
            int dev = json_get(&d, 0, "device");
            json_str(&d, json_get(&d, dev, "name"), device, sizeof device);
            int vol_tok = json_get(&d, dev, "volume_percent");
            int vol = json_is_null(&d, vol_tok) ? -1 : (int)json_int(&d, vol_tok, -1);
            int sup_vol = json_bool(&d, json_get(&d, dev, "supports_volume"), 1);
            int playing = json_bool(&d, json_get(&d, 0, "is_playing"), 0);
            int progress = (int)json_int(&d, json_get(&d, 0, "progress_ms"), 0);
            art_request(image);

            pthread_mutex_lock(&g_sp.mu);
            int hold = now_ms() < g_sp.play_hold_until;
            if (hold && playing != g_sp.is_playing) {
                /* Spotify hasn't caught up with our play/pause yet: keep the optimistic state */
                playing = g_sp.is_playing;
                progress = g_sp.progress_ms + (playing ? (int)(t1 - g_sp.sync_ms) : 0);
                t0 = t1;
            }
            int changed = strcmp(title, g_sp.title) || strcmp(artist, g_sp.artist) || has_item != g_sp.has_item ||
                          playing != g_sp.is_playing || strcmp(device, g_sp.device) || !g_sp.last_poll_ok;
            g_sp.has_item = has_item;
            str_copy(g_sp.title, sizeof g_sp.title, title);
            str_copy(g_sp.artist, sizeof g_sp.artist, artist);
            str_copy(g_sp.album, sizeof g_sp.album, album);
            str_copy(g_sp.device, sizeof g_sp.device, device);
            g_sp.is_playing = playing;
            if (sup_vol != g_sp.supports_volume) {
                g_sp.supports_volume = sup_vol;
                changed = 1;
            }
            g_sp.duration_ms = duration;
            /* Time sync: the reported position was sampled roughly mid-request. */
            g_sp.progress_ms = progress;
            g_sp.sync_ms = t0 + (t1 - t0) / 2;
            if (now_ms() >= g_sp.volume_hold_until && vol != g_sp.volume) {
                g_sp.volume = vol;
                changed = 1;
            }
            g_sp.last_poll_ok = 1;
            if (changed)
                tdlog("spotify: %s '%s' - '%s' on '%s' vol %d", playing ? "playing" : "paused", title, artist, device,
                      vol);
            publish(); /* progress resync always repaints */
            pthread_mutex_unlock(&g_sp.mu);
        }
    }
    else if (r.status != 401 && r.status != 429) {
        tdlog("spotify: player status %d: %.200s", r.status, r.body ? r.body : "");
        rc = -1;
    }
    http_resp_free(&r);
    return rc;
}

/* Turns a failed player command into a short message, using Spotify's "reason" field. */
static void control_error(const http_resp *r, char *out, size_t outlen)
{
    char reason[48] = "", message[96] = "";
    json_doc d;
    if (r->body_len && json_parse(&d, r->body, r->body_len, toks, ARRAY_LEN(toks)) > 0) {
        int e = json_get(&d, 0, "error");
        json_str(&d, json_get(&d, e, "reason"), reason, sizeof reason);
        json_str(&d, json_get(&d, e, "message"), message, sizeof message);
    }
    if (!strcmp(reason, "PREMIUM_REQUIRED"))
        str_copy(out, outlen, "Spotify Premium is required for controls");
    else if (!strcmp(reason, "VOLUME_CONTROL_DISALLOW"))
        str_copy(out, outlen, "This device doesn't allow remote volume");
    else if (!strcmp(reason, "NO_ACTIVE_DEVICE") || r->status == 404)
        str_copy(out, outlen, "No active Spotify device");
    else if (!strcmp(reason, "NO_PREV_TRACK"))
        str_copy(out, outlen, "No previous track");
    else if (!strcmp(reason, "NO_NEXT_TRACK"))
        str_copy(out, outlen, "No next track");
    else if (!strcmp(reason, "ALREADY_PAUSED") || !strcmp(reason, "ALREADY_PLAYING"))
        out[0] = 0;
    else if (r->status == 429)
        str_copy(out, outlen, "Rate limited by Spotify");
    else if (message[0])
        snprintf(out, outlen, "Spotify: %s", message);
    else
        snprintf(out, outlen, "Command failed (%d)", r->status);
}

/* Sends a player command; shows a toast if it fails. Returns 0 on success. */
static int run_command(const char *method, const char *path)
{
    http_resp r;
    int rc = 0;
    if (api_call(method, path, &r) < 0) {
        toast("Network error");
        return -1;
    }
    if (r.status >= 300) {
        tdlog("spotify: %s %s -> %d %.200s", method, path, r.status, r.body ? r.body : "");
        char m[96];
        control_error(&r, m, sizeof m);
        if (r.status != 401 && m[0]) toast(m);
        rc = -1;
    }
    http_resp_free(&r);
    return rc;
}

/* ---------- worker ---------- */

/* Sleeps until deadline_ms, or earlier if a command or auth code arrives. */
static void wait_for(uint64_t deadline_ms)
{
    /* sleeps until deadline or until a command/auth code/network change arrives */
    pthread_mutex_lock(&g_sp.mu);
    while (!g_sp.cmds && !g_sp.auth_code[0]) {
        uint64_t now = now_ms();
        if (now >= deadline_ms) break;
        uint64_t left = deadline_ms - now;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(left / 1000);
        ts.tv_nsec += (long)(left % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int e = pthread_cond_timedwait(&g_sp.cv, &g_sp.mu, &ts);
        if (e == ETIMEDOUT) {
            /* the realtime clock can jump (NTP); re-check monotonic deadline */
            if (now_ms() >= deadline_ms) break;
        }
    }
    pthread_mutex_unlock(&g_sp.mu);
}

/*
 * The network thread. Each pass: wait for Wi-Fi, sync the clock, make sure we have a
 * valid access token (handling new logins and logouts), send queued commands, then poll
 * the player, sleeping in between.
 */
static void *worker(void *arg)
{
    (void)arg;
    thread_background();
    uint64_t next_poll = 0, next_ntp = 0;
    int ntp_ok = 0, ntp_tries = 0;
    load_refresh_token();
    for (;;) {
        pthread_mutex_lock(&g_sp.mu);
        int net_up = g_sp.net_up;
        pthread_mutex_unlock(&g_sp.mu);
        /* no Wi-Fi: drop connections and wait */
        if (!net_up) {
            set_auth(SP_WAIT_NETWORK, NULL);
            https_close(&api);
            https_close(&accounts);
            wait_for(now_ms() + 1000);
            continue;
        }
        uint64_t now = now_ms();
        /* set the clock once (TLS certificate checks need a sane time) */
        if (!ntp_ok && now >= next_ntp) {
            const char *srv = config_get("network", "ntp_server", "pool.ntp.org");
            ntp_ok = sntp_sync(srv) == 0 || sntp_sync("time.google.com") == 0;
            /* DNS/UDP often isn't ready right after DHCP: retry quickly at first */
            next_ntp = now_ms() + (++ntp_tries < 6 ? 5000 : 30000);
            pthread_mutex_lock(&g_sp.mu);
            /* a clock past the build date (e.g. set before a restart) is good enough to show */
            g_sp.time_synced = ntp_ok || time(NULL) > (time_t)g_build_epoch;
            publish();
            pthread_mutex_unlock(&g_sp.mu);
        }
        if (!spotify_configured()) {
            set_auth(SP_NO_CONFIG, "Set client_id and client_secret in thermodeck.ini");
            wait_for(now_ms() + 5000);
            continue;
        }

        /* logout request from the web page */
        pthread_mutex_lock(&g_sp.mu);
        int logout = g_sp.cmds & CMD_LOGOUT;
        g_sp.cmds &= ~CMD_LOGOUT;
        char code[sizeof g_sp.auth_code];
        str_copy(code, sizeof code, g_sp.auth_code);
        g_sp.auth_code[0] = 0;
        pthread_mutex_unlock(&g_sp.mu);
        if (logout) {
            tdlog("spotify: logged out");
            refresh_token[0] = access_token[0] = 0;
            token_expiry_ms = 0;
            save_refresh_token();
            pthread_mutex_lock(&g_sp.mu);
            g_sp.has_item = 0;
            publish();
            pthread_mutex_unlock(&g_sp.mu);
        }

        /* a fresh login from the web page / auth.sh */
        if (code[0]) {
            set_auth(SP_AUTHORIZING, "Authorizing...");
            int rc = exchange_code(code);
            if (rc == 0) {
                tdlog("spotify: authorized");
                set_auth(SP_READY, "");
                next_poll = 0;
            }
            else {
                set_auth(SP_NEED_AUTH, NULL); /* message set by token_request */
            }
            continue;
        }

        /* never logged in (or logged out): wait for the user */
        if (!refresh_token[0]) {
            set_auth(SP_NEED_AUTH, NULL);
            wait_for(now_ms() + 5000);
            continue;
        }

        /* access token missing or about to expire: refresh it */
        if (!access_token[0] || now_ms() + 60000 > token_expiry_ms) {
            if (g_sp.auth != SP_READY) set_auth(SP_AUTHORIZING, "Signing in...");
            int rc = refresh_access();
            if (rc == -1) {
                /* refresh token revoked/invalid: need a new authorization */
                tdlog("spotify: refresh failed, need re-authorization");
                refresh_token[0] = access_token[0] = 0;
                save_refresh_token();
                set_auth(SP_NEED_AUTH, NULL);
                continue;
            }
            else if (rc == -2) {
                wait_for(now_ms() + 5000);
                continue;
            }
            set_auth(SP_READY, "");
        }

        /* commands */
        pthread_mutex_lock(&g_sp.mu);
        int cmds = g_sp.cmds;
        int vol = g_sp.volume_target;
        g_sp.cmds = 0;
        pthread_mutex_unlock(&g_sp.mu);
        if (cmds & (CMD_NEXT | CMD_PREV | CMD_VOLUME | CMD_PLAY | CMD_PAUSE)) {
            if (cmds & (CMD_PLAY | CMD_PAUSE)) {
                int play = (cmds & CMD_PLAY) != 0;
                if (run_command("PUT", play ? "/v1/me/player/play" : "/v1/me/player/pause") < 0) {
                    pthread_mutex_lock(&g_sp.mu);
                    g_sp.play_hold_until = 0; /* failed: let the next poll restore the real state */
                    pthread_mutex_unlock(&g_sp.mu);
                }
            }
            if (cmds & CMD_NEXT) run_command("POST", "/v1/me/player/next");
            if (cmds & CMD_PREV) run_command("POST", "/v1/me/player/previous");
            if (cmds & CMD_VOLUME) {
                char path[96];
                snprintf(path, sizeof path, "/v1/me/player/volume?volume_percent=%d", vol);
                run_command("PUT", path);
            }
            /* Spotify needs a moment before the change shows in /me/player */
            next_poll = now_ms() + ((cmds & (CMD_NEXT | CMD_PREV | CMD_PLAY | CMD_PAUSE)) ? 600 : 1500);
        }

        now = now_ms();
        /* poll faster while playing; back off after errors */
        if (now >= next_poll) {
            int ok = poll_player() == 0;
            pthread_mutex_lock(&g_sp.mu);
            int playing = g_sp.is_playing && g_sp.has_item;
            int remaining = g_sp.duration_ms - g_sp.progress_ms;
            pthread_mutex_unlock(&g_sp.mu);
            int interval = playing ? poll_ms : idle_poll_ms;
            if (!ok) interval = interval < 5000 ? 5000 : interval;
            /* poll right after the track should end so the next one shows promptly */
            if (playing && remaining > 0 && remaining + 300 < interval) interval = remaining + 300;
            next_poll = now_ms() + (uint64_t)interval;
        }
        wait_for(next_poll);
    }
    return NULL;
}

/* Reads the [spotify] settings and starts the network thread. */
void spotify_start(void)
{
    str_copy(client_id, sizeof client_id, config_get("spotify", "client_id", ""));
    str_copy(client_secret, sizeof client_secret, config_get("spotify", "client_secret", ""));
    str_copy(redirect_uri, sizeof redirect_uri,
             config_get("spotify", "redirect_uri", "http://127.0.0.1:8080/callback"));
    poll_ms = config_int("spotify", "poll_interval_ms", 2000);
    idle_poll_ms = config_int("spotify", "idle_poll_interval_ms", 5000);
    volume_step = config_int("spotify", "volume_step", 10);
    if (poll_ms < 1000) poll_ms = 1000;
    if (idle_poll_ms < poll_ms) idle_poll_ms = poll_ms;
    https_init(&accounts, "accounts.spotify.com");
    https_init(&api, "api.spotify.com");
    unsigned char rnd[16];
    random_bytes(rnd, sizeof rnd);
    hex_encode(rnd, sizeof rnd, g_sp.oauth_state);
    g_sp.auth = SP_WAIT_NETWORK;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_create(&t, &at, worker, NULL);
    pthread_detach(t);
}

/* Queues a command (CMD_*) for the network thread. */
void spotify_command(int cmd)
{
    pthread_mutex_lock(&g_sp.mu);
    g_sp.cmds |= cmd;
    pthread_cond_signal(&g_sp.cv);
    pthread_mutex_unlock(&g_sp.mu);
}

/*
 * Hands a login code to the network thread. A non-empty state must match the one in our
 * login URL, so a stale or forged redirect is rejected.
 */
int spotify_submit_code(const char *code, const char *state)
{
    pthread_mutex_lock(&g_sp.mu);
    int ok = !state || !state[0] || !strcmp(state, g_sp.oauth_state);
    if (ok) {
        str_copy(g_sp.auth_code, sizeof g_sp.auth_code, code);
        pthread_cond_signal(&g_sp.cv);
    }
    pthread_mutex_unlock(&g_sp.mu);
    return ok ? 0 : -1;
}

/*
 * Changes volume by one step. The new value shows immediately and polls won't overwrite
 * it for 3 s, so repeated taps feel responsive.
 */
void spotify_volume_step(int dir)
{
    pthread_mutex_lock(&g_sp.mu);
    int v = g_sp.volume < 0 ? 50 : g_sp.volume;
    v = CLAMP(v + dir * volume_step, 0, 100);
    g_sp.volume = v;
    g_sp.volume_target = v;
    g_sp.volume_hold_until = now_ms() + 3000;
    g_sp.cmds |= CMD_VOLUME;
    bump_locked();
    pthread_cond_signal(&g_sp.cv);
    pthread_mutex_unlock(&g_sp.mu);
}

/* Flips play/pause immediately on screen and queues the matching command. */
void spotify_toggle_play(void)
{
    pthread_mutex_lock(&g_sp.mu);
    uint64_t now = now_ms();
    /* freeze/restart the local stopwatch at the current position */
    int p = g_sp.progress_ms;
    if (g_sp.is_playing) p += (int)(now - g_sp.sync_ms);
    if (g_sp.duration_ms > 0 && p > g_sp.duration_ms) p = g_sp.duration_ms;
    g_sp.progress_ms = p;
    g_sp.sync_ms = now;
    g_sp.is_playing = !g_sp.is_playing;
    g_sp.cmds &= ~(CMD_PLAY | CMD_PAUSE);
    g_sp.cmds |= g_sp.is_playing ? CMD_PLAY : CMD_PAUSE;
    g_sp.play_hold_until = now + 2500;
    bump_locked();
    pthread_cond_signal(&g_sp.cv);
    pthread_mutex_unlock(&g_sp.mu);
}

/* Called by the UI when Wi-Fi changes; wakes the network thread if it came up. */
void spotify_set_net(int up, const char *ip, const char *ssid, int signal)
{
    pthread_mutex_lock(&g_sp.mu);
    str_copy(g_sp.wifi_ssid, sizeof g_sp.wifi_ssid, ssid);
    g_sp.wifi_signal = signal;
    if (g_sp.net_up != up || strcmp(g_sp.ip, ip)) {
        g_sp.net_up = up;
        str_copy(g_sp.ip, sizeof g_sp.ip, ip);
        bump_locked();
        pthread_cond_signal(&g_sp.cv);
    }
    pthread_mutex_unlock(&g_sp.mu);
}

/* Playback position "now": the last synced position plus time elapsed since (if playing). */
int spotify_progress_now(void)
{
    pthread_mutex_lock(&g_sp.mu);
    int p = g_sp.progress_ms;
    if (g_sp.is_playing) p += (int)(now_ms() - g_sp.sync_ms);
    if (g_sp.duration_ms > 0 && p > g_sp.duration_ms) p = g_sp.duration_ms;
    if (p < 0) p = 0;
    pthread_mutex_unlock(&g_sp.mu);
    return p;
}
