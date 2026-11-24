/*
 * Weather forecast card ("weather-forecast"), like HA's: current conditions (icon,
 * condition, name, temperature) above a row of forecast days / hours.
 *
 * Options: entity, name, show_current, show_forecast, forecast_type (daily, hourly,
 * twice_daily), secondary_info_attribute. Current values come from tracked attributes;
 * the forecast comes from a weather/subscribe_forecast subscription (it->opt).
 */

#include <time.h>

#include "cards/card.h"
#include "core/common.h"
#include "gfx/icons.h"
#include "ui/theme.h"

enum { WX_CURRENT = 1, WX_FORECAST = 2 };

#define FC_COL_W  52  /* narrowest forecast column */
#define COMPACT_W 220 /* narrower than this: icon and temperature on one line, text below */

static const char *const TYPES[] = {"weather-forecast", NULL};

/* The attribute holding the unit of a weather attribute ("wind_speed" -> "wind_speed_unit"). */
static const char *unit_attr(const char *attr)
{
    static const char *const U[][2] = {
        {"temperature", "temperature_unit"}, {"apparent_temperature", "temperature_unit"},
        {"dew_point", "temperature_unit"},   {"pressure", "pressure_unit"},
        {"wind_speed", "wind_speed_unit"},   {"wind_gust_speed", "wind_speed_unit"},
        {"visibility", "visibility_unit"},   {"precipitation", "precipitation_unit"},
    };
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++)
        if (!strcmp(attr, U[i][0])) return U[i][1];
    return NULL;
}

/* A number as HA shows it: as reported, but at most one decimal ("68" stays "68"). */
static void fmt_value(const char *lit, char *out, size_t len)
{
    const char *dot = strchr(lit, '.');
    if (dot && strlen(dot + 1) > 1)
        card_number(lit, 1, out, len);
    else
        str_copy(out, len, lit);
}

static void options(card_parser *p, int card, ha_item *it)
{
    if (card_bool(p, card, "show_current", 1)) it->flags |= WX_CURRENT;
    if (card_bool(p, card, "show_forecast", 1)) it->flags |= WX_FORECAST;
    card_want_attr(p, it->ent, "temperature");
    card_want_attr(p, it->ent, "temperature_unit");
    /* the optional line under the temperature, e.g. humidity: remember which attribute */
    card_str(p, card, "secondary_info_attribute", it->text, sizeof it->text);
    if (it->text[0]) {
        card_want_attr(p, it->ent, it->text);
        const char *u = unit_attr(it->text);
        if (u) card_want_attr(p, it->ent, u);
    }
    if (it->flags & WX_FORECAST) {
        char type[16];
        card_str(p, card, "forecast_type", type, sizeof type);
        it->opt = (int16_t)card_want_forecast(p, it->ent, type[0] ? type : "daily");
    }
    /* smaller when showing only one part, unless the dashboard sized it */
    if (json_path(card_json(p), card, "grid_options.rows") < 0 &&
        (it->flags & (WX_CURRENT | WX_FORECAST)) != (WX_CURRENT | WX_FORECAST))
        it->rows = 2;
}

/* Secondary attribute (secondary_info_attribute) with its unit, e.g. "68 %"; "" if none. */
static void secondary(const card_ctx *c, char *out, size_t len)
{
    out[0] = 0;
    const char *key = c->it->text, *v = key[0] ? ha_entity_attr(c->e, key) : NULL;
    if (!v) return;
    char num[24];
    fmt_value(v, num, sizeof num);
    const char *ua = unit_attr(key), *u = ua ? ha_entity_attr(c->e, ua) : NULL;
    if (!strcmp(key, "humidity") || !strcmp(key, "cloud_coverage")) u = "%";
    snprintf(out, len, "%s%s%s", num, u ? " " : "", u ? u : "");
}

/*
 * Current conditions; returns the height used. Wide: icon, condition + name, temperature on
 * the right. Narrow: icon and temperature on one line, condition and name below.
 */
static int draw_current(const card_ctx *c, int top, int room_for_name)
{
    const ha_entity *e = c->e;
    int na = card_unavailable(c) || !e->has_state;
    const char *cond = na ? "Unavailable" : ha_weather_label(e->state);
    icon_draw(&icons_36, icon_find(ha_weather_icon(e->state)), c->x + 32, top + 32, na ? C_DIM : C_TEXT);

    /* temperature, right-aligned: number large, unit small */
    char t[24] = "", sec[40];
    const char *tv = ha_entity_attr(e, "temperature"), *tu = ha_entity_attr(e, "temperature_unit");
    if (tv && !na) fmt_value(tv, t, sizeof t);
    secondary(c, sec, sizeof sec);
    if (na) sec[0] = 0;
    int compact = c->w < COMPACT_W;
    const font_t *tf = compact ? &font_lg : &font_xl;
    int uw = gfx_text_width(&font_md, tu ? tu : ""), tw = gfx_text_width(tf, t);
    int rx = c->x + c->w - 14;
    if (t[0]) {
        gfx_text_right(&font_md, rx, top + 30, tu ? tu : "", C_SUB);
        gfx_text_right(tf, rx - uw - 2, top + 40, t, C_TEXT);
    }
    if (compact) {
        /* condition, then name (and the secondary value) on their own lines */
        int tx = c->x + 14, w = c->w - 28;
        card_text_shrink(tx, top + 70, w, cond, C_TEXT);
        if (!room_for_name) return 76;
        /* no room for both: the secondary value says more than the name here */
        gfx_text_fit(&font_sm, tx, top + 89, w, sec[0] ? sec : card_name(c), C_SUB);
        return 96;
    }
    if (sec[0]) gfx_text_right(&font_sm, rx, top + 58, sec, C_SUB);
    /* condition and name, left of the temperature */
    int tx = c->x + 62, w = rx - (t[0] ? tw + uw + 12 : 0) - tx;
    card_text_shrink(tx, top + 28, w, cond, C_TEXT);
    gfx_text_fit(&font_sm, tx, top + 49, w, card_name(c), C_SUB);
    return 68;
}

/* Column heading for a forecast entry: weekday, or the hour for hourly forecasts. */
static void entry_label(const ha_forecast *f, const ha_forecast_entry *fe, int clock_24h, char *out, size_t len)
{
    time_t t = (time_t)fe->time;
    struct tm tm;
    localtime_r(&t, &tm);
    if (!strcmp(f->type, "hourly")) {
        if (clock_24h) {
            strftime(out, len, "%H:%M", &tm);
        }
        else {
            strftime(out, len, "%I %p", &tm);
            if (out[0] == '0') memmove(out, out + 1, strlen(out));
        }
    }
    else {
        strftime(out, len, "%a", &tm);
    }
}

/* Forecast row: as many columns as fit, each with label, icon, high and low. */
static void draw_forecast(const card_ctx *c, int top)
{
    const ha_forecast *f = c->it->opt >= 0 && c->it->opt < c->d->n_fcs ? &c->d->fcs[c->it->opt] : NULL;
    if (!f || !f->n) {
        gfx_text_center(&font_sm, c->x + c->w / 2, top + 40, "Loading forecast...", C_DIM);
        return;
    }
    int n = (c->w - 16) / FC_COL_W;
    if (n > f->n) n = f->n;
    if (n < 1) n = 1;
    int cw = (c->w - 16) / n;
    for (int i = 0; i < n; i++) {
        const ha_forecast_entry *fe = &f->e[i];
        int cx = c->x + 8 + i * cw + cw / 2;
        char label[16], hi[16], lo[16];
        entry_label(f, fe, c->clock_24h, label, sizeof label);
        card_text_center(&font_sm, cx, top + 13, cw - 4, label, C_SUB);
        icon_draw(&icons_24, icon_find(ha_weather_icon(fe->condition)), cx, top + 31, C_TEXT);
        card_number(fe->temp, 0, hi, sizeof hi);
        card_number(fe->templow, 0, lo, sizeof lo);
        if (hi[0]) strcat(hi, "\xC2\xB0");
        if (lo[0]) strcat(lo, "\xC2\xB0");
        card_text_center(&font_sm, cx, top + 56, cw - 4, hi, C_TEXT);
        if (lo[0]) card_text_center(&font_sm, cx, top + 71, cw - 4, lo, C_DIM);
    }
}

static void draw(const card_ctx *c)
{
    card_bg(c);
    if (!c->e) return;
    int top = c->y;
    if (c->it->flags & WX_CURRENT) top += draw_current(c, top, !(c->it->flags & WX_FORECAST) || c->h >= 96 + 84);
    if (c->it->flags & WX_FORECAST) {
        if (c->it->flags & WX_CURRENT) gfx_fill(c->x + 12, top, c->w - 24, 1, C_LINE);
        draw_forecast(c, top + 4);
    }
}

const card_def card_weather = {
    .name = "weather-forecast",
    .types = TYPES,
    .span = 12,
    .rows = 3,
    .options = options,
    .draw = draw,
};
