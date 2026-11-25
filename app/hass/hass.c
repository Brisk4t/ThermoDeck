/*
 * Home Assistant WebSocket client thread.
 *
 * Session: auth with the long-lived token -> subscribe to lovelace_updated -> fetch the
 * dashboard (lovelace/config) -> subscribe_entities for the entities it shows. Edits to
 * the dashboard in HA arrive as lovelace_updated and trigger a reload, so the layout is
 * defined (and changed) on the server. Taps from the UI are queued as call_service
 * messages; the worker sends them and reports failures as toasts.
 */

#include "hass/hass.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "core/common.h"
#include "cards/cards.h"
#include "core/config.h"
#include "json/json.h"
#include "net/ws.h"
#include "ui/ui.h"

ha_shared g_ha = {.mu = PTHREAD_MUTEX_INITIALIZER};

/* settings from thermodeck.ini [homeassistant] */
static char cfg_url[128], cfg_token[512], cfg_dashboard[64], cfg_entities[1024];

/* UI -> worker: queued message bodies (JSON members without the id) + a wake pipe */
#define QLEN 8
static char queue[QLEN][768];
static int q_head, q_count;
static int net_up;
static int wake_pipe[2] = {-1, -1};

static void wake_worker(void)
{
    char c = 1;
    if (wake_pipe[1] >= 0 && write(wake_pipe[1], &c, 1) < 0) {}
}

/* ---------- shared state helpers ---------- */

/* Sets the status (and its message) and repaints. */
static void set_status(ha_status s, const char *fmt, ...)
{
    pthread_mutex_lock(&g_ha.mu);
    g_ha.status = s;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ha.msg, sizeof g_ha.msg, fmt, ap);
    va_end(ap);
    g_ha.version++;
    pthread_mutex_unlock(&g_ha.mu);
    ui_wake();
}

/* Toast shown on the HA screen. Call with g_ha.mu held. */
static void toast_locked(const char *msg)
{
    str_copy(g_ha.toast, sizeof g_ha.toast, msg);
    g_ha.toast_until = now_ms() + 4000;
    g_ha.version++;
}

static int find_entity(const char *id)
{
    for (int i = 0; i < g_ha.d.n_ents; i++)
        if (!strcmp(g_ha.d.ents[i].id, id)) return i;
    return -1;
}

/* ---------- UI side (called from the UI thread) ---------- */

void hass_set_net(int up)
{
    pthread_mutex_lock(&g_ha.mu);
    net_up = up;
    pthread_mutex_unlock(&g_ha.mu);
    wake_worker();
}

/* Queues a message for the worker. Call with g_ha.mu held. */
static void enqueue_locked(const char *body)
{
    if (q_count == QLEN) {
        toast_locked("Home Assistant is busy");
        return;
    }
    str_copy(queue[(q_head + q_count) % QLEN], sizeof queue[0], body);
    q_count++;
}

static int is(const char *a, const char *b) { return !strcmp(a, b); }

/* Shows the expected result of a toggle right away; HA's state update confirms it. */
static void optimistic_toggle(ha_entity *e)
{
    static const char *const PAIRS[][2] = {{"on", "off"}, {"open", "closed"}};
    for (size_t i = 0; i < ARRAY_LEN(PAIRS); i++)
        for (int k = 0; k < 2; k++)
            if (is(e->state, PAIRS[i][k])) {
                str_copy(e->state, sizeof e->state, PAIRS[i][!k]);
                e->hold_until = now_ms() + 4000;
                return;
            }
}

void hass_press(int idx)
{
    char body[768];
    pthread_mutex_lock(&g_ha.mu);
    if (idx < 0 || idx >= g_ha.d.n_items || g_ha.status != HA_READY) goto out;
    const ha_item *it = &g_ha.d.items[idx];
    ha_entity *e = it->ent >= 0 ? &g_ha.d.ents[it->ent] : NULL;
    if (it->act == HA_ACT_NONE) goto out;

    if (it->act == HA_ACT_CALL) {
        /* explicit action from the card: "domain.service" + its target / data */
        char dom[48];
        const char *dot = strchr(it->service, '.');
        snprintf(dom, sizeof dom, "%.*s", (int)(dot - it->service), it->service);
        snprintf(body, sizeof body, "\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\"%s%s", dom, dot + 1,
                 it->data[0] ? "," : "", it->data);
        enqueue_locked(body);
        goto out;
    }
    if (!e) goto out;

    /* default / toggle: pick the service from the entity's domain */
    char dom[32];
    snprintf(dom, sizeof dom, "%.*s", (int)strcspn(e->id, "."), e->id);
    const char *domain = dom, *service = NULL;
    int toggle = it->act == HA_ACT_TOGGLE || ha_entity_toggles(e);
    if (toggle) {
        service = "toggle";
        if (is(dom, "group") || is(dom, "lock") || is(dom, "climate")) domain = "homeassistant";
        optimistic_toggle(e);
    }
    else if (is(dom, "scene") || is(dom, "script")) {
        service = "turn_on";
    }
    else if (is(dom, "button") || is(dom, "input_button")) {
        service = "press";
    }
    else if (is(dom, "media_player")) {
        service = "media_play_pause";
    }
    if (!service) goto out; /* sensors etc.: nothing to do */
    snprintf(body, sizeof body,
             "\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\",\"target\":{\"entity_id\":\"%s\"}", domain,
             service, e->id);
    enqueue_locked(body);
    g_ha.version++;
out:
    pthread_mutex_unlock(&g_ha.mu);
    wake_worker();
    ui_wake();
}

/* ---------- worker: session ---------- */

typedef struct {
    ws_conn *ws;
    int next_id;
    int lovelace_sub;              /* subscription id for lovelace_updated */
    int config_req;                /* id of the pending lovelace/config request */
    int ents_sub;                  /* subscription id for entity states, 0 if none */
    int fc_subs[HA_MAX_FORECASTS]; /* forecast subscription ids (index = ha_dash.fcs), 0 if none */
    int call_ids[QLEN];
    int n_calls;
    uint64_t last_rx, last_ping;
} session;

/* Sends {"id":N,<body>}; returns the id, or -1 if the connection broke. */
static int send_msg(session *s, const char *body)
{
    size_t bl = strlen(body);
    char *m = malloc(bl + 32);
    if (!m) return -1;
    int id = s->next_id++;
    int n = snprintf(m, bl + 32, "{\"id\":%d,%s}", id, body);
    int rc = ws_send(s->ws, m, (size_t)n);
    free(m);
    return rc < 0 ? -1 : id;
}

/* Copies `in` into a JSON string body (escaping quotes and backslashes). */
static void json_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; *in && o + 2 < len; in++) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

static int request_config(session *s)
{
    char body[192], esc[128];
    json_escape(cfg_dashboard, esc, sizeof esc);
    if (cfg_dashboard[0])
        snprintf(body, sizeof body, "\"type\":\"lovelace/config\",\"url_path\":\"%s\"", esc);
    else
        snprintf(body, sizeof body, "\"type\":\"lovelace/config\",\"url_path\":null");
    s->config_req = send_msg(s, body);
    return s->config_req;
}

static int unsubscribe(session *s, int *sub)
{
    if (*sub <= 0) return 0;
    char body[96];
    snprintf(body, sizeof body, "\"type\":\"unsubscribe_events\",\"subscription\":%d", *sub);
    *sub = 0;
    return send_msg(s, body) < 0 ? -1 : 0;
}

/* Subscribes to the forecasts the dashboard's cards asked for (ha_dash.fcs). */
static int subscribe_forecasts(session *s)
{
    pthread_mutex_lock(&g_ha.mu);
    int n = g_ha.d.n_fcs;
    char bodies[HA_MAX_FORECASTS][160];
    for (int i = 0; i < n; i++)
        snprintf(bodies[i], sizeof bodies[i],
                 "\"type\":\"weather/subscribe_forecast\",\"entity_id\":\"%s\",\"forecast_type\":\"%s\"",
                 g_ha.d.ents[g_ha.d.fcs[i].ent].id, g_ha.d.fcs[i].type);
    pthread_mutex_unlock(&g_ha.mu);
    for (int i = 0; i < n; i++)
        if ((s->fc_subs[i] = send_msg(s, bodies[i])) < 0) return -1;
    return 0;
}

/* (Re)subscribes to the states of the dashboard's entities, and to its forecasts. */
static int subscribe_entities(session *s)
{
    if (unsubscribe(s, &s->ents_sub) < 0) return -1;
    for (int i = 0; i < HA_MAX_FORECASTS; i++)
        if (unsubscribe(s, &s->fc_subs[i]) < 0) return -1;
    if (subscribe_forecasts(s) < 0) return -1;
    pthread_mutex_lock(&g_ha.mu);
    size_t cap = (size_t)g_ha.d.n_ents * 72 + 64, o = 0;
    char *body = malloc(cap);
    if (!body) {
        pthread_mutex_unlock(&g_ha.mu);
        return -1;
    }
    o += (size_t)snprintf(body + o, cap - o, "\"type\":\"subscribe_entities\",\"entity_ids\":[");
    for (int i = 0; i < g_ha.d.n_ents; i++)
        o += (size_t)snprintf(body + o, cap - o, "%s\"%s\"", i ? "," : "", g_ha.d.ents[i].id);
    snprintf(body + o, cap - o, "]");
    int n = g_ha.d.n_ents;
    pthread_mutex_unlock(&g_ha.mu);
    int rc = n ? send_msg(s, body) : 0;
    free(body);
    if (rc > 0) s->ents_sub = rc;
    return rc;
}

/* Installs a freshly parsed dashboard, keeping the states (and forecasts) we already know. */
static void install_dashboard(ha_dash *nd)
{
    pthread_mutex_lock(&g_ha.mu);
    for (int i = 0; i < nd->n_ents; i++) {
        int old = find_entity(nd->ents[i].id);
        if (old < 0) continue;
        /* the old state, but the attribute list the new cards want (with values we already have) */
        ha_entity fresh = nd->ents[i];
        nd->ents[i] = g_ha.d.ents[old];
        nd->ents[i].n_attrs = fresh.n_attrs;
        for (int k = 0; k < fresh.n_attrs; k++) {
            const char *v = ha_entity_attr(&g_ha.d.ents[old], fresh.attrs[k].key);
            nd->ents[i].attrs[k] = fresh.attrs[k];
            str_copy(nd->ents[i].attrs[k].val, sizeof fresh.attrs[k].val, v ? v : "");
        }
    }
    for (int i = 0; i < nd->n_fcs; i++)
        for (int k = 0; k < g_ha.d.n_fcs; k++)
            if (!strcmp(g_ha.d.ents[g_ha.d.fcs[k].ent].id, nd->ents[nd->fcs[i].ent].id) &&
                !strcmp(g_ha.d.fcs[k].type, nd->fcs[i].type)) {
                nd->fcs[i].n = g_ha.d.fcs[k].n;
                memcpy(nd->fcs[i].e, g_ha.d.fcs[k].e, sizeof nd->fcs[i].e);
            }
    g_ha.d = *nd;
    g_ha.status = HA_READY;
    g_ha.msg[0] = 0;
    g_ha.layout_ver++;
    g_ha.version++;
    pthread_mutex_unlock(&g_ha.mu);
    ui_wake();
}

/* Applies a lovelace/config result (or its failure). Returns -1 if the connection broke. */
static int on_config(session *s, const json_doc *d, int msg)
{
    ha_dash *nd = malloc(sizeof *nd);
    if (!nd) return -1;
    char err[160] = "";
    int ok = json_bool(d, json_get(d, msg, "success"), 0), rc = 0;
    if (ok) {
        int res = json_get(d, msg, "result");
        const jtok *t = &d->t[res];
        if (ha_dashboard_parse(d->js + t->start, (size_t)(t->end - t->start), nd, err, sizeof err) == 0) {
            tdlog("hass: dashboard loaded: %d items, %d entities", nd->n_items, nd->n_ents);
            install_dashboard(nd);
            rc = subscribe_entities(s);
            free(nd);
            return rc < 0 ? -1 : 0;
        }
    }
    else {
        int e = json_get(d, msg, "error");
        char code[48] = "";
        json_str(d, json_get(d, e, "code"), code, sizeof code);
        json_str(d, json_get(d, e, "message"), err, sizeof err);
        if (is(code, "config_not_found"))
            snprintf(err, sizeof err, "%s%s%s", "No dashboard ", cfg_dashboard[0] ? cfg_dashboard : "(default)",
                     " in Home Assistant");
    }
    if (cfg_entities[0]) {
        /* fallback: tiles for the entities listed in the ini */
        tdlog("hass: %s; using the entities from thermodeck.ini", err);
        ha_dashboard_from_list(cfg_entities, nd);
        install_dashboard(nd);
        rc = subscribe_entities(s);
    }
    else {
        tdlog("hass: no dashboard: %s", err);
        set_status(HA_NO_DASHBOARD, "%s", err);
    }
    free(nd);
    return rc < 0 ? -1 : 0;
}

/* A JSON value as text: strings unescaped, numbers / booleans as written, null as "". */
static void literal(const json_doc *d, int val, char *out, size_t len)
{
    int n = d->t[val].end - d->t[val].start;
    if (json_is_null(d, val) || d->t[val].type == J_OBJ || d->t[val].type == J_ARR)
        out[0] = 0;
    else if (json_str(d, val, out, len) != 0)
        snprintf(out, len, "%.*s", n, d->js + d->t[val].start);
}

/* Reads the attributes we display from an attribute object (a full set or a diff). */
static void apply_attrs(ha_entity *e, const json_doc *d, int a)
{
    if (a < 0) return;
    char v[64];
    int k = a + 1;
    for (int m = 0; m < d->t[a].size / 2 && k < d->n; m++) {
        int val = k + 1;
        char key[32];
        json_str(d, k, key, sizeof key);
        int null = json_is_null(d, val);
        /* attributes some card asked to track */
        for (int w = 0; w < e->n_attrs; w++)
            if (is(key, e->attrs[w].key)) literal(d, val, e->attrs[w].val, sizeof e->attrs[w].val);
        if (is(key, "friendly_name"))
            json_str(d, val, e->name, sizeof e->name);
        else if (is(key, "unit_of_measurement"))
            json_str(d, val, e->unit, sizeof e->unit);
        else if (is(key, "icon"))
            json_str(d, val, e->icon, sizeof e->icon);
        else if (is(key, "device_class"))
            json_str(d, val, e->device_class, sizeof e->device_class);
        else if (is(key, "brightness"))
            e->brightness = null ? -1 : (int)json_int(d, val, -1);
        else if (is(key, "media_title"))
            json_str(d, val, e->extra, sizeof e->extra);
        else if (is(key, "current_temperature") || is(key, "current_position")) {
            /* numbers: copy the literal ("21.5") with a unit */
            int n = d->t[val].end - d->t[val].start;
            if (null || n <= 0 || n > 16) {
                e->extra[0] = 0;
            }
            else {
                snprintf(v, sizeof v, "%.*s", n, d->js + d->t[val].start);
                snprintf(e->extra, sizeof e->extra, "%s%s", v, is(key, "current_position") ? "%" : "\xC2\xB0");
            }
        }
        k = json_next(d, val);
    }
}

/* Removes attributes listed in a diff's "-" part. */
static void remove_attrs(ha_entity *e, const json_doc *d, int arr)
{
    for (int i = 0;; i++) {
        int t = json_at(d, arr, i);
        if (t < 0) break;
        char key[32];
        json_str(d, t, key, sizeof key);
        if (is(key, "unit_of_measurement")) e->unit[0] = 0;
        if (is(key, "icon")) e->icon[0] = 0;
        if (is(key, "brightness")) e->brightness = -1;
        if (is(key, "media_title") || is(key, "current_temperature") || is(key, "current_position")) e->extra[0] = 0;
        for (int w = 0; w < e->n_attrs; w++)
            if (is(key, e->attrs[w].key)) e->attrs[w].val[0] = 0;
    }
}

static void set_state(ha_entity *e, const json_doc *d, int s)
{
    if (s < 0) return;
    json_str(d, s, e->real_state, sizeof e->real_state);
    str_copy(e->state, sizeof e->state, e->real_state);
    e->hold_until = 0;
    e->has_state = 1;
}

/* subscribe_entities event: "a" = full states, "c" = changes ("+" added, "-" removed). */
static void on_entities(const json_doc *d, int ev)
{
    pthread_mutex_lock(&g_ha.mu);
    int a = json_get(d, ev, "a"), c = json_get(d, ev, "c");
    for (int part = 0; part < 2; part++) {
        int obj = part ? c : a;
        if (obj < 0) continue;
        int k = obj + 1;
        for (int m = 0; m < d->t[obj].size / 2 && k < d->n; m++) {
            int val = k + 1;
            char id[64];
            json_str(d, k, id, sizeof id);
            int idx = find_entity(id);
            if (idx >= 0) {
                ha_entity *e = &g_ha.d.ents[idx];
                if (!part) {
                    /* full state: attributes are complete, so reset them first */
                    e->unit[0] = e->icon[0] = e->device_class[0] = e->extra[0] = 0;
                    e->brightness = -1;
                    for (int w = 0; w < e->n_attrs; w++) e->attrs[w].val[0] = 0;
                    set_state(e, d, json_get(d, val, "s"));
                    apply_attrs(e, d, json_get(d, val, "a"));
                }
                else {
                    int plus = json_get(d, val, "+"), minus = json_get(d, val, "-");
                    if (plus >= 0) {
                        set_state(e, d, json_get(d, plus, "s"));
                        apply_attrs(e, d, json_get(d, plus, "a"));
                    }
                    if (minus >= 0) remove_attrs(e, d, json_get(d, minus, "a"));
                }
            }
            k = json_next(d, val);
        }
    }
    g_ha.version++;
    pthread_mutex_unlock(&g_ha.mu);
    ui_wake();
}

/* "2026-11-24T18:00:00+00:00" (or ...Z, or with fractions) -> Unix time; 0 if unreadable. */
static int64_t parse_iso_time(const char *s)
{
    struct tm tm = {0};
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    int64_t t = (int64_t)timegm(&tm);
    /* UTC offset after the time, e.g. "+05:30" */
    const char *z = s + 19;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    int oh = 0, om = 0;
    if ((*z == '+' || *z == '-') && sscanf(z + 1, "%d:%d", &oh, &om) >= 1)
        t -= (*z == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    return t;
}

/* weather/subscribe_forecast event: {"type": "daily", "forecast": [{condition, datetime, temperature, templow}, ...]} */
static void on_forecast(int idx, const json_doc *d, int ev)
{
    pthread_mutex_lock(&g_ha.mu);
    if (idx < g_ha.d.n_fcs) {
        ha_forecast *f = &g_ha.d.fcs[idx];
        int arr = json_get(d, ev, "forecast");
        f->n = 0;
        for (int i = 0; i < HA_FORECAST_LEN; i++) {
            int fe = json_at(d, arr, i);
            if (fe < 0) break;
            ha_forecast_entry *e = &f->e[f->n++];
            char when[40] = "";
            json_str(d, json_get(d, fe, "datetime"), when, sizeof when);
            e->time = parse_iso_time(when);
            e->condition[0] = e->temp[0] = e->templow[0] = 0;
            json_str(d, json_get(d, fe, "condition"), e->condition, sizeof e->condition);
            int t = json_get(d, fe, "temperature"), tl = json_get(d, fe, "templow");
            if (t >= 0) literal(d, t, e->temp, sizeof e->temp);
            if (tl >= 0) literal(d, tl, e->templow, sizeof e->templow);
        }
        g_ha.version++;
    }
    pthread_mutex_unlock(&g_ha.mu);
    ui_wake();
}

/* Handles one message from HA. Returns -1 to drop the connection. */
static int on_message(session *s, const char *js, size_t len)
{
    int max = (int)(len / 3) + 64;
    jtok *t = malloc((size_t)max * sizeof *t);
    if (!t) return -1;
    json_doc d;
    int rc = 0;
    if (json_parse(&d, js, len, t, max) <= 0) goto done;
    char type[32];
    json_str(&d, json_get(&d, 0, "type"), type, sizeof type);
    int id = (int)json_int(&d, json_get(&d, 0, "id"), -1);
    if (is(type, "event")) {
        int ev = json_get(&d, 0, "event");
        if (id == s->ents_sub) {
            on_entities(&d, ev);
        }
        for (int i = 0; i < HA_MAX_FORECASTS; i++)
            if (id > 0 && id == s->fc_subs[i])
                on_forecast(i, &d, ev);
            else if (id == s->lovelace_sub) {
                /* someone edited a dashboard in HA: reload if it's ours */
                char path[64] = "";
                json_str(&d, json_path(&d, ev, "data.url_path"), path, sizeof path);
                if (!strcmp(path, cfg_dashboard) || (!cfg_dashboard[0] && is(path, "lovelace"))) {
                    tdlog("hass: dashboard changed, reloading");
                    if (request_config(s) < 0) rc = -1;
                }
            }
    }
    else if (is(type, "result")) {
        int ok = json_bool(&d, json_get(&d, 0, "success"), 0);
        if (id == s->config_req) {
            rc = on_config(s, &d, 0);
        }
        else if (!ok) {
            /* a failed service call (or subscription): tell the user */
            char m[96] = "Home Assistant: request failed";
            json_str(&d, json_path(&d, 0, "error.message"), m, sizeof m);
            tdlog("hass: request %d failed: %s", id, m);
            pthread_mutex_lock(&g_ha.mu);
            toast_locked(m);
            pthread_mutex_unlock(&g_ha.mu);
            ui_wake();
        }
    }
done:
    free(t);
    return rc;
}

/* Waits for one text message (for the auth exchange). */
static char *recv_one(session *s, int timeout_ms, size_t *len)
{
    uint64_t end = now_ms() + (uint64_t)timeout_ms;
    while (now_ms() < end) {
        if (!(ws_wait(s->ws, -1, (int)(end - now_ms())) & WS_READY)) continue;
        char *m;
        int r = ws_recv(s->ws, &m, len);
        if (r < 0) return NULL;
        if (r == 1) return m;
    }
    return NULL;
}

/* Gives up on optimistic states that HA never confirmed (e.g. the call failed). */
static void expire_guesses(void)
{
    uint64_t now = now_ms();
    int changed = 0;
    pthread_mutex_lock(&g_ha.mu);
    for (int i = 0; i < g_ha.d.n_ents; i++) {
        ha_entity *e = &g_ha.d.ents[i];
        if (e->hold_until && now > e->hold_until) {
            str_copy(e->state, sizeof e->state, e->real_state);
            e->hold_until = 0;
            changed = 1;
        }
    }
    if (changed) g_ha.version++;
    pthread_mutex_unlock(&g_ha.mu);
    if (changed) ui_wake();
}

enum { END_DROPPED, END_AUTH, END_NET };

/* One connection: auth, load the dashboard, then serve events and the UI's queue. */
static int run_session(ws_conn *ws)
{
    session s = {.ws = ws, .next_id = 1};
    size_t len;
    char *m = recv_one(&s, 10000, &len);
    if (!m || !strstr(m, "auth_required")) {
        free(m);
        set_status(HA_ERROR, "Unexpected reply from Home Assistant");
        return END_DROPPED;
    }
    free(m);
    char auth[600];
    int n = snprintf(auth, sizeof auth, "{\"type\":\"auth\",\"access_token\":\"%s\"}", cfg_token);
    if (ws_send(ws, auth, (size_t)n) < 0) return END_DROPPED;
    m = recv_one(&s, 10000, &len);
    if (!m) return END_DROPPED;
    if (!strstr(m, "\"auth_ok\"")) {
        tdlog("hass: auth failed: %.120s", m);
        free(m);
        set_status(HA_AUTH_FAILED, "Home Assistant rejected the access token");
        return END_AUTH;
    }
    free(m);
    tdlog("hass: authenticated");
    set_status(HA_CONNECTING, "Loading dashboard...");

    s.lovelace_sub = send_msg(&s, "\"type\":\"subscribe_events\",\"event_type\":\"lovelace_updated\"");
    if (s.lovelace_sub < 0 || request_config(&s) < 0) return END_DROPPED;
    s.last_rx = s.last_ping = now_ms();

    for (;;) {
        int r = ws_wait(ws, wake_pipe[0], 1000);
        if (r & WS_EXTRA) {
            char junk[32];
            while (read(wake_pipe[0], junk, sizeof junk) > 0) {}
        }
        /* send what the UI queued */
        for (;;) {
            char body[768];
            pthread_mutex_lock(&g_ha.mu);
            int up = net_up, have = q_count > 0;
            if (have) {
                str_copy(body, sizeof body, queue[q_head]);
                q_head = (q_head + 1) % QLEN;
                q_count--;
            }
            pthread_mutex_unlock(&g_ha.mu);
            if (!up) return END_NET;
            if (!have) break;
            if (send_msg(&s, body) < 0) return END_DROPPED;
        }
        if (r & WS_READY) {
            char *msg;
            int rr = ws_recv(ws, &msg, &len);
            if (rr < 0) {
                tdlog("hass: connection closed");
                return END_DROPPED;
            }
            s.last_rx = now_ms();
            if (rr == 1) {
                int hr = on_message(&s, msg, len);
                free(msg);
                if (hr < 0) return END_DROPPED;
            }
        }
        /* keepalive: ping when idle, give up if HA stops answering */
        uint64_t now = now_ms();
        if (now - s.last_rx > 30000 && now - s.last_ping > 30000) {
            s.last_ping = now;
            if (send_msg(&s, "\"type\":\"ping\"") < 0) return END_DROPPED;
        }
        if (now - s.last_rx > 75000) {
            tdlog("hass: no reply for 75 s, reconnecting");
            return END_DROPPED;
        }
        expire_guesses();
    }
}

/* Sleeps up to ms, returning early when the network state changes. */
static void idle(int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    pthread_mutex_lock(&g_ha.mu);
    int was = net_up;
    pthread_mutex_unlock(&g_ha.mu);
    while (now_ms() < end) {
        usleep(200 * 1000);
        pthread_mutex_lock(&g_ha.mu);
        int up = net_up;
        pthread_mutex_unlock(&g_ha.mu);
        if (up != was) return;
    }
}

static void *worker(void *arg)
{
    int backoff = 2;
    for (;;) {
        pthread_mutex_lock(&g_ha.mu);
        int up = net_up;
        pthread_mutex_unlock(&g_ha.mu);
        if (!up) {
            if (g_ha.status != HA_WAIT_NETWORK) set_status(HA_WAIT_NETWORK, "Waiting for Wi-Fi");
            idle(60000);
            continue;
        }
        if (g_ha.status != HA_READY) set_status(HA_CONNECTING, "Connecting to %s", cfg_url);
        char err[160];
        ws_conn *ws = ws_connect(cfg_url, "/api/websocket", err, sizeof err);
        if (!ws) {
            tdlog("hass: %s", err);
            set_status(HA_ERROR, "%s", err);
            idle(backoff * 1000);
            backoff = backoff < 30 ? backoff * 2 : 60;
            continue;
        }
        int end = run_session(ws);
        ws_close(ws);
        if (end == END_AUTH) {
            idle(300000); /* a bad token won't fix itself; retry rarely */
        }
        else if (end == END_DROPPED) {
            if (g_ha.status == HA_READY) set_status(HA_CONNECTING, "Reconnecting to Home Assistant...");
            idle(2000);
        }
        backoff = 2;
    }
    return NULL;
}

void hass_start(void)
{
    str_copy(cfg_url, sizeof cfg_url, config_get("homeassistant", "url", ""));
    str_copy(cfg_token, sizeof cfg_token, config_get("homeassistant", "token", ""));
    str_copy(cfg_dashboard, sizeof cfg_dashboard, config_get("homeassistant", "dashboard", "dashboard-thermodeck"));
    str_copy(cfg_entities, sizeof cfg_entities, config_get("homeassistant", "entities", ""));
    /* strip a trailing slash from the URL */
    size_t ul = strlen(cfg_url);
    if (ul && cfg_url[ul - 1] == '/') cfg_url[ul - 1] = 0;
    if (!cfg_url[0] || !cfg_token[0]) {
        set_status(HA_OFF, "Add url and token under [homeassistant] in %s/thermodeck.ini", g_dir);
        return;
    }
    if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) < 0) tdlog("%s: no wake pipe", __func__);
    pthread_t th;
    pthread_create(&th, NULL, worker, NULL);
    pthread_detach(th);
}
