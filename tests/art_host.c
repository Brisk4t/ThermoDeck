/* Host check: fetch + decode + scale one image through art.c, write it as PPM. */
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
