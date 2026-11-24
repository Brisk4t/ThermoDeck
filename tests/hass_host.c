/* Host check of the Home Assistant client against tools/mock_hass.py:
 *   tools/mock_hass.py &
 *   make tests && ./build-host/tests/hass_host [url] [token]
 * Connects, loads the dashboard, waits for entity states, toggles a light and checks
 * that the change comes back from the server. */
#include <assert.h>
#include <unistd.h>

#include "core/common.h"
#include "core/config.h"
#include "hass/hass.h"
#include "ui/ui.h"

void ui_wake(void) {}

static ha_status wait_status(int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 100; i++) {
        pthread_mutex_lock(&g_ha.mu);
        ha_status s = g_ha.status;
        pthread_mutex_unlock(&g_ha.mu);
        if (s == HA_READY || s == HA_AUTH_FAILED || s == HA_NO_DASHBOARD) return s;
        usleep(100000);
    }
    return g_ha.status;
}

static int find_item(const char *entity)
{
    for (int i = 0; i < g_ha.d.n_items; i++)
        if (g_ha.d.items[i].ent >= 0 && !strcmp(g_ha.d.ents[g_ha.d.items[i].ent].id, entity)) return i;
    return -1;
}

static const char *state_of(const char *entity)
{
    for (int i = 0; i < g_ha.d.n_ents; i++)
        if (!strcmp(g_ha.d.ents[i].id, entity)) return g_ha.d.ents[i].state;
    return "?";
}

int main(int argc, char **argv)
{
    char ini[256];
    snprintf(ini, sizeof ini, "/tmp/hass_host_%d.ini", (int)getpid());
    FILE *f = fopen(ini, "w");
    fprintf(f, "[homeassistant]\nurl = %s\ntoken = %s\n", argc > 1 ? argv[1] : "http://127.0.0.1:8123",
            argc > 2 ? argv[2] : "test-token");
    fclose(f);
    config_load(ini);
    unlink(ini);

    hass_start();
    hass_set_net(1);
    ha_status s = wait_status(10000);
    printf("status %d: %s\n", s, g_ha.msg);
    assert(s == HA_READY);
    printf("%d items, %d entities\n", g_ha.d.n_items, g_ha.d.n_ents);

    /* states arrive right after the subscription */
    for (int i = 0; i < 30 && !g_ha.d.ents[0].has_state; i++) usleep(100000);
    pthread_mutex_lock(&g_ha.mu);
    for (int i = 0; i < g_ha.d.n_ents; i++) {
        char st[64];
        ha_entity_state_text(&g_ha.d.ents[i], st, sizeof st);
        printf("  %-32s %-12s %-20s icon=%s\n", g_ha.d.ents[i].id, g_ha.d.ents[i].state, st,
               ha_entity_icon(&g_ha.d.ents[i]));
    }
    pthread_mutex_unlock(&g_ha.mu);
    assert(!strcmp(state_of("light.floor_lamp"), "off"));

    /* tap the floor lamp tile: optimistic "on", then confirmed by the server */
    int item = find_item("light.floor_lamp");
    assert(item >= 0);
    hass_press(item);
    printf("after tap: %s\n", state_of("light.floor_lamp"));
    assert(!strcmp(state_of("light.floor_lamp"), "on"));
    for (int i = 0; i < 20; i++) {
        usleep(100000);
        pthread_mutex_lock(&g_ha.mu);
        int confirmed = 0;
        for (int k = 0; k < g_ha.d.n_ents; k++)
            if (!strcmp(g_ha.d.ents[k].id, "light.floor_lamp"))
                confirmed = !strcmp(g_ha.d.ents[k].real_state, "on") && g_ha.d.ents[k].brightness == 200;
        pthread_mutex_unlock(&g_ha.mu);
        if (confirmed) {
            printf("confirmed by HA after %d ms\n", (i + 1) * 100);
            hass_press(item); /* and back off, to leave the mock as it was */
            usleep(500000);
            printf("OK\n");
            return 0;
        }
    }
    printf("state never confirmed\n");
    return 1;
}
