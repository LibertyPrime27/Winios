/* A TrueType parser and rasterizer.
 *
 * Enough of the format to draw text and to measure it: the six tables that
 * carry outlines and metrics, and nothing else. No hinting -- the bytecode
 * interpreter is a large piece of work whose whole purpose is to snap stems
 * to a pixel grid, and at the sizes a dialog uses, antialiased unhinted text
 * is closer to what a modern Windows looks like than badly hinted text is.
 *
 * Everything here is integer. Coordinates are 26.6 fixed point (64ths of a
 * pixel) and coverage is accumulated as an integer count of sub-scanline
 * overlaps, so a glyph rasterizes to the same bytes on x86, on aarch64 under
 * qemu and on a phone. That is not fastidiousness: the whole test suite
 * compares presented frames by checksum, and text is most of what is in
 * them, so a font that rounded differently per platform would end the ability
 * to check anything about a dialog at all.
 */
#ifndef W32_TRUETYPE_H
#define W32_TRUETYPE_H
#include <stdint.h>
#include <stddef.h>

typedef struct tt_face tt_face;

/* Parse a font out of memory. The bytes must outlive the face -- nothing is
 * copied. NULL if this is not a TrueType file with the tables we need. */
tt_face *tt_open(const unsigned char *data, size_t len);
void     tt_close(tt_face *f);

int  tt_units_per_em(const tt_face *f);
/* Design metrics, in font units. */
void tt_vmetrics(const tt_face *f, int *ascent, int *descent, int *line_gap);
/* The glyph for a Unicode code point; 0 (.notdef) when the font has none. */
int  tt_glyph_of(const tt_face *f, uint32_t cp);
int  tt_advance_units(const tt_face *f, int glyph);

/* A rasterized glyph. `a` is w*h bytes of coverage, 0..255, row-major from
 * the top. The top-left of that box sits `left` pixels right of the pen and
 * `top` pixels above the baseline; `top` counts upward, so it is positive for
 * a capital and negative for a glyph entirely below the baseline. */
typedef struct {
    int w, h, left, top, advance;
    const unsigned char *a;
} tt_glyph;

/* Rasterize, with a cache inside the face -- a repaint draws the same few
 * dozen glyphs at the same few sizes over and over, and rasterizing an
 * outline is expensive enough that doing it per frame is visible.
 *
 * `px` is the em size in pixels, `oblique` shears for a synthesised italic,
 * `embolden` widens for a synthesised bold. Returns NULL if the glyph is
 * empty (a space) or cannot be rendered; the pointer is owned by the face and
 * stays valid until it is evicted, so use it before the next call for a
 * different glyph.
 */
const tt_glyph *tt_render(tt_face *f, int glyph, int px, int oblique, int embolden);

#endif
