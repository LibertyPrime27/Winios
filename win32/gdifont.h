/* The stroke font's one export: the polyline for a character. */
#ifndef W32_GDIFONT_H
#define W32_GDIFONT_H
#include <stdint.h>

/* Design-grid coordinates. `move` starts a new polyline rather than
 * continuing the previous one. */
typedef struct { signed char x, y, move; } gf_point;

enum {
    GF_EM      = 13,   /* design cell height: cap line to descender */
    GF_ADVANCE = 7,    /* design advance width */
    GF_BASE    = 10,   /* design baseline */
    GF_MAXPTS  = 32,   /* the longest glyph in the table, with room */
};

/* Fills `out` with up to `max` points; returns how many. */
int gf_glyph(uint32_t ch, gf_point *out, int max);

#endif
