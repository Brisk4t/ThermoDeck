/* Icon lookup by name and drawing (bitmaps come from gen/icon_data.c). */

#include "gfx/icons.h"

#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"

static int cmp_name(const void *key, const void *elem) { return strcmp(key, *(const char *const *)elem); }

int icon_find(const char *name)
{
    if (!name || !name[0]) return -1;
    if (!strncmp(name, "mdi:", 4)) name += 4;
    const char *const *hit = bsearch(name, icon_names, (size_t)icon_count, sizeof icon_names[0], cmp_name);
    return hit ? (int)(hit - icon_names) : -1;
}

void icon_draw(const font_t *set, int idx, int cx, int cy, uint32_t c)
{
    if (idx < 0 || idx >= icon_count) return;
    const glyph_t *g = &set->glyphs[idx];
    int x0 = cx - set->size / 2 + g->xoff, y0 = cy - set->size / 2 + g->yoff;
    const uint8_t *bm = set->bits + g->off;
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++)
            if (bm[y * g->w + x]) gfx_blend_px(x0 + x, y0 + y, c, bm[y * g->w + x]);
}
