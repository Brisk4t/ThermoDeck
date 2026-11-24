/* Time zone picker, opened by tapping the clock. */

#include <time.h>

#include "core/common.h"
#include "ui/internal.h"

/* POSIX TZ strings (no zoneinfo needed). Ordered west to east. */
static const struct {
    const char *name, *places, *tz;
} ZONES[] = {
    {"Hawaii", "Honolulu", "HST10"},
    {"Alaska", "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific", "Vancouver, Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Mountain", "Edmonton, Calgary, Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Mountain (no DST)", "Arizona", "MST7"},
    {"Central", "Winnipeg, Chicago, Mexico City", "CST6CDT,M3.2.0,M11.1.0"},
    {"Central (no DST)", "Saskatchewan", "CST6"},
    {"Eastern", "Toronto, Montreal, New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Atlantic", "Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"Newfoundland", "St. John's", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Brazil / Argentina", "Sao Paulo, Buenos Aires", "<-03>3"},
    {"UTC", "Coordinated Universal Time", "UTC0"},
    {"UK / Ireland", "London, Dublin, Lisbon", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "Paris, Berlin, Madrid, Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "Athens, Helsinki, Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow / Istanbul", "Moscow, Istanbul, Riyadh", "<+03>-3"},
    {"Gulf", "Dubai, Abu Dhabi", "<+04>-4"},
    {"Pakistan", "Karachi, Tashkent", "<+05>-5"},
    {"India", "Delhi, Mumbai, Colombo", "IST-5:30"},
    {"Bangladesh", "Dhaka", "<+06>-6"},
    {"Indochina", "Bangkok, Jakarta, Hanoi", "<+07>-7"},
    {"China / Singapore", "Beijing, Singapore, Perth", "<+08>-8"},
    {"Japan / Korea", "Tokyo, Seoul", "JST-9"},
    {"Australia East", "Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Queensland", "Brisbane", "AEST-10"},
    {"New Zealand", "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
#define N_ZONES ((int)ARRAY_LEN(ZONES))

/* Index of tz in ZONES, or -1 for a custom zone from the ini. */
static int zone_index(const char *tz)
{
    for (int i = 0; i < N_ZONES; i++)
        if (!strcmp(ZONES[i].tz, tz)) return i;
    return -1;
}

/* Opens the time zone screen scrolled to the current zone. */
void open_tz(void)
{
    ui.screen = SCR_TZ;
    int cur = zone_index(ui.tz);
    ui.list_off = cur < 0 ? 0 : cur - LIST_ROWS / 2;
}

/*
 * Time zone picker: current time + 12/24 h toggle, then the zone list with each zone's
 * local time so the right one is easy to spot.
 */
void render_tz(void)
{
    draw_header("Time zone");
    time_t now = time(NULL);
    int cur = zone_index(ui.tz);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    ui.last_minute = now_tm.tm_min;

    /* current setting */
    char t[16];
    fmt_clock(now, t, sizeof t);
    gfx_rrect(8, 52, SCR_W - 16, 60, 12, C_CARD);
    gfx_text(&font_lg, 22, 81, t, C_TEXT);
    gfx_text_fit(&font_sm, 22, 101, SCR_W - 104 - 8 - 22, cur >= 0 ? ZONES[cur].name : ui.tz, C_SUB);
    button(BTN_TZ_24H, SCR_W - 104, 62, 88, 40, ui.clock_24h ? "24-hour" : "12-hour", &font_mdb, BS_NORMAL);

    /* zones, each with its local time right now */
    int n = N_ZONES;
    if (ui.list_off > n - LIST_ROWS) ui.list_off = n - LIST_ROWS;
    if (ui.list_off < 0) ui.list_off = 0;
    for (int r = 0; r < LIST_ROWS && ui.list_off + r < n; r++) {
        int i = ui.list_off + r;
        int y = LIST_Y + r * LIST_ROW_H;
        int sel = i == cur;
        if (is_pressed(BTN_ROW0 + r)) gfx_fill(0, y, SCR_W, LIST_ROW_H, 0x262626);
        gfx_fill(16, y + LIST_ROW_H - 1, SCR_W - 32, 1, C_LINE);
        fmt_clock_in(ZONES[i].tz, now, t, sizeof t);
        int tw = gfx_text_width(&font_mdb, t);
        gfx_text_right(&font_mdb, SCR_W - 18, y + 35, t, sel ? C_GREEN : C_SUB);
        int maxw = SCR_W - 18 - tw - 12 - 18;
        gfx_text_fit(&font_md, 18, y + 27, maxw, ZONES[i].name, sel ? C_GREEN : C_TEXT);
        gfx_text_fit(&font_sm, 18, y + 46, maxw, ZONES[i].places, C_DIM);
        add_hit(BTN_ROW0 + r, 0, y, SCR_W, LIST_ROW_H);
    }
    clock_apply(); /* back to the selected zone */
    int track = LIST_ROWS * LIST_ROW_H;
    gfx_rrect(SCR_W - 6, LIST_Y + track * ui.list_off / n, 3, track * LIST_ROWS / n, 1, 0x555555);

    int by = 412, bh = 60;
    button(BTN_LIST_UP, 6, by, 72, bh, NULL, &font_md, ui.list_off > 0 ? BS_NORMAL : BS_DISABLED);
    gfx_tri(42 - 10, by + 36, 42 + 10, by + 36, 42, by + 22, ui.list_off > 0 ? C_TEXT : C_DIM);
    button(BTN_DONE, 86, by, 148, bh, "Done", &font_mdb, BS_PRIMARY);
    int can_down = ui.list_off + LIST_ROWS < n;
    button(BTN_LIST_DOWN, 242, by, 72, bh, NULL, &font_md, can_down ? BS_NORMAL : BS_DISABLED);
    gfx_tri(278 - 10, by + 24, 278 + 10, by + 24, 278, by + 38, can_down ? C_TEXT : C_DIM);
}

/* A zone row (select it) or the 12/24-hour toggle. */
void press_tz(int id)
{
    if (id >= BTN_ROW0 && id < BTN_ROW0 + LIST_ROWS) {
        int idx = ui.list_off + (id - BTN_ROW0);
        if (idx < N_ZONES && strcmp(ui.tz, ZONES[idx].tz)) {
            str_copy(ui.tz, sizeof ui.tz, ZONES[idx].tz);
            clock_apply();
            settings_save();
            tdlog("clock: time zone set to %s (%s)", ZONES[idx].name, ui.tz);
        }
        return;
    }
    if (id == BTN_TZ_24H) {
        ui.clock_24h = !ui.clock_24h;
        settings_save();
    }
}
