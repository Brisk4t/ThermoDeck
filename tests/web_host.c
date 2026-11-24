/* Host harness: runs the real web server + Spotify worker without the UI. */
#include <unistd.h>
#include "common.h"
#include "config.h"
#include "spotify.h"
#include "web.h"
void ui_wake(void) {}
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
    for (;;) pause();
}
