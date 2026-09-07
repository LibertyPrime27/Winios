/* Reading TrueType outlines, and turning them into pixels.
 *
 * See truetype.h for what this is and is not. The short version: six tables,
 * no hinting, all integer.
 *
 * The rasterizer is a scanline fill with four sub-scanlines per pixel row and
 * exact horizontal coverage. That combination is chosen deliberately over the
 * two obvious alternatives. Sampling once per row and calling it done gives
 * the jagged text of 1995. Computing exact area per pixel -- what FreeType's
 * smooth renderer does -- is better still, but it is a much larger piece of
 * code whose correctness is hard to see by reading it, and at 11 to 16 pixels
 * the difference is not visible. Four vertical samples with exact horizontal
 * coverage gives 256 levels of grey along a near-vertical stem, which is
 * where a UI font spends most of its edges.
 */
#include "truetype.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ reading */

/* Big-endian, bounds-checked. A font is a file off somebody's disk -- a
 * game's own, eventually -- so every offset in it is hostile until proven
 * otherwise, and a read past the end returns zero rather than crashing. */
typedef struct { const unsigned char *p; size_t n; } blob;

static uint32_t rd8(blob b, size_t o)  { return o < b.n ? b.p[o] : 0; }
static uint32_t rd16(blob b, size_t o) { return o + 1 < b.n ? (uint32_t)(b.p[o] << 8 | b.p[o+1]) : 0; }
static int32_t  rs16(blob b, size_t o) { return (int32_t)(int16_t)rd16(b, o); }
static uint32_t rd32(blob b, size_t o) {
    return o + 3 < b.n ? ((uint32_t)b.p[o] << 24 | (uint32_t)b.p[o+1] << 16
                        | (uint32_t)b.p[o+2] << 8 | (uint32_t)b.p[o+3]) : 0;
}

/* ------------------------------------------------------------- the face */

enum { CACHE_N = 192 };          /* rendered glyphs kept around */

typedef struct {
    int      glyph, px, oblique, embolden;   /* the key; px 0 means empty slot */
    uint32_t stamp;
    tt_glyph g;
    unsigned char *bits;
    size_t   cap;
} cache_ent;

struct tt_face {
    blob   f;
    size_t glyf, loca, cmap, hmtx;
    size_t glyf_n, loca_n, cmap_n, hmtx_n;
    int    upem, loc_long, num_glyphs, num_hmetrics;
    int    ascent, descent, line_gap;
    int    cmap_fmt;
    size_t cmap_sub;                     /* the chosen subtable */
    cache_ent cache[CACHE_N];
    uint32_t clock;
};

static blob sub(blob b, size_t off, size_t len) {
    blob r = { 0, 0 };
    if (off <= b.n) { r.p = b.p + off; r.n = b.n - off < len ? b.n - off : len; }
    return r;
}

/* Pick a character map. Preference order is the one every shaper uses:
 * a full-Unicode format 12 if there is one, then the Basic Multilingual Plane
 * format 4 that nearly every font actually ships, then anything at all --
 * because a font with only a Macintosh table still draws Latin text, and
 * refusing it would mean refusing the font. */
static void pick_cmap(tt_face *f) {
    blob c = sub(f->f, f->cmap, f->cmap_n);
    int n = (int)rd16(c, 2);
    int best_score = -1;
    for (int i = 0; i < n; i++) {
        size_t rec = 4 + (size_t)i * 8;
        int plat = (int)rd16(c, rec), enc = (int)rd16(c, rec + 2);
        size_t off = rd32(c, rec + 4);
        if (off >= c.n) continue;
        int fmt = (int)rd16(c, off);
        int score = -1;
        if (plat == 3 && enc == 10 && fmt == 12) score = 100;
        else if (plat == 0 && fmt == 12)          score = 95;
        else if (plat == 3 && enc == 1 && fmt == 4) score = 90;
        else if (plat == 0 && fmt == 4)           score = 85;
        else if (plat == 3 && enc == 0)           score = 50;  /* symbol */
        else if (fmt == 4 || fmt == 12)           score = 40;
        else if (fmt == 6 || fmt == 0)            score = 20;
        if (score > best_score) {
            best_score = score; f->cmap_sub = f->cmap + off; f->cmap_fmt = fmt;
        }
    }
}

tt_face *tt_open(const unsigned char *data, size_t len) {
    if (!data || len < 12) return 0;
    blob b = { data, len };
    uint32_t tag = rd32(b, 0);
    /* 0x00010000 and 'true' are TrueType outlines. 'OTTO' is CFF, which is a
     * different outline format this does not read; saying so is better than
     * drawing nothing and leaving someone to wonder. */
    if (tag != 0x00010000u && tag != 0x74727565u) return 0;
    int ntab = (int)rd16(b, 4);
    if (ntab <= 0 || ntab > 512) return 0;

    tt_face *f = (tt_face *)calloc(1, sizeof *f);
    if (!f) return 0;
    f->f = b;
    size_t head = 0, hhea = 0, maxp = 0, head_n = 0, hhea_n = 0, maxp_n = 0;
    for (int i = 0; i < ntab; i++) {
        size_t rec = 12 + (size_t)i * 16;
        uint32_t t = rd32(b, rec);
        size_t off = rd32(b, rec + 8), n = rd32(b, rec + 12);
        if (off > len) continue;
        if (n > len - off) n = len - off;
        switch (t) {
        case 0x68656164u: head = off; head_n = n; break;   /* head */
        case 0x68686561u: hhea = off; hhea_n = n; break;   /* hhea */
        case 0x6D617870u: maxp = off; maxp_n = n; break;   /* maxp */
        case 0x6C6F6361u: f->loca = off; f->loca_n = n; break;
        case 0x676C7966u: f->glyf = off; f->glyf_n = n; break;
        case 0x636D6170u: f->cmap = off; f->cmap_n = n; break;
        case 0x686D7478u: f->hmtx = off; f->hmtx_n = n; break;
        default: break;
        }
    }
    if (!head || !hhea || !maxp || !f->glyf || !f->loca) { free(f); return 0; }

    blob hd = sub(b, head, head_n), hh = sub(b, hhea, hhea_n), mx = sub(b, maxp, maxp_n);
    f->upem       = (int)rd16(hd, 18);
    f->loc_long   = rs16(hd, 50) != 0;
    f->ascent     = rs16(hh, 4);
    f->descent    = rs16(hh, 6);
    f->line_gap   = rs16(hh, 8);
    f->num_hmetrics = (int)rd16(hh, 34);
    f->num_glyphs = (int)rd16(mx, 4);
    if (f->upem < 16 || f->upem > 16384 || f->num_glyphs <= 0) { free(f); return 0; }
    /* A descent is conventionally negative in hhea; some fonts state it
     * positive. Normalise, or every baseline is placed with the wrong sign. */
    if (f->descent > 0) f->descent = -f->descent;
    if (f->cmap) pick_cmap(f);
    return f;
}

void tt_close(tt_face *f) {
    if (!f) return;
    for (int i = 0; i < CACHE_N; i++) free(f->cache[i].bits);
    free(f);
}

int tt_units_per_em(const tt_face *f) { return f ? f->upem : 1000; }
void tt_vmetrics(const tt_face *f, int *a, int *d, int *g) {
    if (a) *a = f ? f->ascent : 0;
    if (d) *d = f ? f->descent : 0;
    if (g) *g = f ? f->line_gap : 0;
}

int tt_glyph_of(const tt_face *f, uint32_t cp) {
    if (!f || !f->cmap_sub) return 0;
    blob c = sub(f->f, f->cmap_sub, f->cmap_n);
    switch (f->cmap_fmt) {
    case 0:
        return cp < 256 ? (int)rd8(c, 6 + cp) : 0;
    case 4: {
        /* The segmented map. A symbol font (platform 3, encoding 0) puts its
         * glyphs at U+F000..U+F0FF, and a program asking for character 0x41
         * means 0xF041 there -- which is how Wingdings-style fonts have always
         * worked, and how a checkbox drawn with Marlett finds its glyph. */
        int segx2 = (int)rd16(c, 6);
        if (segx2 <= 0) return 0;
        size_t ends = 14, starts = ends + segx2 + 2, deltas = starts + segx2,
               ranges = deltas + segx2;
        uint32_t want = cp;
        for (int try = 0; try < 2; try++) {
            for (int i = 0; i < segx2; i += 2) {
                if (want > rd16(c, ends + i)) continue;
                uint32_t start = rd16(c, starts + i);
                if (want < start) break;
                uint32_t ro = rd16(c, ranges + i);
                uint32_t g;
                if (!ro) g = (want + rd16(c, deltas + i)) & 0xFFFF;
                else {
                    size_t at = ranges + i + ro + 2 * (want - start);
                    g = rd16(c, at);
                    if (g) g = (g + rd16(c, deltas + i)) & 0xFFFF;
                }
                if (g) return (int)g;
                break;
            }
            if (cp >= 0x20 && cp < 0x100) want = 0xF000 + cp; else break;
        }
        return 0;
    }
    case 6: {
        uint32_t first = rd16(c, 6), n = rd16(c, 8);
        return cp >= first && cp < first + n ? (int)rd16(c, 10 + 2 * (cp - first)) : 0;
    }
    case 12: {
        uint32_t n = rd32(c, 12);
        if (n > 100000) return 0;
        /* Binary search: a full-Unicode table has thousands of groups and
         * this is called once per character drawn. */
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t g = 16 + (size_t)mid * 12;
            if (cp < rd32(c, g)) hi = mid;
            else if (cp > rd32(c, g + 4)) lo = mid + 1;
            else return (int)(rd32(c, g + 8) + (cp - rd32(c, g)));
        }
        return 0;
    }
    default: return 0;
    }
}

int tt_advance_units(const tt_face *f, int glyph) {
    if (!f || !f->hmtx || f->num_hmetrics <= 0) return f ? f->upem / 2 : 0;
    blob h = sub(f->f, f->hmtx, f->hmtx_n);
    int i = glyph < f->num_hmetrics ? glyph : f->num_hmetrics - 1;
    return (int)rd16(h, (size_t)i * 4);
}

/* Where a glyph's outline lives. Equal offsets mean an empty glyph -- a
 * space -- which is not an error. */
static int glyph_range(const tt_face *f, int glyph, size_t *off, size_t *len) {
    if (glyph < 0 || glyph >= f->num_glyphs) return 0;
    blob l = sub(f->f, f->loca, f->loca_n);
    size_t a, b;
    if (f->loc_long) { a = rd32(l, (size_t)glyph * 4); b = rd32(l, (size_t)glyph * 4 + 4); }
    else             { a = rd16(l, (size_t)glyph * 2) * 2; b = rd16(l, (size_t)glyph * 2 + 2) * 2; }
    if (b <= a || a >= f->glyf_n) return 0;
    if (b > f->glyf_n) b = f->glyf_n;
    *off = f->glyf + a; *len = b - a;
    return 1;
}

/* ------------------------------------------------------- the outline */

/* Points in 26.6, with a flag saying which contour a point ends. */
typedef struct { int32_t x, y; unsigned char on, end; } opt;

typedef struct {
    opt   *p;
    int    n, cap;
    int    ok;
} outline;

static void out_push(outline *o, int32_t x, int32_t y, int on) {
    if (o->n == o->cap) {
        int cap = o->cap ? o->cap * 2 : 64;
        opt *q = (opt *)realloc(o->p, (size_t)cap * sizeof *q);
        if (!q) { o->ok = 0; return; }
        o->p = q; o->cap = cap;
    }
    o->p[o->n].x = x; o->p[o->n].y = y;
    o->p[o->n].on = (unsigned char)on; o->p[o->n].end = 0;
    o->n++;
}

/* Scale a font-unit coordinate to 26.6 pixels. The multiply is done in 64
 * bits because a 5000-unit coordinate at 200 pixels times 64 is comfortably
 * inside 32 bits but the rounding term is easier to reason about without
 * having to check. Round to nearest, half away from zero, so a glyph and its
 * mirror image round the same way. */
static int32_t scale_u(int v, int px, int upem) {
    long long n = (long long)v * px * 64;
    long long d = upem;
    return (int32_t)(n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d));
}

static void load_outline(const tt_face *f, int glyph, int px, outline *o, int depth);

static void load_simple(const tt_face *f, blob g, int px, outline *o,
                        int nc, int32_t dx, int32_t dy,
                        int32_t a, int32_t b, int32_t c, int32_t d) {
    size_t ends = 10;
    int npts = nc ? (int)rd16(g, ends + (size_t)(nc - 1) * 2) + 1 : 0;
    if (npts <= 0 || npts > 10000) { return; }
    size_t ilen = rd16(g, ends + (size_t)nc * 2);
    size_t fl = ends + (size_t)nc * 2 + 2 + ilen;

    unsigned char *flags = (unsigned char *)malloc((size_t)npts);
    int32_t *xs = (int32_t *)malloc((size_t)npts * sizeof *xs);
    int32_t *ys = (int32_t *)malloc((size_t)npts * sizeof *ys);
    if (!flags || !xs || !ys) { free(flags); free(xs); free(ys); o->ok = 0; return; }

    size_t at = fl;
    for (int i = 0; i < npts; ) {
        unsigned char fv = (unsigned char)rd8(g, at++);
        flags[i++] = fv;
        if (fv & 8) {                       /* REPEAT */
            int r = (int)rd8(g, at++);
            while (r-- > 0 && i < npts) flags[i++] = fv;
        }
    }
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        unsigned char fv = flags[i];
        if (fv & 2) { int32_t u = (int32_t)rd8(g, at++); v += (fv & 16) ? u : -u; }
        else if (!(fv & 16)) { v += rs16(g, at); at += 2; }
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        unsigned char fv = flags[i];
        if (fv & 4) { int32_t u = (int32_t)rd8(g, at++); v += (fv & 32) ? u : -u; }
        else if (!(fv & 32)) { v += rs16(g, at); at += 2; }
        ys[i] = v;
    }

    int first = 0;
    for (int ci = 0; ci < nc; ci++) {
        int last = (int)rd16(g, ends + (size_t)ci * 2);
        if (last < first || last >= npts) break;
        for (int i = first; i <= last; i++) {
            /* Apply the component transform in font units, then scale. A
             * composite's 2x2 is F2Dot14, kept here as 16.16 by the caller. */
            long long tx = ((long long)xs[i] * a + (long long)ys[i] * c) >> 16;
            long long ty = ((long long)xs[i] * b + (long long)ys[i] * d) >> 16;
            out_push(o, scale_u((int)tx, px, f->upem) + dx,
                        scale_u((int)ty, px, f->upem) + dy, flags[i] & 1);
        }
        if (o->n > 0) o->p[o->n - 1].end = 1;
        first = last + 1;
    }
    free(flags); free(xs); free(ys);
}

static void load_outline(const tt_face *f, int glyph, int px, outline *o, int depth) {
    size_t off, len;
    if (depth > 5 || !glyph_range(f, glyph, &off, &len)) return;
    blob g = sub(f->f, off, len);
    int nc = rs16(g, 0);
    if (nc >= 0) {
        load_simple(f, g, px, o, nc, 0, 0, 1 << 16, 0, 0, 1 << 16);
        return;
    }
    /* Composite: an accented letter is its base plus its mark, placed. */
    size_t at = 10;
    for (;;) {
        uint32_t flags = rd16(g, at), idx = rd16(g, at + 2);
        at += 4;
        int32_t a1, a2;
        if (flags & 1) { a1 = rs16(g, at); a2 = rs16(g, at + 2); at += 4; }
        else { a1 = (int32_t)(int8_t)rd8(g, at); a2 = (int32_t)(int8_t)rd8(g, at + 1); at += 2; }
        int32_t sa = 1 << 16, sb = 0, sc = 0, sd = 1 << 16;
        if (flags & 8) { sa = sd = (int32_t)rs16(g, at) * 4; at += 2; }
        else if (flags & 0x40) { sa = (int32_t)rs16(g, at) * 4; sd = (int32_t)rs16(g, at + 2) * 4; at += 4; }
        else if (flags & 0x80) {
            sa = (int32_t)rs16(g, at) * 4;     sb = (int32_t)rs16(g, at + 2) * 4;
            sc = (int32_t)rs16(g, at + 4) * 4; sd = (int32_t)rs16(g, at + 6) * 4; at += 8;
        }
        /* ARGS_ARE_XY_VALUES. The point-matching alternative is vanishingly
         * rare and placing the component at the origin is a better failure
         * than not drawing it. */
        int32_t dx = 0, dy = 0;
        if (flags & 2) { dx = scale_u(a1, px, f->upem); dy = scale_u(a2, px, f->upem); }

        size_t coff, clen;
        if (glyph_range(f, (int)idx, &coff, &clen)) {
            blob cg = sub(f->f, coff, clen);
            int cnc = rs16(cg, 0);
            if (cnc >= 0) load_simple(f, cg, px, o, cnc, dx, dy, sa, sb, sc, sd);
            else {
                /* A composite of a composite. Rare, and the transform does
                 * not compose here -- the nested parts are placed by their
                 * own offsets, which is right whenever the outer transform is
                 * a pure translation, and that is what these actually are. */
                int base = o->n;
                load_outline(f, (int)idx, px, o, depth + 1);
                for (int i = base; i < o->n; i++) { o->p[i].x += dx; o->p[i].y += dy; }
            }
        }
        if (!(flags & 0x20)) break;          /* MORE_COMPONENTS */
        if (at >= g.n) break;
    }
}

/* --------------------------------------------------------- flattening */

typedef struct { int32_t x0, y0, x1, y1; int dir; } edge;
typedef struct { edge *e; int n, cap; int ok; } edges;

static void edge_push(edges *E, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1) return;                    /* horizontal: crosses nothing */
    if (E->n == E->cap) {
        int cap = E->cap ? E->cap * 2 : 128;
        edge *q = (edge *)realloc(E->e, (size_t)cap * sizeof *q);
        if (!q) { E->ok = 0; return; }
        E->e = q; E->cap = cap;
    }
    edge *e = &E->e[E->n++];
    if (y0 < y1) { e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir =  1; }
    else         { e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1; }
}

/* A quadratic, in as many straight pieces as its size deserves. The count
 * comes from the control polygon's length in whole pixels, so a glyph at 11
 * pixels is cheap and the same glyph at 90 is smooth, and the arithmetic is
 * integer so both are the same everywhere. */
static void flatten_quad(edges *E, int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                         int32_t x1, int32_t y1) {
    int32_t d = (cx > x0 ? cx - x0 : x0 - cx) + (cy > y0 ? cy - y0 : y0 - cy)
              + (x1 > cx ? x1 - cx : cx - x1) + (y1 > cy ? y1 - cy : cy - y1);
    int n = 2 + (int)(d >> 7);               /* one step per two pixels */
    if (n > 24) n = 24;
    int32_t px_ = x0, py_ = y0;
    for (int i = 1; i <= n; i++) {
        /* B(t) with t = i/n, kept in integers: the numerators are at most
         * n^2 (576) times a 26.6 coordinate, which stays inside 32 bits for
         * any glyph at any size this draws. */
        long long t = i, u = n - i, nn = (long long)n * n;
        long long qx = (u * u * x0 + 2 * u * t * cx + t * t * x1) / nn;
        long long qy = (u * u * y0 + 2 * u * t * cy + t * t * y1) / nn;
        edge_push(E, px_, py_, (int32_t)qx, (int32_t)qy);
        px_ = (int32_t)qx; py_ = (int32_t)qy;
    }
}

/* Walk one contour, turning TrueType's quadratic B-spline into edges.
 *
 * The wrinkle that catches everyone: two consecutive off-curve points imply
 * an on-curve point at their midpoint, and a contour may start off-curve, in
 * which case the start is an implied midpoint too. Getting this wrong does
 * not crash -- it draws a glyph with a bite out of it. */
static void contour_edges(edges *E, const opt *p, int n) {
    if (n < 2) return;
    int32_t sx, sy;
    int i0;
    if (p[0].on) { sx = p[0].x; sy = p[0].y; i0 = 1; }
    else if (p[n - 1].on) { sx = p[n - 1].x; sy = p[n - 1].y; i0 = 0; }
    else { sx = (p[0].x + p[n - 1].x) / 2; sy = (p[0].y + p[n - 1].y) / 2; i0 = 0; }

    int32_t cx = sx, cy = sy;                /* current on-curve point */
    int have_ctrl = 0;
    int32_t qx = 0, qy = 0;
    for (int k = 0; k < n; k++) {
        const opt *q = &p[(i0 + k) % n];
        if (q->on) {
            if (have_ctrl) { flatten_quad(E, cx, cy, qx, qy, q->x, q->y); have_ctrl = 0; }
            else edge_push(E, cx, cy, q->x, q->y);
            cx = q->x; cy = q->y;
        } else {
            if (have_ctrl) {
                int32_t mx = (qx + q->x) / 2, my = (qy + q->y) / 2;
                flatten_quad(E, cx, cy, qx, qy, mx, my);
                cx = mx; cy = my;
            }
            qx = q->x; qy = q->y; have_ctrl = 1;
        }
    }
    if (have_ctrl) flatten_quad(E, cx, cy, qx, qy, sx, sy);
    else edge_push(E, cx, cy, sx, sy);
}

/* --------------------------------------------------------- the fill */

enum { SUBS = 4 };               /* sub-scanlines per pixel row */

typedef struct { int32_t x; int dir; } cross;

static void sort_cross(cross *c, int n) {
    for (int i = 1; i < n; i++) {            /* insertion: n is tiny */
        cross v = c[i];
        int j = i - 1;
        while (j >= 0 && c[j].x > v.x) { c[j + 1] = c[j]; j--; }
        c[j + 1] = v;
    }
}

static int32_t div_floor(int32_t a, int32_t b) {
    return a >= 0 ? a / b : -(((-a) + b - 1) / b);
}
static int32_t div_ceil(int32_t a, int32_t b) {
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

/* ------------------------------------------------------------ rendering */

/* Stem darkening.
 *
 * Coverage straight off the rasterizer is linearly correct and looks wrong:
 * an 11-pixel stem covers a little over one pixel, so its grey lands near the
 * middle and the text reads washed-out and spidery next to Windows, whose
 * hinting snaps that stem to a whole black pixel instead. Hinting is not
 * happening here, so the weight is put back the way every unhinted renderer
 * puts it back -- by bending the coverage curve so partial coverage counts
 * for more than its area.
 *
 * The curve is a + a(255-a)/512, which follows a gamma of about 1.45 to
 * within a few levels, is monotone, fixes both endpoints, and is one multiply
 * of integers -- so it darkens by the same amount on every machine.
 */
static unsigned char darken(int a) {
    int v = a + (a * (255 - a)) / 512;
    return (unsigned char)(v > 255 ? 255 : v);
}

static void widen(unsigned char *a, int w, int h, int by) {
    /* Synthesised bold: smear right. Done on coverage rather than on the
     * outline because thickening an outline properly means offsetting curves,
     * and this is only ever a fallback for a face with no real bold. */
    for (int y = 0; y < h; y++) {
        unsigned char *r = a + (size_t)y * w;
        for (int x = w - 1; x >= 0; x--) {
            int v = r[x];
            for (int k = 1; k <= by && x - k >= 0; k++) if (r[x - k] > v) v = r[x - k];
            r[x] = (unsigned char)v;
        }
    }
}

const tt_glyph *tt_render(tt_face *f, int glyph, int px, int oblique, int embolden) {
    if (!f || px < 1) return 0;
    if (px > 400) px = 400;

    /* Cache first. A repaint redraws every label on the dialog, and the same
     * two dozen glyphs at one size come back every frame. */
    for (int i = 0; i < CACHE_N; i++) {
        cache_ent *c = &f->cache[i];
        if (c->px == px && c->glyph == glyph && c->oblique == oblique
            && c->embolden == embolden && c->bits) {
            c->stamp = ++f->clock;
            return &c->g;
        }
    }

    outline o = { 0, 0, 0, 1 };
    load_outline(f, glyph, px, &o, 0);
    if (!o.ok || o.n < 2) { free(o.p); return 0; }

    if (oblique) {
        /* tan(11.5 degrees) is about 0.2036; 13/64 is 0.2031, and being
         * integer it shears identically everywhere. */
        for (int i = 0; i < o.n; i++) o.p[i].x += (o.p[i].y * 13) >> 6;
    }

    int32_t xmin = o.p[0].x, xmax = o.p[0].x, ymin = o.p[0].y, ymax = o.p[0].y;
    for (int i = 1; i < o.n; i++) {
        if (o.p[i].x < xmin) xmin = o.p[i].x;
        if (o.p[i].x > xmax) xmax = o.p[i].x;
        if (o.p[i].y < ymin) ymin = o.p[i].y;
        if (o.p[i].y > ymax) ymax = o.p[i].y;
    }
    int gx0 = div_floor(xmin, 64), gx1 = div_ceil(xmax, 64);
    int gy0 = div_floor(ymin, 64), gy1 = div_ceil(ymax, 64);
    int w = gx1 - gx0, h = gy1 - gy0;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) { free(o.p); return 0; }
    int bold_by = embolden ? (px >= 24 ? 2 : 1) : 0;
    w += bold_by;

    edges E = { 0, 0, 0, 1 };
    int start = 0;
    for (int i = 0; i < o.n; i++) {
        if (o.p[i].end || i == o.n - 1) {
            contour_edges(&E, o.p + start, i - start + 1);
            start = i + 1;
        }
    }
    free(o.p);
    if (!E.ok || E.n == 0) { free(E.e); return 0; }

    unsigned char *bits = (unsigned char *)calloc((size_t)w * h, 1);
    cross *cr = (cross *)malloc((size_t)E.n * sizeof *cr);
    int *cov = (int *)malloc((size_t)w * sizeof *cov);
    if (!bits || !cr || !cov) { free(bits); free(cr); free(cov); free(E.e); return 0; }

    for (int row = 0; row < h; row++) {
        memset(cov, 0, (size_t)w * sizeof *cov);
        int ytop = (gy1 - row) * 64;          /* this row spans [ytop-64, ytop) */
        for (int s = 0; s < SUBS; s++) {
            /* Sample in the middle of each sub-row, so a stem exactly on a
             * pixel boundary is split evenly rather than landing on one side. */
            int32_t sy = ytop - 64 + (2 * s + 1) * 64 / (2 * SUBS);
            int nc = 0;
            for (int i = 0; i < E.n; i++) {
                edge *e = &E.e[i];
                if (sy < e->y0 || sy >= e->y1) continue;
                long long num = (long long)(sy - e->y0) * (e->x1 - e->x0);
                cr[nc].x = e->x0 + (int32_t)(num / (e->y1 - e->y0));
                cr[nc].dir = e->dir;
                nc++;
            }
            if (nc < 2) continue;
            sort_cross(cr, nc);
            int wind = 0;
            for (int i = 0; i < nc - 1; i++) {
                wind += cr[i].dir;
                if (!wind) continue;                  /* nonzero winding rule */
                int32_t xa = cr[i].x, xb = cr[i + 1].x;
                if (xb <= xa) continue;
                int pa = div_floor(xa, 64) - gx0, pb = div_floor(xb - 1, 64) - gx0;
                if (pb < 0 || pa >= w) continue;
                if (pa < 0) pa = 0;
                if (pb >= w) pb = w - 1;
                for (int p = pa; p <= pb; p++) {
                    int32_t l = (int32_t)(p + gx0) * 64, r = l + 64;
                    int32_t a = xa > l ? xa : l, b = xb < r ? xb : r;
                    if (b > a) cov[p] += b - a;       /* 0..64 per sub-scanline */
                }
            }
        }
        unsigned char *dst = bits + (size_t)row * w;
        for (int p = 0; p < w; p++) {
            int v = cov[p];                            /* 0..SUBS*64 == 0..256 */
            dst[p] = darken(v >= 255 ? 255 : v < 0 ? 0 : v);
        }
    }
    free(cr); free(cov); free(E.e);
    if (bold_by) widen(bits, w, h, bold_by);

    /* Into the cache, over whatever was used least recently. */
    int victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < CACHE_N; i++) {
        if (!f->cache[i].bits) { victim = i; break; }
        if (f->cache[i].stamp < oldest) { oldest = f->cache[i].stamp; victim = i; }
    }
    cache_ent *c = &f->cache[victim];
    free(c->bits);
    c->bits = bits;
    c->glyph = glyph; c->px = px; c->oblique = oblique; c->embolden = embolden;
    c->stamp = ++f->clock;
    c->g.w = w; c->g.h = h; c->g.left = gx0; c->g.top = gy1; c->g.a = bits;
    c->g.advance = 0;                       /* the caller knows its own metrics */
    return &c->g;
}
