/*
 * Per-domain knowledge about Home Assistant entities: what counts as "on", what a tap does,
 * how the state reads, and which icon / color to use. Mirrors HA's own defaults closely
 * enough that a dashboard looks familiar.
 */

#include <ctype.h>
#include <stdlib.h>

#include "core/common.h"
#include "hass/hass.h"

/* entity_id "light.kitchen" -> "light" (into a small buffer) */
static const char *domain_of(const ha_entity *e, char *buf, size_t len)
{
    size_t n = strcspn(e->id, ".");
    if (n >= len) n = len - 1;
    memcpy(buf, e->id, n);
    buf[n] = 0;
    return buf;
}

static int is(const char *a, const char *b) { return !strcmp(a, b); }

/* HA's weather conditions with their English wording and icons (as in HA's frontend). */
static const struct {
    const char *cond, *label, *icon;
} WEATHER[] = {
    {"clear-night", "Clear, night", "weather-night"},
    {"cloudy", "Cloudy", "weather-cloudy"},
    {"exceptional", "Exceptional", "alert-circle-outline"},
    {"fog", "Fog", "weather-fog"},
    {"hail", "Hail", "weather-hail"},
    {"lightning", "Lightning", "weather-lightning"},
    {"lightning-rainy", "Lightning, rainy", "weather-lightning-rainy"},
    {"partlycloudy", "Partly cloudy", "weather-partly-cloudy"},
    {"pouring", "Pouring", "weather-pouring"},
    {"rainy", "Rainy", "weather-rainy"},
    {"snowy", "Snowy", "weather-snowy"},
    {"snowy-rainy", "Snowy, rainy", "weather-snowy-rainy"},
    {"sunny", "Sunny", "weather-sunny"},
    {"windy", "Windy", "weather-windy"},
    {"windy-variant", "Windy, cloudy", "weather-windy-variant"},
};

const char *ha_weather_label(const char *c)
{
    for (size_t i = 0; i < ARRAY_LEN(WEATHER); i++)
        if (is(c, WEATHER[i].cond)) return WEATHER[i].label;
    return c;
}

const char *ha_weather_icon(const char *c)
{
    for (size_t i = 0; i < ARRAY_LEN(WEATHER); i++)
        if (is(c, WEATHER[i].cond)) return WEATHER[i].icon;
    return "weather-partly-cloudy";
}

const char *ha_entity_attr(const ha_entity *e, const char *key)
{
    for (int i = 0; i < e->n_attrs; i++)
        if (is(e->attrs[i].key, key)) return e->attrs[i].val[0] ? e->attrs[i].val : NULL;
    return NULL;
}

int ha_entity_is_on(const ha_entity *e)
{
    char d[32];
    const char *dom = domain_of(e, d, sizeof d);
    const char *s = e->state;
    if (is(dom, "cover")) return is(s, "open") || is(s, "opening") || is(s, "closing");
    if (is(dom, "lock")) return is(s, "unlocked") || is(s, "open");
    if (is(dom, "media_player")) return is(s, "playing");
    if (is(dom, "climate")) return s[0] && !is(s, "off") && !is(s, "unavailable");
    if (is(dom, "person") || is(dom, "device_tracker")) return is(s, "home");
    if (is(dom, "vacuum")) return is(s, "cleaning");
    if (is(dom, "alarm_control_panel")) return !strncmp(s, "armed", 5) || is(s, "triggered");
    return is(s, "on");
}

int ha_entity_toggles(const ha_entity *e)
{
    /* (not climate or lock: one tap shouldn't switch off the heating or unlock a door) */
    static const char *const T[] = {"light", "switch", "fan",   "input_boolean", "automation",
                                    "siren", "remote", "cover", "humidifier",    "group"};
    char d[32];
    const char *dom = domain_of(e, d, sizeof d);
    for (size_t i = 0; i < ARRAY_LEN(T); i++)
        if (is(dom, T[i])) return 1;
    return 0;
}

/* Numeric state with at most 1 decimal ("21.4567" -> "21.5"); copies other states as-is. */
static void fmt_number(const char *s, char *out, size_t len)
{
    char *end;
    double v = strtod(s, &end);
    const char *dot = strchr(s, '.');
    if (end != s && !*end && dot && strlen(dot + 1) > 2)
        snprintf(out, len, "%.1f", v);
    else
        str_copy(out, len, s);
}

/* "not_home" -> "Not home" */
static void humanize(const char *s, char *out, size_t len)
{
    str_copy(out, len, s);
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
    out[0] = (char)toupper((unsigned char)out[0]);
}

void ha_entity_state_text(const ha_entity *e, char *out, size_t len)
{
    char d[32];
    const char *dom = domain_of(e, d, sizeof d), *s = e->state, *dc = e->device_class;
    if (!e->has_state) {
        str_copy(out, len, "...");
        return;
    }
    if (is(s, "unavailable") || is(s, "unknown")) {
        humanize(s, out, len);
        return;
    }
    if (is(dom, "binary_sensor")) {
        /* the words HA uses for each device class */
        static const struct {
            const char *dc, *on, *off;
        } B[] = {
            {"door", "Open", "Closed"},
            {"garage_door", "Open", "Closed"},
            {"window", "Open", "Closed"},
            {"opening", "Open", "Closed"},
            {"motion", "Detected", "Clear"},
            {"occupancy", "Detected", "Clear"},
            {"presence", "Home", "Away"},
            {"moisture", "Wet", "Dry"},
            {"smoke", "Detected", "Clear"},
            {"gas", "Detected", "Clear"},
            {"carbon_monoxide", "Detected", "Clear"},
            {"problem", "Problem", "OK"},
            {"safety", "Unsafe", "Safe"},
            {"lock", "Unlocked", "Locked"},
            {"battery", "Low", "Normal"},
            {"connectivity", "Connected", "Disconnected"},
            {"plug", "Plugged in", "Unplugged"},
            {"power", "Power", "No power"},
            {"running", "Running", "Not running"},
        };
        int on = is(s, "on");
        for (size_t i = 0; i < ARRAY_LEN(B); i++)
            if (is(dc, B[i].dc)) {
                str_copy(out, len, on ? B[i].on : B[i].off);
                return;
            }
        str_copy(out, len, on ? "On" : "Off");
        return;
    }
    if (is(dom, "weather")) {
        str_copy(out, len, ha_weather_label(s));
        return;
    }
    /* scenes and buttons store when they last ran: say what they are instead */
    if (is(dom, "scene")) {
        str_copy(out, len, "Scene");
        return;
    }
    if (is(dom, "button") || is(dom, "input_button")) {
        str_copy(out, len, "Button");
        return;
    }
    if (is(dom, "script")) {
        str_copy(out, len, is(s, "on") ? "Running" : "Script");
        return;
    }
    if (is(dom, "light") && is(s, "on") && e->brightness >= 0) {
        snprintf(out, len, "On \xC2\xB7 %d%%", (e->brightness * 100 + 127) / 255);
        return;
    }
    if (is(dom, "person") || is(dom, "device_tracker")) {
        if (is(s, "not_home"))
            str_copy(out, len, "Away");
        else
            humanize(s, out, len);
        return;
    }
    if (is(dom, "media_player") && e->extra[0] && (is(s, "playing") || is(s, "paused"))) {
        str_copy(out, len, e->extra);
        return;
    }
    if (e->unit[0]) {
        /* "21.5 °C", "45%" */
        char num[32];
        fmt_number(s, num, sizeof num);
        int tight = is(e->unit, "%");
        snprintf(out, len, "%s%s%s", num, tight ? "" : " ", e->unit);
        return;
    }
    char h[48];
    fmt_number(s, h, sizeof h);
    if (!strcmp(h, s)) humanize(s, h, sizeof h);
    if (e->extra[0] && (is(dom, "climate") || (is(dom, "cover") && ha_entity_is_on(e))))
        snprintf(out, len, "%s \xC2\xB7 %s", h, e->extra);
    else
        str_copy(out, len, h);
}

const char *ha_entity_icon(const ha_entity *e)
{
    if (e->icon[0]) return e->icon;
    char d[32];
    const char *dom = domain_of(e, d, sizeof d), *dc = e->device_class;
    int on = ha_entity_is_on(e);
    if (is(dom, "light")) return on ? "lightbulb" : "lightbulb-outline";
    if (is(dom, "switch"))
        return is(dc, "outlet") ? (on ? "power-plug" : "power-plug-off")
                                : (on ? "toggle-switch-variant" : "toggle-switch-variant-off");
    if (is(dom, "input_boolean")) return on ? "toggle-switch" : "toggle-switch-off";
    if (is(dom, "fan")) return on ? "fan" : "fan-off";
    if (is(dom, "cover")) {
        if (is(dc, "garage")) return on ? "garage-open" : "garage";
        if (is(dc, "gate")) return on ? "gate-open" : "gate";
        if (is(dc, "door")) return on ? "door-open" : "door-closed";
        if (is(dc, "blind") || is(dc, "shade")) return on ? "blinds-open" : "blinds";
        if (is(dc, "curtain")) return on ? "curtains" : "curtains-closed";
        return on ? "window-shutter-open" : "window-shutter";
    }
    if (is(dom, "lock")) return on ? "lock-open-variant" : "lock";
    if (is(dom, "climate")) return "thermostat";
    if (is(dom, "water_heater")) return "water-boiler";
    if (is(dom, "humidifier")) return "air-humidifier";
    if (is(dom, "scene")) return "palette";
    if (is(dom, "script")) return "script-text";
    if (is(dom, "automation")) return on ? "robot" : "robot-off";
    if (is(dom, "button") || is(dom, "input_button")) return "gesture-tap-button";
    if (is(dom, "media_player")) return is(dc, "tv") ? "television" : on ? "speaker" : "speaker-off";
    if (is(dom, "person") || is(dom, "device_tracker")) return on ? "account" : "account-arrow-right";
    if (is(dom, "sun")) return is(e->state, "below_horizon") ? "weather-night" : "white-balance-sunny";
    if (is(dom, "weather")) return ha_weather_icon(e->state);
    if (is(dom, "camera")) return "video";
    if (is(dom, "vacuum")) return "robot-vacuum";
    if (is(dom, "alarm_control_panel")) return on ? "shield-lock" : "shield-off";
    if (is(dom, "siren")) return "bullhorn";
    if (is(dom, "remote")) return "remote";
    if (is(dom, "zone")) return "map-marker";
    if (is(dom, "counter")) return "counter";
    if (is(dom, "timer")) return "timer-outline";
    if (is(dom, "update")) return "package-up";
    if (is(dom, "input_number") || is(dom, "number")) return "ray-vertex";
    if (is(dom, "input_select") || is(dom, "select")) return "format-list-bulleted";
    if (is(dom, "group")) return "google-circles-communities";
    if (is(dom, "binary_sensor")) {
        static const struct {
            const char *dc, *on, *off;
        } B[] = {
            {"door", "door-open", "door-closed"},
            {"garage_door", "garage-open", "garage"},
            {"window", "window-open", "window-closed"},
            {"opening", "door-open", "door-closed"},
            {"motion", "motion-sensor", "motion-sensor-off"},
            {"occupancy", "home", "home-outline"},
            {"presence", "home", "home-outline"},
            {"moisture", "water", "water-off"},
            {"smoke", "smoke-detector-variant-alert", "smoke-detector-variant"},
            {"connectivity", "wifi", "wifi-off"},
            {"battery", "battery-low", "battery"},
            {"lock", "lock-open-variant", "lock"},
            {"plug", "power-plug", "power-plug-off"},
            {"problem", "alert-circle", "checkbox-marked-circle"},
        };
        for (size_t i = 0; i < ARRAY_LEN(B); i++)
            if (is(dc, B[i].dc)) return on ? B[i].on : B[i].off;
        return on ? "checkbox-marked-circle" : "radiobox-blank";
    }
    if (is(dom, "sensor")) {
        static const struct {
            const char *dc, *icon;
        } S[] = {{"temperature", "thermometer"},
                 {"humidity", "water-percent"},
                 {"power", "flash"},
                 {"energy", "lightning-bolt"},
                 {"battery", "battery"},
                 {"illuminance", "brightness-5"},
                 {"pressure", "gauge"},
                 {"carbon_dioxide", "molecule-co2"},
                 {"carbon_monoxide", "molecule-co"},
                 {"signal_strength", "wifi"},
                 {"timestamp", "clock-outline"},
                 {"date", "calendar"}};
        for (size_t i = 0; i < ARRAY_LEN(S); i++)
            if (is(dc, S[i].dc)) return S[i].icon;
        return "eye";
    }
    return "bookmark";
}

uint32_t ha_entity_color(const ha_entity *e)
{
    char d[32];
    const char *dom = domain_of(e, d, sizeof d);
    if (is(dom, "light")) return 0xFFC107;
    if (is(dom, "cover")) return 0x9C7BE0;
    if (is(dom, "lock") || is(dom, "alarm_control_panel")) return 0xE5534B;
    if (is(dom, "climate")) return is(e->state, "cool") ? 0x3AA0E8 : 0xFF8A3D;
    if (is(dom, "binary_sensor")) return 0xFF9800;
    if (is(dom, "media_player") || is(dom, "person") || is(dom, "device_tracker")) return 0x1DB954;
    return 0x3AA0E8;
}
