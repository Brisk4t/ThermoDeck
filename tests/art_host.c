/* Host check: fetch + decode + scale one image through art.c, write it as PPM.
 * gcc -O1 -g -D_GNU_SOURCE -pthread -Isrc -Ithird_party/bearssl/inc -Ithird_party/stb -o build-host/art_host tests/art_host.c \
 *     src/art.c src/gfx.c src/spotify.c src/util.c src/https.c src/json.c src/config.c gen/trust_anchors.c build-host/libbearssl.a -lm
 * ./build-host/art_host <image url> out.ppm   (then tools/ppm2png.py) */
#include <stdlib.h>
#include <unistd.h>
#include "art.h"
#include "common.h"
#include "gfx.h"
void ui_wake(void) {}
int main(int argc, char **argv)
{
    art_start();
    art_request(argv[1]);
    for (int i = 0; i < 100; i++) {
        usleep(100000);
        static uint32_t buf[ART_PX * ART_PX];
        (void)buf;
        if (art_draw(0, 0, 10)) {
            printf("art ready after %d ms\n", i * 100);
            gfx_dump_ppm(argv[2]);
            return 0;
        }
    }
    printf("art not ready\n");
    return 1;
}
