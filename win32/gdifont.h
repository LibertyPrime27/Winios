/* The stroke font: the polyline for a character, and how far to move after
 * drawing it. */
#ifndef W32_GDIFONT_H
#define W32_GDIFONT_H
#include <stdint.h>

/* Design-grid coordinates. `move` starts a new polyline rather than
 * continuing the previous one. */
typedef struct { signed char x, y, move; } gf_point;

enum {
    GF_EM     = 13,   /* design cell height: cap line to descender */
    GF_BASE   = 10,   /* design baseline */
    GF_MAXPTS = 40,   /* the longest glyph in the table, with room */
};

/* Fills `out` with up to `max` points; returns how many. */
int gf_glyph(uint32_t ch, gf_point *out, int max);

/* How far to advance after drawing `ch`, in design units. Proportional: an
 * `i` is three units and a `W` is eleven, because a dialog is laid out for a
 * proportional font and drawing one monospaced overruns every control. */
int gf_advance(uint32_t ch);

#endif
