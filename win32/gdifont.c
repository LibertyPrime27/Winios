/* Which face a program's LOGFONT means, and what it costs to draw.
 *
 * Three faces are built in: a proportional sans, its bold, and a monospace.
 * That is not a limitation anyone will notice in a dialog -- Windows itself
 * substitutes for almost every name a program asks for, and a program that
 * asks for "Tahoma", "MS Shell Dlg", "Segoe UI" or "Arial" wants the same
 * thing in each case: the interface font. What matters is that the substitute
 * has *proportional metrics*, because every control on a dialog is positioned
 * and sized in units derived from the average character width of one, and
 * getting the metrics wrong moves every control on the page.
 */
#include "gdifont.h"
#include <stdlib.h>
#include <string.h>

static tt_face *g_sans, *g_bold, *g_mono;
static int g_ready;

static void faces_init(void) {
    if (g_ready) return;
    g_ready = 1;
    g_sans = tt_open(w32_font_sans, w32_font_sans_len);
    g_bold = tt_open(w32_font_sans_bold, w32_font_sans_bold_len);
    g_mono = tt_open(w32_font_mono, w32_font_mono_len);
}

static int has(const char *s, const char *needle) {
    if (!s || !*s) return 0;
    size_t n = strlen(needle);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 0x20) == needle[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Does this name mean a typewriter face? The list is the set of names that
 * actually turn up: a program showing a licence agreement or a log picks one
 * of these and expects the columns to line up. */
static int wants_mono(const char *face) {
    return has(face, "courier") || has(face, "consol") || has(face, "mono")
        || has(face, "terminal") || has(face, "fixedsys") || has(face, "lucida console");
}

void gf_select(gf_font *o, const char *face, int height, int weight,
               int italic, int fixed_pitch) {
    faces_init();
    memset(o, 0, sizeof *o);

    int mono = fixed_pitch || wants_mono(face);
    int bold = weight >= 600;
    o->fixed = mono;
    if (mono) {
        o->tt = g_mono ? g_mono : g_sans;
        /* No bold monospace is shipped -- it would be a third of the font
         * data for a case that is nearly always a licence agreement -- so a
         * bold fixed-pitch request is thickened instead. */
        o->embolden = bold;
    } else if (bold) {
        o->tt = g_bold ? g_bold : g_sans;
    } else {
        o->tt = g_sans;
    }
    if (!o->tt) return;
    o->oblique = italic ? 1 : 0;

    int upem = tt_units_per_em(o->tt);
    int a, d, g;
    tt_vmetrics(o->tt, &a, &d, &g);
    int cell_units = a - d;                       /* d is negative */
    if (cell_units <= 0) cell_units = upem;

    /* LOGFONT height. Negative is the em size, which is what a program means
     * when it converts a point size; positive is the cell height, which is
     * what a dialog template's own arithmetic produces. Zero is "whatever the
     * shell uses", and 11 pixels is that at 96 DPI.
     *
     * The two are not the same number, and treating them as if they were is
     * how text ends up ten per cent too large everywhere. */
    int px;
    if (height < 0) px = -height;
    else if (height > 0) px = (int)(((long long)height * upem + cell_units / 2) / cell_units);
    else px = 11;
    if (px < 4) px = 4;
    if (px > 300) px = 300;
    o->px = px;

    o->ascent  = (int)(((long long)a * px + upem / 2) / upem);
    o->descent = (int)(((long long)(-d) * px + upem / 2) / upem);
    if (o->ascent < 1) o->ascent = 1;
    if (o->descent < 0) o->descent = 0;
    o->height = o->ascent + o->descent;
}

int gf_advance(const gf_font *f, uint32_t ch) {
    if (!f || !f->tt) return 64;
    int upem = tt_units_per_em(f->tt);
    int g = tt_glyph_of(f->tt, ch);
    /* A character the font does not have still moves the pen -- by the width
     * of the .notdef box, which is what gets drawn in its place. Returning
     * zero here is an infinite loop in any caller that walks a string by
     * width looking for where to break it. */
    long long u = tt_advance_units(f->tt, g);
    int adv = (int)((u * f->px * 64 + upem / 2) / upem);
    if (f->embolden) adv += f->px >= 24 ? 128 : 64;
    return adv < 1 ? 1 : adv;
}

const tt_glyph *gf_glyph(const gf_font *f, uint32_t ch) {
    if (!f || !f->tt) return 0;
    int g = tt_glyph_of(f->tt, ch);
    /* Glyph 0 is .notdef, drawn as an empty box, which is the honest answer
     * for a character we have no shape for -- and much easier to recognise in
     * a screenshot than a silent gap. */
    return tt_render(f->tt, g, f->px, f->oblique, f->embolden);
}

int gf_average_width(const gf_font *f) {
    if (!f || !f->tt) return 6;
    long long total = 0;
    for (int c = 'A'; c <= 'Z'; c++) total += gf_advance(f, (uint32_t)c);
    for (int c = 'a'; c <= 'z'; c++) total += gf_advance(f, (uint32_t)c);
    int avg = (int)(total / 52 / 64);
    return avg < 1 ? 1 : avg;
}
