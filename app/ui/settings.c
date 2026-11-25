/* User settings (time zone, 12/24 h clock, brightness) and clock formatting. */

#include <stdlib.h>
#include <time.h>

#include "backplate/backplate.h"
#include "core/common.h"
#include "core/config.h"
#include "gfx/gfx.h"
#include "ui/internal.h"

/* Applies the selected zone to this process (localtime uses TZ). */
void clock_apply(void)
{
    setenv("TZ", ui.tz[0] ? ui.tz : "UTC0", 1);
    tzset();
}

/* Sets the backlight (skips the sysfs writes if nothing changed). */
void brightness_apply(int pct)
{
    static int applied = -1;
    pct = CLAMP(pct, BRIGHT_MIN, 100);
    if (pct != applied) {
        gfx_set_backlight(pct);
        applied = pct;
    }
}

/* Undims the screen and restarts the timeout. */
void screen_activity(void)
{
    ui.last_active_ms = now_ms();
    if (ui.dimmed) {
        ui.dimmed = 0;
        brightness_apply(ui.brightness);
    }
}

/* Called from ui_update(): proximity counts as activity; dim once the timeout runs out. */
void screen_timeout_tick(void)
{
    uint64_t near = backplate_near_ms();
    if (near > ui.last_active_ms) screen_activity();
    if (!ui.dimmed && ui.dim_after_s > 0 && now_ms() - ui.last_active_ms > (uint64_t)ui.dim_after_s * 1000) {
        ui.dimmed = 1;
        brightness_apply(ui.dim_pct < ui.brightness ? ui.dim_pct : ui.brightness);
    }
}

/* User settings changed on the device live in settings.conf and override thermodeck.ini.
 * (Older builds wrote clock.conf; it's read once if settings.conf doesn't exist yet.) */
void settings_load(void)
{
    str_copy(ui.tz, sizeof ui.tz, config_get("display", "timezone", "UTC0"));
    ui.clock_24h = config_int("display", "clock_24h", 0);
    ui.brightness = config_int("display", "brightness", 80);
    ui.dim_after_s = config_int("display", "dim_after", 30);
    ui.dim_pct = CLAMP(config_int("display", "dim_brightness", 10), BRIGHT_MIN, 100);
    char p[300];
    path_join(p, sizeof p, "settings.conf");
    char *d = file_read(p, NULL);
    if (!d) {
        path_join(p, sizeof p, "clock.conf");
        d = file_read(p, NULL);
    }
    if (d) {
        for (char *line = strtok(d, "\n"); line; line = strtok(NULL, "\n")) {
            if (!strncmp(line, "tz=", 3))
                str_copy(ui.tz, sizeof ui.tz, line + 3);
            else if (!strncmp(line, "clock_24h=", 10))
                ui.clock_24h = atoi(line + 10);
            else if (!strncmp(line, "brightness=", 11))
                ui.brightness = atoi(line + 11);
        }
        free(d);
    }
    ui.brightness = CLAMP(ui.brightness, BRIGHT_MIN, 100);
    clock_apply();
    tdlog("settings: TZ=%s, %d-hour, brightness %d%%, dim to %d%% after %d s", ui.tz, ui.clock_24h ? 24 : 12,
          ui.brightness, ui.dim_pct, ui.dim_after_s);
}

void settings_save(void)
{
    char p[300], buf[160];
    path_join(p, sizeof p, "settings.conf");
    int n = snprintf(buf, sizeof buf, "tz=%s\nclock_24h=%d\nbrightness=%d\n", ui.tz, ui.clock_24h, ui.brightness);
    if (file_write_atomic(p, buf, (size_t)n, 0644) < 0) tdlog("settings: could not save %s", p);
}

/* "6:45 AM" / "06:45" for time t in the current TZ. */
void fmt_clock(time_t t, char *out, size_t outlen)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (ui.clock_24h)
        strftime(out, outlen, "%H:%M", &tm);
    else {
        strftime(out, outlen, "%I:%M %p", &tm);
        if (out[0] == '0') memmove(out, out + 1, strlen(out));
    }
}

/* Same, but in another zone (briefly switches TZ; only the UI thread uses local time). */
void fmt_clock_in(const char *tz, time_t t, char *out, size_t outlen)
{
    setenv("TZ", tz, 1);
    tzset();
    fmt_clock(t, out, outlen);
}
