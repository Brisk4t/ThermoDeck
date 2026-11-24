/* Small HTTP server on the LAN (port 8080 by default).
 *
 * While Spotify needs authorizing, the login/paste page is gated by a 6-digit PIN
 * shown on the thermostat screen; a correct PIN sets a short-lived session cookie.
 * Once connected, "/" is a read-only status page (now playing + debug info).
 * Requests from the device itself (127.0.0.1, e.g. auth.sh) skip the PIN.
 *
 * GET  /          status page, or PIN form / login page while authorizing
 * POST /pin       pin=NNNNNN -> session cookie
 * GET  /callback  ?code=&state= (needs session; localhost: auth.sh)
 * POST /code      url=<redirected address> (needs session)
 * GET  /logout    forget the Spotify token (localhost only: auth.sh logout)
 */
#include "web.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "spotify.h"

#define SESSION_MS (15 * 60 * 1000)
#define MAX_PIN_FAILURES 5

static int port;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;
static char pin[7];
static char session[33];
static uint64_t session_expiry;
static int pin_failures;

/* ---------- PIN / session ---------- */

static void new_pin(void)
{
    unsigned char r[4];
    random_bytes(r, sizeof r);
    uint32_t v = ((uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]) % 1000000u;
    pthread_mutex_lock(&wmu);
    snprintf(pin, sizeof pin, "%06u", (unsigned)v);
    pin_failures = 0;
    session[0] = 0;
    pthread_mutex_unlock(&wmu);
    /* repaint the screen with the new PIN */
    pthread_mutex_lock(&g_sp.mu);
    g_sp.version++;
    pthread_mutex_unlock(&g_sp.mu);
    ui_wake();
}

void web_get_pin(char out[7])
{
    pthread_mutex_lock(&wmu);
    memcpy(out, pin, 7);
    pthread_mutex_unlock(&wmu);
}

static int ct_equal(const char *a, const char *b, size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static int session_valid(const char *cookie_val)
{
    pthread_mutex_lock(&wmu);
    int ok = session[0] && strlen(cookie_val) == 32 && ct_equal(cookie_val, session, 32) && now_ms() < session_expiry;
    pthread_mutex_unlock(&wmu);
    return ok;
}

/* ---------- HTTP helpers ---------- */

static void html_escape(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (; *in && o + 7 < outlen; in++) {
        switch (*in) {
        case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
        case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(out + o, "&#39;", 5); o += 5; break;
        default: out[o++] = *in;
        }
    }
    out[o] = 0;
}

static void send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        buf += w;
        len -= (size_t)w;
    }
}

static void respond(int fd, int status, const char *ctype, const char *extra_hdr, const char *body)
{
    char head[768];
    const char *st = status == 200 ? "OK" : status == 303 ? "See Other" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : "Bad Request";
    int n = snprintf(head, sizeof head,
                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n%sConnection: close\r\n\r\n",
                     status, st, ctype, strlen(body), extra_hdr ? extra_hdr : "");
    send_all(fd, head, (size_t)n);
    send_all(fd, body, strlen(body));
}

static void redirect(int fd, const char *location, const char *extra)
{
    char h[2600];
    snprintf(h, sizeof h, "Location: %s\r\n%s", location, extra ? extra : "");
    respond(fd, 303, "text/plain", h, "");
}

static const char *CSS =
    "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#121212;color:#eee;max-width:560px;margin:0 auto;padding:20px;line-height:1.45}"
    "h1{font-size:22px}a.btn,button{display:inline-block;background:#1DB954;color:#000;font-weight:600;border:0;border-radius:24px;padding:12px 22px;"
    "text-decoration:none;font-size:16px;cursor:pointer}"
    "textarea,input{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:8px;padding:10px;font-size:15px}"
    "textarea{height:90px}input.pin{font-size:28px;letter-spacing:8px;text-align:center;max-width:240px}"
    ".card{background:#1e1e1e;border-radius:12px;padding:16px;margin:14px 0}.muted{color:#9a9a9a;font-size:14px}code{background:#2a2a2a;padding:1px 5px;border-radius:4px}"
    ".ok{color:#1DB954}.err{color:#ff6b6b}.warn{color:#E0B341}ol{padding-left:20px}"
    "table{width:100%;border-collapse:collapse;font-size:14px}td{padding:5px 4px;border-bottom:1px solid #2a2a2a;vertical-align:top}td:first-child{color:#9a9a9a;width:42%}"
    ".bar{height:6px;background:#3e3e3e;border-radius:3px;margin:10px 0 4px}.bar div{height:6px;background:#fff;border-radius:3px}";

typedef struct {
    char *b;
    size_t o, cap;
} sbuf;

static void P(sbuf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void P(sbuf *s, const char *fmt, ...)
{
    if (s->o >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->b + s->o, s->cap - s->o, fmt, ap);
    va_end(ap);
    if (n > 0) s->o += (size_t)n;
    if (s->o >= s->cap) s->o = s->cap - 1;
}

static void page_start(sbuf *s, int refresh)
{
    P(s, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>");
    if (refresh) P(s, "<meta http-equiv=refresh content=%d>", refresh);
    P(s, "<title>ThermoDeck</title><style>%s</style></head><body><h1>ThermoDeck</h1>", CSS);
}

static void page_end(sbuf *s, int fd)
{
    P(s, "<p class=muted>ThermoDeck " TD_VERSION "</p></body></html>");
    respond(fd, 200, "text/html; charset=utf-8", NULL, s->b);
}

static void fmt_ms(int ms, char *out, size_t len)
{
    int sec = ms / 1000;
    snprintf(out, len, "%d:%02d", sec / 60, sec % 60);
}

static long read_kb(const char *path, const char *key)
{
    char *d = file_read(path, NULL);
    long v = -1;
    if (d) {
        char *p = strstr(d, key);
        if (p) v = atol(p + strlen(key));
        free(d);
    }
    return v;
}

/* ---------- pages ---------- */

static int needs_auth(sp_auth_state a)
{
    return spotify_configured() && (a == SP_NEED_AUTH || a == SP_AUTHORIZING);
}

static void page_status(int fd)
{
    static char body[12000];
    sbuf s = {body, 0, sizeof body};
    char title[400], artist[400], album[400], device[160], ssid[100], msg[300], ip[16];
    pthread_mutex_lock(&g_sp.mu);
    sp_auth_state auth = g_sp.auth;
    html_escape(g_sp.title, title, sizeof title);
    html_escape(g_sp.artist, artist, sizeof artist);
    html_escape(g_sp.album, album, sizeof album);
    html_escape(g_sp.device, device, sizeof device);
    html_escape(g_sp.wifi_ssid, ssid, sizeof ssid);
    html_escape(g_sp.auth_msg, msg, sizeof msg);
    str_copy(ip, sizeof ip, g_sp.ip);
    int has_item = g_sp.has_item, playing = g_sp.is_playing, dur = g_sp.duration_ms, vol = g_sp.volume;
    int poll_ok = g_sp.last_poll_ok, synced = g_sp.time_synced, sig = g_sp.wifi_signal, net = g_sp.net_up;
    pthread_mutex_unlock(&g_sp.mu);
    int prog = has_item ? spotify_progress_now() : 0;

    page_start(&s, 5);
    P(&s, "<div class=card>");
    switch (auth) {
    case SP_READY:
        P(&s, "<b class=ok>Spotify connected</b>");
        if (!poll_ok) P(&s, " <span class=warn>(last update failed, retrying)</span>");
        if (has_item) {
            char a[16], b[16];
            fmt_ms(prog, a, sizeof a);
            fmt_ms(dur, b, sizeof b);
            P(&s, "<p style='margin-bottom:0'><span class=muted>%s%s%s</span><br><b style='font-size:18px'>%s</b><br>%s<br><span class=muted>%s</span></p>",
              playing ? "NOW PLAYING" : "PAUSED", device[0] ? " on " : "", device, title, artist, album);
            P(&s, "<div class=bar><div style='width:%d%%'></div></div><span class=muted>%s / %s</span>",
              dur > 0 ? (int)((long long)prog * 100 / dur) : 0, a, b);
        } else {
            P(&s, "<p class=muted>Nothing playing.</p>");
        }
        break;
    case SP_NO_CONFIG:
        P(&s, "<b class=err>Spotify not configured.</b><p>Add your app's <code>client_id</code> and <code>client_secret</code> to "
              "<code>%s/thermodeck.ini</code> and restart ThermoDeck.</p>", g_dir);
        break;
    case SP_WAIT_NETWORK: P(&s, "<b class=warn>Waiting for network</b>"); break;
    default: P(&s, "<b class=warn>Spotify not connected</b>%s%s", msg[0] ? "<p class=err>" : "", msg[0] ? msg : ""); break;
    }
    P(&s, "</div>");

    /* debug */
    double up = 0;
    char *u = file_read("/proc/uptime", NULL);
    if (u) { up = atof(u); free(u); }
    char *la = file_read("/proc/loadavg", NULL);
    char load[64] = "?";
    if (la) { str_copy(load, sizeof load, la); load[strcspn(load, "\n")] = 0; free(la); }
    char clock[40] = "not synced";
    if (synced) {
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(clock, sizeof clock, "%Y-%m-%d %H:%M:%S UTC", &tm);
    }
    int upm = (int)(up / 60);
    P(&s, "<div class=card><table>");
    P(&s, "<tr><td>Wi-Fi</td><td>%s</td></tr>", net ? ssid : "disconnected");
    if (net) P(&s, "<tr><td>IP address</td><td>%s</td></tr>", ip);
    if (net && sig) P(&s, "<tr><td>Signal</td><td>%d dBm</td></tr>", sig);
    P(&s, "<tr><td>Clock</td><td>%s</td></tr>", clock);
    if (auth == SP_READY) {
        if (vol >= 0) P(&s, "<tr><td>Volume</td><td>%d%%</td></tr>", vol);
        else P(&s, "<tr><td>Volume</td><td>unknown</td></tr>");
    }
    P(&s, "<tr><td>Uptime</td><td>%dd %dh %dm</td></tr>", upm / 1440, (upm / 60) % 24, upm % 60);
    P(&s, "<tr><td>Load average</td><td>%s</td></tr>", load);
    P(&s, "<tr><td>ThermoDeck memory</td><td>%ld kB</td></tr>", read_kb("/proc/self/status", "VmRSS:"));
    P(&s, "<tr><td>Free memory</td><td>%ld kB</td></tr>", read_kb("/proc/meminfo", "MemFree:"));
    P(&s, "</table></div>");
    page_end(&s, fd);
}

static void page_pin(int fd, const char *error)
{
    static char body[6000];
    sbuf s = {body, 0, sizeof body};
    page_start(&s, 0);
    P(&s, "<div class=card><b>Connect Spotify</b><p>Enter the PIN shown on the thermostat screen.</p>"
          "<form method=post action=/pin><input class=pin name=pin inputmode=numeric pattern='[0-9]*' maxlength=6 autocomplete=off autofocus>"
          "<p><button type=submit>Continue</button></p></form>");
    if (error && *error) P(&s, "<p class=err>%s</p>", error);
    P(&s, "</div>");
    page_end(&s, fd);
}

static void page_login(int fd)
{
    static char body[12000];
    sbuf s = {body, 0, sizeof body};
    char url[1400], esc_url[2200], msg[300], ru[600];
    pthread_mutex_lock(&g_sp.mu);
    sp_auth_state auth = g_sp.auth;
    html_escape(g_sp.auth_msg, msg, sizeof msg);
    pthread_mutex_unlock(&g_sp.mu);
    html_escape(spotify_redirect_uri(), ru, sizeof ru);
    spotify_authorize_url(url, sizeof url);
    html_escape(url, esc_url, sizeof esc_url);

    page_start(&s, auth == SP_AUTHORIZING ? 3 : 0);
    P(&s, "<div class=card><ol>"
          "<li><code>%s</code> must be listed under <i>Redirect URIs</i> in your app on the "
          "<a style='color:#1DB954' href='https://developer.spotify.com/dashboard' target=_blank>Spotify developer dashboard</a>.</li>"
          "<li><a class=btn href=\"%s\" target=_blank>Log in with Spotify</a></li>"
          "<li>After approving, the browser opens <code>%s?code=...</code>, which won't load. Copy the <b>full address</b> "
          "from the address bar and paste it here:</li></ol>"
          "<form method=post action=/code><textarea name=url placeholder='http://127.0.0.1:8080/callback?code=...'></textarea>"
          "<p><button type=submit>Connect</button></p></form>",
      ru, esc_url, ru);
    if (auth == SP_AUTHORIZING) P(&s, "<p class=warn>Authorizing&hellip;</p>");
    else if (msg[0]) P(&s, "<p class=err>%s</p>", msg);
    P(&s, "</div>");
    page_end(&s, fd);
}

static void page_done(int fd, int ok, const char *text)
{
    static char body[3000];
    sbuf s = {body, 0, sizeof body};
    P(&s, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
          "<meta http-equiv=refresh content='3;url=/'><title>ThermoDeck</title><style>%s</style></head><body>"
          "<h1>ThermoDeck</h1><div class=card><b class=%s>%s</b><p class=muted>Returning to the status page&hellip;</p></div></body></html>",
      CSS, ok ? "ok" : "err", text);
    respond(fd, 200, "text/html; charset=utf-8", NULL, s.b);
}

/* Accepts a pasted redirect URL ("...?code=X&state=Y") or a bare code. */
static int accept_code_from(const char *text)
{
    char code[512] = "", state[64] = "";
    const char *q = strchr(text, '?');
    if (query_param(q ? q + 1 : text, "code", code, sizeof code) == 0) {
        query_param(q ? q + 1 : text, "state", state, sizeof state);
    } else if (!strchr(text, '=') && !strchr(text, '/') && strlen(text) > 20) {
        str_copy(code, sizeof code, text);
        code[strcspn(code, " \r\n\t")] = 0;
    }
    if (!code[0]) return -1;
    return spotify_submit_code(code, state);
}

static void get_cookie(const char *req, const char *hdr_end, const char *name, char *out, size_t outlen)
{
    out[0] = 0;
    const char *c = strcasestr(req, "\r\nCookie:");
    if (!c || c > hdr_end) return;
    const char *eol = strstr(c + 2, "\r\n");
    size_t nl = strlen(name);
    for (const char *p = c + 9; p && p < eol; p = strchr(p, ';')) {
        while (*p == ';' || *p == ' ') p++;
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            size_t l = strcspn(p + nl + 1, "; \r\n");
            if (l >= outlen) l = outlen - 1;
            memcpy(out, p + nl + 1, l);
            out[l] = 0;
            return;
        }
    }
}

static void handle(int fd, int local)
{
    static char req[16384];
    size_t len = 0;
    char *hdr_end = NULL;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (len + 1 < sizeof req) {
        ssize_t r = recv(fd, req + len, sizeof req - 1 - len, 0);
        if (r <= 0) break;
        len += (size_t)r;
        req[len] = 0;
        if (!hdr_end) hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) {
            char *cl = strcasestr(req, "\r\nContent-Length:");
            size_t want = cl && cl < hdr_end ? (size_t)atol(cl + 17) : 0;
            if (len >= (size_t)(hdr_end + 4 - req) + want) break;
        }
    }
    req[len] = 0;
    if (!hdr_end) return;
    char method[8] = "", target[2048] = "";
    if (sscanf(req, "%7s %2047s", method, target) != 2) return;
    char *body = hdr_end + 4;
    char *qs = strchr(target, '?');
    if (qs) *qs++ = 0;

    char cookie[64];
    get_cookie(req, hdr_end, "td", cookie, sizeof cookie);
    int authed = local || session_valid(cookie);
    pthread_mutex_lock(&g_sp.mu);
    sp_auth_state auth = g_sp.auth;
    pthread_mutex_unlock(&g_sp.mu);

    if (!strcmp(method, "GET") && !strcmp(target, "/")) {
        if (!needs_auth(auth)) page_status(fd);
        else if (authed) page_login(fd);
        else page_pin(fd, NULL);
    } else if (!strcmp(method, "POST") && !strcmp(target, "/pin")) {
        char p[16] = "";
        query_param(body, "pin", p, sizeof p);
        char cur[7];
        web_get_pin(cur);
        if (strlen(p) == 6 && ct_equal(p, cur, 6)) {
            unsigned char r[16];
            char tok[33], hdr[160];
            random_bytes(r, sizeof r);
            hex_encode(r, sizeof r, tok);
            pthread_mutex_lock(&wmu);
            memcpy(session, tok, 33);
            session_expiry = now_ms() + SESSION_MS;
            pin_failures = 0;
            pthread_mutex_unlock(&wmu);
            snprintf(hdr, sizeof hdr, "Set-Cookie: td=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax\r\n", tok, SESSION_MS / 1000);
            redirect(fd, "/", hdr);
        } else {
            sleep(2); /* slow down guessing */
            pthread_mutex_lock(&wmu);
            int fails = ++pin_failures;
            pthread_mutex_unlock(&wmu);
            tdlog("web: wrong PIN (%d)", fails);
            if (fails >= MAX_PIN_FAILURES) {
                new_pin();
                page_pin(fd, "Too many wrong PINs - a new PIN is now shown on the thermostat.");
            } else {
                page_pin(fd, "Wrong PIN.");
            }
        }
    } else if (!strcmp(method, "GET") && !strcmp(target, "/callback")) {
        char err[64] = "", code[512] = "", state[64] = "";
        if (!authed) {
            page_pin(fd, "Enter the PIN first, then log in again.");
        } else if (qs && query_param(qs, "error", err, sizeof err) == 0) {
            char e[128], m[200];
            html_escape(err, e, sizeof e);
            snprintf(m, sizeof m, "Spotify returned an error: %s", e);
            page_done(fd, 0, m);
        } else if (qs && query_param(qs, "code", code, sizeof code) == 0) {
            query_param(qs, "state", state, sizeof state);
            int rc = spotify_submit_code(code, state);
            page_done(fd, rc == 0, rc == 0 ? "Authorization received - check the thermostat screen." : "State mismatch - please start again.");
        } else {
            page_done(fd, 0, "No authorization code in the request.");
        }
    } else if (!strcmp(method, "POST") && !strcmp(target, "/code")) {
        if (!authed) {
            page_pin(fd, "Your session expired - enter the PIN again.");
        } else {
            char text[2048] = "";
            if (query_param(body, "url", text, sizeof text) < 0) text[0] = 0;
            int rc = accept_code_from(text);
            page_done(fd, rc == 0, rc == 0 ? "Authorization received - check the thermostat screen." :
                                             "Couldn't find a valid code in that address (or it was from an older login link).");
        }
    } else if (!strcmp(method, "GET") && !strcmp(target, "/logout")) {
        if (!local) {
            respond(fd, 403, "text/plain", NULL, "Log out from the device console: /config/thermodeck/auth.sh logout\n");
        } else {
            spotify_command(CMD_LOGOUT);
            respond(fd, 200, "text/plain", NULL, "Logged out of Spotify.\n");
        }
    } else {
        respond(fd, 404, "text/plain", NULL, "not found\n");
    }
}

static void *server(void *arg)
{
    (void)arg;
    int s = -1;
    for (;;) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 4) == 0) break;
        tdlog("web: bind :%d failed: %s (retrying)", port, strerror(errno));
        close(s);
        sleep(5);
    }
    tdlog("web: listening on :%d", port);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept(s, (struct sockaddr *)&peer, &pl);
        if (c < 0) {
            if (errno != EINTR) sleep(1);
            continue;
        }
        int local = peer.sin_family == AF_INET && ntohl(peer.sin_addr.s_addr) == INADDR_LOOPBACK;
        handle(c, local);
        close(c);
    }
    return NULL;
}

void web_start(int p)
{
    port = p;
    new_pin();
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_create(&t, &at, server, NULL);
    pthread_detach(t);
}
