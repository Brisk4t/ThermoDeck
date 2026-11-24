#ifndef GFX_ICONS_H
#define GFX_ICONS_H

/* Material Design Icons (the subset in tools/mdi_icons.txt), as alpha bitmaps. */

#include <stdint.h>

#include "gfx/font.h"

/* generated (gen/icon_data.c): glyph i of each set is icon_names[i] */
extern const int icon_count;
extern const char *const icon_names[];
extern const font_t icons_24, icons_36;

/* Index of an icon by name ("mdi:lightbulb" or "lightbulb"), or -1 if it isn't compiled in. */
int icon_find(const char *name);
/* Draws icon idx from set (icons_24 / icons_36) centered on (cx, cy). */
void icon_draw(const font_t *set, int idx, int cx, int cy, uint32_t c);

#endif
