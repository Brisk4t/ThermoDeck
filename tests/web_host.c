/* Host harness: runs the real web server + Spotify worker without the UI. The login PIN and
 * any software update PIN are printed instead of shown on a screen.
 *   make tests && ./build-host/tests/web_host <dir with thermodeck.ini> */
#include <unistd.h>
#include "core/common.h"
#include "core/config.h"
#include "spotify/spotify.h"
#include "ui/ui.h"
#include "web/ota.h"
#include "web/web.h"
void ui_wake(void) {}
int ui_screen_private(void) { return 0; }
int ui_render_hass_page(int page, uint32_t *out, int timeout_ms) { return -1; }
int main(int argc, char **argv)
{
    str_copy(g_dir, sizeof g_dir, argv[1]);
    char p[300];
    path_join(p, sizeof p, "thermodeck.ini");
    config_load(p);
    spotify_start();
    web_start(config_int("web", "port", 8080));
    spotify_set_net(1, "10.0.0.5", "TestNet", -60);
    sleep(2);
    char pin[7];
    web_get_pin(pin);
    printf("PIN=%s\n", pin);
    fflush(stdout);
    /* stands in for the screen: print each software update PIN as it appears */
    char shown[7] = "";
    for (;;) {
        ota_view v;
        ota_get_view(&v);
        if (strcmp(v.pin, shown)) {
            memcpy(shown, v.pin, sizeof shown);
            if (shown[0]) printf("OTA PIN=%s from %s\n", shown, v.from);
            fflush(stdout);
        }
        usleep(100000);
    }
}
