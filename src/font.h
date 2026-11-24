#ifndef FONT_H
#define FONT_H
#include <stdint.h>

typedef struct {
    uint8_t w, h;     /* bitmap size */
    int8_t xoff;      /* left bearing from pen x */
    int8_t yoff;      /* top of bitmap relative to baseline (negative = above) */
    uint8_t adv;      /* horizontal advance */
    uint32_t off;     /* offset in bits[] (w*h bytes of 8-bit alpha) */
} glyph_t;

typedef struct {
    uint8_t size, ascent, descent, line_h;
    const glyph_t *glyphs;
    const uint8_t *bits;
} font_t;

extern const font_t font_sm, font_md, font_mdb, font_lg, font_xl;

/* Map a Unicode code point to a glyph index (ASCII 32..126, Latin-1 160..255).
 * Common typographic characters are folded to ASCII; anything else -> '?'. */
static inline int font_index(uint32_t cp)
{
    if (cp >= 32 && cp <= 126) return (int)cp - 32;
    if (cp >= 160 && cp <= 255) return 95 + (int)cp - 160;
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'' - 32;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"' - 32;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-' - 32;
    case 0x2022: case 0x00B7: case 0x2027: return 95 + 0xB7 - 160;
    case 0x2026: return '.' - 32; /* caller expands to "..." is not worth it */
    case 0x2122: return 'T' - 32;
    default: return '?' - 32;
    }
}

#endif
