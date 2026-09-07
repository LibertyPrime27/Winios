/* gdi32.dll -- pixels.
 *
 * Everything a Windows program draws that is not a 3D API goes through here:
 * an installer's whole user interface is FillRect, a few lines, and a lot of
 * TextOut. Until this file existed the win32 layer could create a window and
 * pump its messages but had nothing to paint it with, so a setup program ran
 * blind -- which is exactly what "the installer runs but I cannot see it"
 * means.
 *
 * The model is the simple one, and it is the one Windows itself used before
 * the compositor: there is a single screen surface, a window's client area is
 * a rectangle in it, and a device context is that surface plus an origin and
 * a clip rectangle. Drawing a child control is drawing into the screen at the
 * control's position. No per-window backing store, no z-order compositing --
 * a repaint is a repaint, in painter's order, which is what the message flow
 * already gives us.
 *
 * A memory DC is the same structure over its own allocation, so
 * CreateCompatibleDC + BitBlt (how a well-behaved program avoids flicker)
 * works without a special case.
 *
 * Colours: a COLORREF is 0x00BBGGRR, and the surface is what the Metal view
 * uploads -- 0xAARRGGBB in a word, B,G,R,A in memory. `px()` is the one place
 * that conversion happens.
 */
#define _GNU_SOURCE
#include "w32.h"
#include "gdifont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- how big a "point" is ------------------------------------------------
 *
 * A Windows dialog is laid out in dialog units, which come from the size of
 * the system font, which comes from the display's DPI. Every desktop
 * installer was drawn for 96 DPI, where a wizard is about 500 by 350
 * pixels -- a comfortable window on a 1280x1024 monitor.
 *
 * That same dialog on a tablet's panel is a postage stamp. Rendering the
 * guest at 2732 pixels wide and then drawing a 500-pixel dialog in the
 * middle of it is technically faithful and practically unusable; rendering
 * at 640 and scaling the frame up with nearest-neighbour sampling gives a
 * dialog that fills the screen out of blocks. Neither is what the person
 * wanted, and it is why "it renders" and "you can read it" turned out to be
 * different claims.
 *
 * Windows solves this with DPI scaling, and so does this: report a higher
 * DPI, and every dialog that asks -- which is all of them, through their own
 * font size and their own dialog units -- lays itself out proportionally
 * bigger. The text is then drawn at a size where the strokes have room, so
 * it is sharper as well as larger. Nothing about the program changes; it is
 * told the truth about a display that is genuinely denser.
 */
static int g_dpi = 96;
static int g_stock_ready;
void w32_set_ui_dpi(int dpi) {
    if (dpi < 96) dpi = 96;
    if (dpi > 480) dpi = 480;
    if (dpi == g_dpi) return;
    g_dpi = dpi;
    /* The stock font's size came from the old DPI, so it has to be made
     * again -- otherwise every dialog scales its layout and keeps its old
     * text size, which is worse than not scaling at all. */
    g_stock_ready = 0;
}
int w32_ui_dpi(void) { return g_dpi; }
/* A point size to a pixel height, at whatever DPI we are claiming. */
int w32_points_to_pixels(int points) {
    int h = points * g_dpi / 72;
    return h < 8 ? 8 : h;
}

static uint32_t px(uint32_t ref) {
    return 0xFF000000u | ((ref & 0xFFu) << 16) | (ref & 0xFF00u) | ((ref >> 16) & 0xFFu);
}
static uint32_t ref_of(uint32_t p) {
    return ((p >> 16) & 0xFFu) | (p & 0xFF00u) | ((p & 0xFFu) << 16);
}

/* ------------------------------------------------------------- objects */

typedef enum { G_FREE = 0, G_PEN, G_BRUSH, G_FONT, G_BITMAP, G_REGION } gtype;

enum { PS_SOLID = 0, PS_NULL = 5 };
enum { BS_SOLID = 0, BS_NULL = 1, BS_HATCHED = 2, BS_PATTERN = 3 };

typedef struct {
    gtype    type;
    int      stock;                /* a stock object is never deleted */
    uint32_t color;                /* COLORREF: pen or brush colour */
    int      style, width;         /* pen style/width, or brush style */
    /* font */
    int      height, weight, italic, underline, strikeout;
    char     face[32];
    /* bitmap */
    int      bw, bh;
    uint32_t *bits;
} gobj;

enum { MAX_OBJ = 256, OBJ_BASE = 0x00080000u, OBJ_STEP = 8 };
static gobj g_obj[MAX_OBJ];

static gobj *obj_of(uint64_t h) {
    if (h < OBJ_BASE) return 0;
    uint64_t i = (h - OBJ_BASE) / OBJ_STEP;
    if (i >= MAX_OBJ || (h - OBJ_BASE) % OBJ_STEP || g_obj[i].type == G_FREE) return 0;
    return &g_obj[i];
}
static uint64_t handle_of(const gobj *o) { return OBJ_BASE + (uint64_t)(o - g_obj) * OBJ_STEP; }

static gobj *obj_new(gtype t) {
    for (int i = 0; i < MAX_OBJ; i++) if (g_obj[i].type == G_FREE) {
        memset(&g_obj[i], 0, sizeof g_obj[i]);
        g_obj[i].type = t;
        return &g_obj[i];
    }
    return 0;
}

/* Stock objects, made once and never freed. The indices are the documented
 * GetStockObject constants, so a program that asks for WHITE_BRUSH by number
 * gets a white brush. */
enum {
    WHITE_BRUSH = 0, LTGRAY_BRUSH = 1, GRAY_BRUSH = 2, DKGRAY_BRUSH = 3,
    BLACK_BRUSH = 4, NULL_BRUSH = 5, WHITE_PEN = 6, BLACK_PEN = 7, NULL_PEN = 8,
    OEM_FIXED_FONT = 10, ANSI_FIXED_FONT = 11, ANSI_VAR_FONT = 12,
    SYSTEM_FONT = 13, DEVICE_DEFAULT_FONT = 14, SYSTEM_FIXED_FONT = 16,
    DEFAULT_GUI_FONT = 17, DC_BRUSH = 18, DC_PEN = 19, NSTOCK = 20,
};
static uint64_t g_stock[NSTOCK];

static gobj *mk_brush(uint32_t color, int style) {
    gobj *o = obj_new(G_BRUSH);
    if (!o) return 0;
    o->color = color; o->style = style;
    return o;
}
static gobj *mk_pen(uint32_t color, int style, int width) {
    gobj *o = obj_new(G_PEN);
    if (!o) return 0;
    o->color = color; o->style = style; o->width = width < 1 ? 1 : width;
    return o;
}
static gobj *mk_font(int height, int weight, const char *face) {
    gobj *o = obj_new(G_FONT);
    if (!o) return 0;
    o->height = height; o->weight = weight;
    snprintf(o->face, sizeof o->face, "%s", face ? face : "");
    return o;
}

static void stock_init(void) {
    if (g_stock_ready) return;
    g_stock_ready = 1;
    struct { int i; uint32_t c; } B[] = {
        { WHITE_BRUSH, 0xFFFFFF }, { LTGRAY_BRUSH, 0xC0C0C0 }, { GRAY_BRUSH, 0x808080 },
        { DKGRAY_BRUSH, 0x404040 }, { BLACK_BRUSH, 0x000000 }, { DC_BRUSH, 0xFFFFFF },
    };
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        gobj *o = mk_brush(B[i].c, BS_SOLID); o->stock = 1; g_stock[B[i].i] = handle_of(o);
    }
    gobj *o = mk_brush(0, BS_NULL); o->stock = 1; g_stock[NULL_BRUSH] = handle_of(o);
    o = mk_pen(0xFFFFFF, PS_SOLID, 1); o->stock = 1; g_stock[WHITE_PEN] = handle_of(o);
    o = mk_pen(0x000000, PS_SOLID, 1); o->stock = 1; g_stock[BLACK_PEN] = handle_of(o);
    o = mk_pen(0x000000, PS_SOLID, 1); o->stock = 1; g_stock[DC_PEN] = handle_of(o);
    o = mk_pen(0, PS_NULL, 1);         o->stock = 1; g_stock[NULL_PEN] = handle_of(o);
    /* The UI font: 8 point, which is what a dialog template asks for, at
     * whatever DPI we are claiming. A dialog that sets its own font size
     * gets that instead, converted the same way. */
    o = mk_font(-w32_points_to_pixels(8), 400, "Winios Sans"); o->stock = 1;
    g_stock[DEFAULT_GUI_FONT] = handle_of(o);
    g_stock[SYSTEM_FONT] = g_stock[DEFAULT_GUI_FONT];
    g_stock[DEVICE_DEFAULT_FONT] = g_stock[DEFAULT_GUI_FONT];
    g_stock[ANSI_VAR_FONT] = g_stock[DEFAULT_GUI_FONT];
    o = mk_font(-w32_points_to_pixels(8), 400, "Winios Mono"); o->stock = 1;
    g_stock[ANSI_FIXED_FONT] = g_stock[OEM_FIXED_FONT] = g_stock[SYSTEM_FIXED_FONT] = handle_of(o);
}
/* A brush and a font that user32 can make without going through the export
 * table: a system-colour brush, and the font a dialog template asks for. */
uint64_t w32_make_solid_brush(uint32_t colorref) {
    stock_init();
    gobj *o = mk_brush(colorref, BS_SOLID);
    return o ? handle_of(o) : 0;
}
uint64_t w32_make_font(int height, const char *face) {
    stock_init();
    gobj *o = mk_font(height, 400, face);
    return o ? handle_of(o) : 0;
}
uint64_t w32_stock_object(int i) {
    stock_init();
    return (i >= 0 && i < NSTOCK) ? g_stock[i] : 0;
}

/* --------------------------------------------------------------- the DCs */

enum { TA_LEFT = 0, TA_RIGHT = 2, TA_CENTER = 6, TA_TOP = 0, TA_BOTTOM = 8, TA_BASELINE = 24 };

typedef struct {
    int      used;
    uint64_t hwnd;                 /* the window this DC paints, 0 for a memory DC */
    uint32_t *bits;                /* target; borrowed for a screen DC */
    int      sw, sh;               /* target size in pixels */
    int      ox, oy;               /* where (0,0) in DC space lands in the target */
    int      cl, ct, cr, cb;       /* clip, in target coordinates */
    uint64_t pen, brush, font, bitmap;
    uint32_t textcolor, bkcolor;
    int      bkmode;               /* 1 TRANSPARENT, 2 OPAQUE */
    int      align;
    int      cx, cy;               /* current position, DC space */
    int      own_bits;             /* a memory DC allocated its own surface */
    int      saved;                /* SaveDC depth, for a matching RestoreDC */
} gdc;

enum { MAX_DC = 32, DC_BASE = 0x00090000u, DC_STEP = 8, TRANSPARENT_ = 1, OPAQUE_ = 2 };
static gdc g_dc[MAX_DC];

static gdc *dc_of(uint64_t h) {
    if (h < DC_BASE) return 0;
    uint64_t i = (h - DC_BASE) / DC_STEP;
    if (i >= MAX_DC || (h - DC_BASE) % DC_STEP || !g_dc[i].used) return 0;
    return &g_dc[i];
}
static uint64_t dch_of(const gdc *d) { return DC_BASE + (uint64_t)(d - g_dc) * DC_STEP; }

static void dc_defaults(gdc *d) {
    stock_init();
    d->pen = g_stock[BLACK_PEN];
    d->brush = g_stock[WHITE_BRUSH];
    d->font = g_stock[DEFAULT_GUI_FONT];
    d->textcolor = 0x000000;
    d->bkcolor = 0xFFFFFF;
    d->bkmode = OPAQUE_;
    d->align = TA_LEFT | TA_TOP;
}

/* A DC over the screen, clipped to a window's client area and with its origin
 * there -- what GetDC and BeginPaint both hand back. */
uint64_t w32_dc_for_window(w32 *w, uint64_t hwnd, int whole_window) {
    (void)w;
    int sw = 0, sh = 0;
    uint32_t *bits = w32_desktop_bits(&sw, &sh);
    if (!bits) return 0;
    int x = 0, y = 0, cw = sw, ch = sh;
    if (hwnd && !w32_window_area(hwnd, whole_window, &x, &y, &cw, &ch)) return 0;
    /* A control cannot draw outside its parent. A template that positions
     * something slightly over the edge -- and they do -- would otherwise
     * paint over whatever is beside the dialog, which on one shared surface
     * means over another window. */
    int cl = x, ct = y, cr = x + cw, cb = y + ch;
    if (hwnd) {
        int px = 0, py = 0, pw = 0, ph = 0;
        if (w32_window_parent_area(hwnd, &px, &py, &pw, &ph)) {
            if (px > cl) cl = px;
            if (py > ct) ct = py;
            if (px + pw < cr) cr = px + pw;
            if (py + ph < cb) cb = py + ph;
        }
    }
    for (int i = 0; i < MAX_DC; i++) if (!g_dc[i].used) {
        gdc *d = &g_dc[i];
        memset(d, 0, sizeof *d);
        d->used = 1; d->hwnd = hwnd;
        d->bits = bits; d->sw = sw; d->sh = sh;
        d->ox = x; d->oy = y;
        d->cl = cl < 0 ? 0 : cl; d->ct = ct < 0 ? 0 : ct;
        d->cr = cr > sw ? sw : cr;
        d->cb = cb > sh ? sh : cb;
        dc_defaults(d);
        return dch_of(d);
    }
    return 0;
}
void w32_dc_release(uint64_t hdc) {
    gdc *d = dc_of(hdc);
    if (!d) return;
    if (d->own_bits) free(d->bits);
    d->used = 0;
}

/* ------------------------------------------------------------ primitives */

static void plot(gdc *d, int x, int y, uint32_t p) {
    int tx = x + d->ox, ty = y + d->oy;
    if (tx < d->cl || tx >= d->cr || ty < d->ct || ty >= d->cb) return;
    d->bits[(size_t)ty * d->sw + tx] = p;
}

/* Rectangles are half-open, as GDI's are: Rectangle(l,t,r,b) fills up to but
 * not including r and b, which is why a 100-wide control is l..l+100. */
static void fill(gdc *d, int l, int t, int r, int b, uint32_t p) {
    l += d->ox; r += d->ox; t += d->oy; b += d->oy;
    if (l < d->cl) l = d->cl;
    if (t < d->ct) t = d->ct;
    if (r > d->cr) r = d->cr;
    if (b > d->cb) b = d->cb;
    for (int y = t; y < b; y++) {
        uint32_t *row = d->bits + (size_t)y * d->sw;
        for (int x = l; x < r; x++) row[x] = p;
    }
}

static void line(gdc *d, int x0, int y0, int x1, int y1, uint32_t p, int thick) {
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = ax - ay;
    for (;;) {
        if (thick <= 1) plot(d, x0, y0, p);
        else for (int j = 0; j < thick; j++) for (int i = 0; i < thick; i++) plot(d, x0 + i, y0 + j, p);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ay) { err -= ay; x0 += sx; }
        if (e2 <  ax) { err += ax; y0 += sy; }
    }
}

/* ---------------------------------------------------------------- text */

/* The scale a font of `height` draws at. GDI's LOGFONT height is the cell
 * height when positive and the character height when negative; both land in
 * the same place here because the design grid has one em from cap line to
 * descender. A zero height means "the default", not "invisible". */
static void font_metrics(const gobj *f, int *cell, int *ascent, int *bold) {
    int h = f ? f->height : -11;
    if (h < 0) h = -h * 13 / 11;                   /* char height -> cell height */
    if (h == 0) h = 13;
    if (h < 6) h = 6;
    if (h > 200) h = 200;
    *cell = h;
    *ascent = h * GF_BASE / GF_EM;
    *bold = f && f->weight >= 600;
}

/* One glyph's advance in pixels at this cell height. Rounded to the nearest
 * pixel rather than down, or every narrow letter collapses to the same width
 * at small sizes and the font stops being proportional where it matters
 * most. Never zero: a glyph that does not move is an infinite loop in any
 * caller that walks a string by width. */
static int glyph_advance(int cell, uint32_t ch) {
    int a = (gf_advance(ch) * cell + GF_EM / 2) / GF_EM;
    return a < 1 ? 1 : a;
}

/* A whole run. This has to be a function of the *string*, not of its length
 * -- which is the whole reason the measuring API takes one. A proportional
 * font whose extent is computed from a character count is a monospaced font
 * with extra steps, and every control sized by GetTextExtentPoint32 comes
 * out the wrong width. */
static int run_width(int cell, const char *s, int len) {
    int w = 0;
    int tab = glyph_advance(cell, ' ') * 4;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') { w = (w / tab + 1) * tab; continue; }
        w += glyph_advance(cell, c);
    }
    return w;
}

/* One character, its origin at the cell's top-left in DC space. */
static void draw_glyph(gdc *d, int x, int y, uint32_t ch, int cell, int bold, uint32_t p) {
    gf_point pts[GF_MAXPTS];
    int n = gf_glyph(ch, pts, GF_MAXPTS);
    int px0 = 0, py0 = 0;
    /* A one-pixel stroke is right at 11 pixels and spidery at 30. Real text
     * gets heavier as it gets bigger; a plotter font that does not looks
     * like a wireframe of text rather than text. */
    int weight = cell >= 34 ? 3 : cell >= 19 ? 2 : 1;
    if (bold) weight++;
    for (int i = 0; i < n; i++) {
        /* +GF_EM/2 rounds to the nearest pixel instead of always down, which
         * is the difference between a legible small size and a mush. */
        int gx = x + (pts[i].x * cell + GF_EM / 2) / GF_EM;
        int gy = y + (pts[i].y * cell + GF_EM / 2) / GF_EM;
        if (!pts[i].move) {
            /* Thickness by drawing the stroke again, offset -- a real pen
             * width would need the outline of the stroke, and for a UI font
             * at these sizes the difference is not visible. */
            for (int k = 0; k < weight; k++) line(d, px0 + k, py0, gx + k, gy, p, 1);
            if (weight > 1) for (int k = 0; k < weight - 1; k++)
                line(d, px0, py0 + k, gx, gy + k, p, 1);
        }
        px0 = gx; py0 = gy;
    }
}

/* Text at (x, y), where y is the top of the cell. Returns the width drawn. */
static int draw_text_run(gdc *d, int x, int y, const char *s, int len) {
    gobj *f = obj_of(d->font);
    int cell, ascent, bold;
    font_metrics(f, &cell, &ascent, &bold);
    uint32_t fg = px(d->textcolor), bg = px(d->bkcolor);
    if (d->bkmode == OPAQUE_) fill(d, x, y, x + run_width(cell, s, len), y + cell, bg);
    int tab = glyph_advance(cell, ' ') * 4;
    int cx = x;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') { cx = x + ((cx - x) / tab + 1) * tab; continue; }
        draw_glyph(d, cx, y, c, cell, bold, fg);
        cx += glyph_advance(cell, c);
    }
    if (f && f->underline) fill(d, x, y + ascent + 2, cx, y + ascent + 3, fg);
    if (f && f->strikeout)  fill(d, x, y + ascent * 2 / 3, cx, y + ascent * 2 / 3 + 1, fg);
    return cx - x;
}

void w32_gdi_text_extent(uint64_t hdc, const char *s, int len, int *cx, int *cy) {
    gdc *d = dc_of(hdc);
    if (!d) { if (cx) *cx = 0; if (cy) *cy = 0; return; }
    gobj *f = obj_of(d->font);
    int cell, ascent, bold;
    font_metrics(f, &cell, &ascent, &bold);
    if (cx) *cx = s ? run_width(cell, s, len) : 0;
    if (cy) *cy = cell;
}
/* The average character width, which is what a dialog's unit conversion is
 * defined in terms of -- not the width of any particular letter. Windows
 * takes the mean over 'A'..'Z' and 'a'..'z', and using anything else puts
 * every control in a template at the wrong place. */
int w32_gdi_average_width(uint64_t hdc) {
    gdc *d = dc_of(hdc);
    if (!d) return 6;
    int cell, ascent, bold;
    font_metrics(obj_of(d->font), &cell, &ascent, &bold);
    int total = 0;
    for (int c = 'A'; c <= 'Z'; c++) total += glyph_advance(cell, (uint32_t)c);
    for (int c = 'a'; c <= 'z'; c++) total += glyph_advance(cell, (uint32_t)c);
    int avg = total / 52;
    return avg < 1 ? 1 : avg;
}
int w32_gdi_line_height(uint64_t hdc) {
    gdc *d = dc_of(hdc);
    if (!d) return 13;
    int cell, ascent, bold;
    font_metrics(obj_of(d->font), &cell, &ascent, &bold);
    return cell;
}

/* --------------------------------------------------- the host-side API */

/* What user32 draws with. FillRect, DrawText and the frame primitives live in
 * user32.dll but the pixels are here, so these are the seam. */
void w32_gdi_fill_rect(uint64_t hdc, int l, int t, int r, int b, uint32_t color) {
    gdc *d = dc_of(hdc);
    if (d) fill(d, l, t, r, b, px(color));
}
void w32_gdi_frame_rect(uint64_t hdc, int l, int t, int r, int b, uint32_t color) {
    gdc *d = dc_of(hdc);
    if (!d) return;
    uint32_t p = px(color);
    fill(d, l, t, r, t + 1, p);
    fill(d, l, b - 1, r, b, p);
    fill(d, l, t, l + 1, b, p);
    fill(d, r - 1, t, r, b, p);
}
void w32_gdi_line(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t color) {
    gdc *d = dc_of(hdc);
    if (d) line(d, x0, y0, x1, y1, px(color), 1);
}
int w32_gdi_text_at(uint64_t hdc, int x, int y, const char *s, int len) {
    gdc *d = dc_of(hdc);
    return d ? draw_text_run(d, x, y, s, len) : 0;
}
uint32_t w32_gdi_brush_color(uint64_t hbrush, int *is_null) {
    gobj *o = obj_of(hbrush);
    if (is_null) *is_null = !o || o->style == BS_NULL;
    return o ? o->color : 0xFFFFFF;
}
void w32_gdi_set_text_color(uint64_t hdc, uint32_t c) { gdc *d = dc_of(hdc); if (d) d->textcolor = c; }
void w32_gdi_set_bk_color(uint64_t hdc, uint32_t c)   { gdc *d = dc_of(hdc); if (d) d->bkcolor = c; }
void w32_gdi_set_bk_mode(uint64_t hdc, int m)         { gdc *d = dc_of(hdc); if (d) d->bkmode = m; }
uint64_t w32_gdi_set_font(uint64_t hdc, uint64_t f) {
    gdc *d = dc_of(hdc);
    if (!d) return 0;
    uint64_t old = d->font;
    if (obj_of(f)) d->font = f;
    return old;
}
/* Narrow a DC to a sub-rectangle of itself, in DC coordinates: how a control
 * is given a DC that cannot draw outside its own bounds. */
void w32_gdi_clip_to(uint64_t hdc, int l, int t, int r, int b) {
    gdc *d = dc_of(hdc);
    if (!d) return;
    if (l + d->ox > d->cl) d->cl = l + d->ox;
    if (t + d->oy > d->ct) d->ct = t + d->oy;
    if (r + d->ox < d->cr) d->cr = r + d->ox;
    if (b + d->oy < d->cb) d->cb = b + d->oy;
}
void w32_gdi_get_clip(uint64_t hdc, int *l, int *t, int *r, int *b, int *ox, int *oy) {
    gdc *d = dc_of(hdc);
    if (!d) return;
    *l = d->cl; *t = d->ct; *r = d->cr; *b = d->cb; *ox = d->ox; *oy = d->oy;
}
void w32_gdi_set_clip(uint64_t hdc, int l, int t, int r, int b, int ox, int oy) {
    gdc *d = dc_of(hdc);
    if (!d) return;
    d->cl = l; d->ct = t; d->cr = r; d->cb = b; d->ox = ox; d->oy = oy;
}

/* ------------------------------------------------------------ exports */

static void g_GetStockObject(w32 *w) { RET(w32_stock_object((int)(int32_t)(uint32_t)ARG(0))); }

static void g_CreateSolidBrush(w32 *w) {
    stock_init();
    gobj *o = mk_brush((uint32_t)ARG(0), BS_SOLID);
    RET(o ? handle_of(o) : 0);
}
/* LOGBRUSH is { UINT style; COLORREF color; ULONG_PTR hatch } -- the colour is
 * always at offset 4, both bitnesses, because the first field is a UINT. */
static void g_CreateBrushIndirect(w32 *w) {
    stock_init();
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    uint32_t style = (uint32_t)w32_read(w, p, 4);
    uint32_t color = (uint32_t)w32_read(w, p + 4, 4);
    gobj *o = mk_brush(color, style == 1 ? BS_NULL : BS_SOLID);
    RET(o ? handle_of(o) : 0);
}
static void g_CreatePatternBrush(w32 *w) { stock_init(); gobj *o = mk_brush(0xC0C0C0, BS_SOLID); RET(o ? handle_of(o) : 0); }
static void g_CreateHatchBrush(w32 *w)   { stock_init(); gobj *o = mk_brush((uint32_t)ARG(1), BS_SOLID); RET(o ? handle_of(o) : 0); }

static void g_CreatePen(w32 *w) {
    stock_init();
    gobj *o = mk_pen((uint32_t)ARG(2), (int)ARG(0), (int)ARG(1));
    RET(o ? handle_of(o) : 0);
}
/* LOGPEN is { UINT style; POINT width; COLORREF color } -- 4 + 8 + 4. */
static void g_CreatePenIndirect(w32 *w) {
    stock_init();
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    gobj *o = mk_pen((uint32_t)w32_read(w, p + 12, 4), (int)w32_read(w, p, 4), (int)w32_read(w, p + 4, 4));
    RET(o ? handle_of(o) : 0);
}

/* LOGFONT: height at 0, weight at 16, italic 20, underline 21, strikeout 22,
 * face name at 28. Identical in both bitnesses -- every field is fixed width
 * and the name is inline, which is why this one struct needs no offset table. */
static void logfont(w32 *w, uint64_t p, int wide) {
    stock_init();
    if (!p) { RET(0); return; }
    char face[32];
    if (wide) w32_wtoa(w, p + 28, face, sizeof face);
    else snprintf(face, sizeof face, "%.31s", (const char *)W32P(w, p + 28));
    gobj *o = mk_font((int)(int32_t)(uint32_t)w32_read(w, p, 4),
                      (int)(int32_t)(uint32_t)w32_read(w, p + 16, 4), face);
    if (!o) { RET(0); return; }
    o->italic    = (int)w32_read(w, p + 20, 1);
    o->underline = (int)w32_read(w, p + 21, 1);
    o->strikeout = (int)w32_read(w, p + 22, 1);
    RET(handle_of(o));
}
static void g_CreateFontIndirectA(w32 *w) { logfont(w, ARG(0), 0); }
static void g_CreateFontIndirectW(w32 *w) { logfont(w, ARG(0), 1); }
static void g_CreateFontA(w32 *w) {
    stock_init();
    gobj *o = mk_font((int)(int32_t)(uint32_t)ARG(0), (int)ARG(4), ARG(13) ? GSTR(ARG(13)) : "");
    if (o) { o->italic = (int)ARG(5); o->underline = (int)ARG(6); o->strikeout = (int)ARG(7); }
    RET(o ? handle_of(o) : 0);
}
static void g_CreateFontW(w32 *w) {
    stock_init();
    char face[32]; face[0] = 0;
    if (ARG(13)) w32_wtoa(w, ARG(13), face, sizeof face);
    gobj *o = mk_font((int)(int32_t)(uint32_t)ARG(0), (int)ARG(4), face);
    if (o) { o->italic = (int)ARG(5); o->underline = (int)ARG(6); o->strikeout = (int)ARG(7); }
    RET(o ? handle_of(o) : 0);
}

/* Deleting an object still selected into a DC is a program error on Windows
 * and fails there; it fails here too, rather than leaving a DC pointing at a
 * freed slot. */
static void g_DeleteObject(w32 *w) {
    gobj *o = obj_of(ARG(0));
    if (!o) { RET(0); return; }
    if (o->stock) { RET(1); return; }
    uint64_t h = ARG(0);
    for (int i = 0; i < MAX_DC; i++)
        if (g_dc[i].used && (g_dc[i].pen == h || g_dc[i].brush == h || g_dc[i].font == h || g_dc[i].bitmap == h)) {
            RET(0); return;
        }
    free(o->bits);
    memset(o, 0, sizeof *o);
    RET(1);
}

static void g_SelectObject(w32 *w) {
    gdc *d = dc_of(ARG(0));
    gobj *o = obj_of(ARG(1));
    if (!d || !o) { RET(0); return; }
    uint64_t old = 0;
    switch (o->type) {
    case G_PEN:    old = d->pen;    d->pen = ARG(1);    break;
    case G_BRUSH:  old = d->brush;  d->brush = ARG(1);  break;
    case G_FONT:   old = d->font;   d->font = ARG(1);   break;
    case G_BITMAP:
        old = d->bitmap;
        /* Only a memory DC has a bitmap to swap; on a screen DC this is a
         * no-op on Windows too. */
        if (!d->hwnd && o->bits) {
            if (d->own_bits) { /* the previous bitmap keeps its own pixels */ }
            d->bits = o->bits; d->sw = o->bw; d->sh = o->bh; d->own_bits = 0;
            d->ox = d->oy = 0; d->cl = d->ct = 0; d->cr = o->bw; d->cb = o->bh;
            d->bitmap = ARG(1);
        }
        break;
    default: RET(0); return;
    }
    RET(old);
}

static void g_GetObjectA(w32 *w) {
    gobj *o = obj_of(ARG(0));
    uint64_t n = ARG(1), p = ARG(2);
    if (!o) { RET(0); return; }
    if (!p) { RET(o->type == G_FONT ? 60 : o->type == G_BITMAP ? 24 : 12); return; }
    if (o->type == G_BRUSH && n >= 12) {
        w32_write(w, p, 4, (uint64_t)(uint32_t)o->style);
        w32_write(w, p + 4, 4, o->color);
        RET(12); return;
    }
    if (o->type == G_BITMAP && n >= 24) {
        w32_write(w, p, 4, 0);
        w32_write(w, p + 4, 4, (uint64_t)(uint32_t)o->bw);
        w32_write(w, p + 8, 4, (uint64_t)(uint32_t)o->bh);
        w32_write(w, p + 12, 4, (uint64_t)(uint32_t)(o->bw * 4));
        w32_write(w, p + 16, 2, 1);
        w32_write(w, p + 18, 2, 32);
        RET(24); return;
    }
    if (o->type == G_FONT && n >= 28) {
        w32_write(w, p, 4, (uint64_t)(uint32_t)o->height);
        w32_write(w, p + 16, 4, (uint64_t)(uint32_t)o->weight);
        RET(n < 60 ? n : 60); return;
    }
    RET(0);
}
static void g_GetObjectW(w32 *w) { g_GetObjectA(w); }

/* Device capabilities. The numbers a program reads here decide its layout, so
 * they have to be the real ones for the surface we actually have. */
static void g_GetDeviceCaps(w32 *w) {
    int sw = 0, sh = 0;
    (void)w32_desktop_bits(&sw, &sh);
    switch ((int)ARG(1)) {
    case 8:   RET(sw); return;              /* HORZRES */
    case 10:  RET(sh); return;              /* VERTRES */
    case 4:   RET(sw * 254 / (g_dpi * 10)); return;   /* HORZSIZE, in mm */
    case 6:   RET(sh * 254 / (g_dpi * 10)); return;   /* VERTSIZE */
    case 12:  RET(32); return;              /* BITSPIXEL */
    case 14:  RET(1);  return;              /* PLANES */
    case 88:  RET((uint64_t)(uint32_t)g_dpi); return;   /* LOGPIXELSX */
    case 90:  RET((uint64_t)(uint32_t)g_dpi); return;   /* LOGPIXELSY */
    case 104: RET(1);  return;              /* NUMCOLORS: direct colour */
    case 24:  RET(0);  return;              /* NUMBRUSHES */
    case 38:  RET(0);  return;              /* RASTERCAPS */
    case 2:   RET(1);  return;              /* TECHNOLOGY: DT_RASDISPLAY */
    default:  RET(0);  return;
    }
}

static void g_SetTextColor(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0xFFFFFFFFu); return; }
    uint32_t old = d->textcolor; d->textcolor = (uint32_t)ARG(1); RET(old);
}
static void g_GetTextColor(w32 *w) { gdc *d = dc_of(ARG(0)); RET(d ? d->textcolor : 0); }
static void g_SetBkColor(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0xFFFFFFFFu); return; }
    uint32_t old = d->bkcolor; d->bkcolor = (uint32_t)ARG(1); RET(old);
}
static void g_GetBkColor(w32 *w) { gdc *d = dc_of(ARG(0)); RET(d ? d->bkcolor : 0xFFFFFF); }
static void g_SetBkMode(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    int old = d->bkmode; d->bkmode = (int)ARG(1); RET(old);
}
static void g_GetBkMode(w32 *w) { gdc *d = dc_of(ARG(0)); RET(d ? d->bkmode : OPAQUE_); }
static void g_SetTextAlign(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0xFFFFFFFFu); return; }
    int old = d->align; d->align = (int)ARG(1); RET(old);
}
static void g_GetTextAlign(w32 *w) { gdc *d = dc_of(ARG(0)); RET(d ? (uint64_t)d->align : 0); }

/* TextOut honours SetTextAlign, because a program that centres its own text
 * does it by setting TA_CENTER and passing the centre, and ignoring that puts
 * every caption in the wrong place. */
static void text_out(gdc *d, int x, int y, const char *s, int len) {
    int cell, ascent, bold;
    font_metrics(obj_of(d->font), &cell, &ascent, &bold);
    int width = run_width(cell, s, len);
    if ((d->align & 6) == TA_CENTER) x -= width / 2;
    else if ((d->align & 6) == TA_RIGHT) x -= width;
    if (d->align & TA_BASELINE) y -= ascent;
    else if (d->align & TA_BOTTOM) y -= cell;
    draw_text_run(d, x, y, s, len);
}

static void g_TextOutA(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d || !ARG(3)) { RET(0); return; }
    int len = (int)(int32_t)(uint32_t)ARG(4);
    const char *s = (const char *)W32P(w, ARG(3));
    if (!s) { RET(0); return; }
    if (len < 0) len = (int)strlen(s);
    text_out(d, (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2), s, len);
    RET(1);
}
static void g_TextOutW(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d || !ARG(3)) { RET(0); return; }
    char buf[512];
    w32_wtoa_n(w, ARG(3), (int)(int32_t)(uint32_t)ARG(4), buf, sizeof buf);
    text_out(d, (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2), buf, (int)strlen(buf));
    RET(1);
}
/* ExtTextOut(hdc, x, y, options, rect, str, len, dx). ETO_OPAQUE fills the
 * rectangle first, which is how a control clears its own background. */
static void g_ExtTextOutA(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    uint32_t opt = (uint32_t)ARG(3);
    uint64_t rp = ARG(4);
    if (rp && (opt & 3)) {                     /* ETO_OPAQUE | ETO_CLIPPED */
        int l = (int)(int32_t)w32_read(w, rp, 4),      t = (int)(int32_t)w32_read(w, rp + 4, 4);
        int r = (int)(int32_t)w32_read(w, rp + 8, 4),  b = (int)(int32_t)w32_read(w, rp + 12, 4);
        if (opt & 2) fill(d, l, t, r, b, px(d->bkcolor));
    }
    if (!ARG(5)) { RET(1); return; }
    const char *s = (const char *)W32P(w, ARG(5));
    int len = (int)(int32_t)(uint32_t)ARG(6);
    if (!s) { RET(1); return; }
    if (len < 0) len = (int)strlen(s);
    int saved = d->bkmode;
    d->bkmode = (opt & 2) ? TRANSPARENT_ : d->bkmode;
    text_out(d, (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2), s, len);
    d->bkmode = saved;
    RET(1);
}
static void g_ExtTextOutW(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    char buf[512];
    w32_wtoa_n(w, ARG(5), (int)(int32_t)(uint32_t)ARG(6), buf, sizeof buf);
    uint64_t rp = ARG(4);
    uint32_t opt = (uint32_t)ARG(3);
    if (rp && (opt & 2)) {
        int l = (int)(int32_t)w32_read(w, rp, 4),      t = (int)(int32_t)w32_read(w, rp + 4, 4);
        int r = (int)(int32_t)w32_read(w, rp + 8, 4),  b = (int)(int32_t)w32_read(w, rp + 12, 4);
        fill(d, l, t, r, b, px(d->bkcolor));
    }
    int saved = d->bkmode;
    d->bkmode = (opt & 2) ? TRANSPARENT_ : d->bkmode;
    text_out(d, (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2), buf, (int)strlen(buf));
    d->bkmode = saved;
    RET(1);
}

static void extent(w32 *w, const char *s, int len, uint64_t hdc, uint64_t out) {
    int cx = 0, cy = 0;
    w32_gdi_text_extent(hdc, s, len, &cx, &cy);
    if (out) { w32_write(w, out, 4, (uint64_t)(uint32_t)cx); w32_write(w, out + 4, 4, (uint64_t)(uint32_t)cy); }
    RET(1);
}
static void g_GetTextExtentPoint32A(w32 *w) {
    const char *s = ARG(1) ? (const char *)W32P(w, ARG(1)) : "";
    int len = (int)(int32_t)(uint32_t)ARG(2);
    extent(w, s ? s : "", len, ARG(0), ARG(3));
}
static void g_GetTextExtentPoint32W(w32 *w) {
    char buf[1024];
    w32_wtoa_n(w, ARG(1), (int)(int32_t)(uint32_t)ARG(2), buf, sizeof buf);
    extent(w, buf, (int)strlen(buf), ARG(0), ARG(3));
}
static void g_GetTextExtentPointA(w32 *w)   { g_GetTextExtentPoint32A(w); }
static void g_GetTextExtentPointW(w32 *w)   { g_GetTextExtentPoint32W(w); }

/* TEXTMETRIC, in the order the struct declares: height, ascent, descent,
 * internal leading, external leading, ave char width, max char width, ... */
static void g_GetTextMetricsA(w32 *w) {
    gdc *d = dc_of(ARG(0));
    uint64_t p = ARG(1);
    if (!d || !p) { RET(0); return; }
    int cell, ascent, bold;
    font_metrics(obj_of(d->font), &cell, &ascent, &bold);
    int avg = w32_gdi_average_width(ARG(0));
    int wide = glyph_advance(cell, 'W');
    int32_t v[8] = { cell, ascent, cell - ascent, 0, 0, avg, wide, bold ? 700 : 400 };
    for (int i = 0; i < 8; i++) w32_write(w, p + (unsigned)i * 4, 4, (uint64_t)(uint32_t)v[i]);
    RET(1);
}
static void g_GetTextMetricsW(w32 *w) { g_GetTextMetricsA(w); }

/* Rectangle/Ellipse fill with the brush and outline with the pen. A NULL_PEN
 * or NULL_BRUSH skips its half, which is how a program draws a frame with no
 * fill or a fill with no frame. */
static void rect_prim(w32 *w, int ellipse) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    int l = (int)(int32_t)(uint32_t)ARG(1), t = (int)(int32_t)(uint32_t)ARG(2);
    int r = (int)(int32_t)(uint32_t)ARG(3), b = (int)(int32_t)(uint32_t)ARG(4);
    gobj *br = obj_of(d->brush), *pn = obj_of(d->pen);
    if (br && br->style != BS_NULL) {
        if (!ellipse) fill(d, l, t, r, b, px(br->color));
        else {
            /* Integer midpoint fill: for each row, the half-width from the
             * ellipse equation, so no floating point and the same pixels
             * everywhere. */
            int cx = (l + r) / 2, cy = (t + b) / 2, a = (r - l) / 2, c = (b - t) / 2;
            if (a > 0 && c > 0) for (int y = -c; y <= c; y++) {
                int64_t k = (int64_t)a * a * ((int64_t)c * c - (int64_t)y * y);
                int hw = 0; while ((int64_t)(hw + 1) * (hw + 1) * c * c <= k) hw++;
                fill(d, cx - hw, cy + y, cx + hw + 1, cy + y + 1, px(br->color));
            }
        }
    }
    if (pn && pn->style != PS_NULL) {
        uint32_t p = px(pn->color);
        line(d, l, t, r - 1, t, p, pn->width);
        line(d, l, b - 1, r - 1, b - 1, p, pn->width);
        line(d, l, t, l, b - 1, p, pn->width);
        line(d, r - 1, t, r - 1, b - 1, p, pn->width);
    }
    RET(1);
}
static void g_Rectangle(w32 *w) { rect_prim(w, 0); }
static void g_Ellipse(w32 *w)   { rect_prim(w, 1); }
static void g_RoundRect(w32 *w) { rect_prim(w, 0); }

static void g_MoveToEx(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    if (ARG(3)) {
        w32_write(w, ARG(3), 4, (uint64_t)(uint32_t)d->cx);
        w32_write(w, ARG(3) + 4, 4, (uint64_t)(uint32_t)d->cy);
    }
    d->cx = (int)(int32_t)(uint32_t)ARG(1);
    d->cy = (int)(int32_t)(uint32_t)ARG(2);
    RET(1);
}
static void g_LineTo(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    gobj *pn = obj_of(d->pen);
    int x = (int)(int32_t)(uint32_t)ARG(1), y = (int)(int32_t)(uint32_t)ARG(2);
    if (pn && pn->style != PS_NULL) line(d, d->cx, d->cy, x, y, px(pn->color), pn->width);
    d->cx = x; d->cy = y;
    RET(1);
}
static void g_Polyline(w32 *w) {
    gdc *d = dc_of(ARG(0));
    uint64_t p = ARG(1);
    int n = (int)(int32_t)(uint32_t)ARG(2);
    if (!d || !p || n < 2) { RET(0); return; }
    gobj *pn = obj_of(d->pen);
    if (!pn || pn->style == PS_NULL) { RET(1); return; }
    int px0 = (int)(int32_t)w32_read(w, p, 4), py0 = (int)(int32_t)w32_read(w, p + 4, 4);
    for (int i = 1; i < n && i < 4096; i++) {
        int x = (int)(int32_t)w32_read(w, p + (unsigned)i * 8, 4);
        int y = (int)(int32_t)w32_read(w, p + (unsigned)i * 8 + 4, 4);
        line(d, px0, py0, x, y, px(pn->color), pn->width);
        px0 = x; py0 = y;
    }
    RET(1);
}
static void g_SetPixel(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0xFFFFFFFFu); return; }
    plot(d, (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2), px((uint32_t)ARG(3)));
    RET(ARG(3));
}
static void g_GetPixel(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0xFFFFFFFFu); return; }
    int x = (int)(int32_t)(uint32_t)ARG(1) + d->ox, y = (int)(int32_t)(uint32_t)ARG(2) + d->oy;
    if (x < d->cl || x >= d->cr || y < d->ct || y >= d->cb) { RET(0xFFFFFFFFu); return; }
    RET(ref_of(d->bits[(size_t)y * d->sw + x]));
}
/* PatBlt with PATCOPY paints the brush; BLACKNESS and WHITENESS are the two
 * a program uses to clear. */
static void g_PatBlt(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    int x = (int)(int32_t)(uint32_t)ARG(1), y = (int)(int32_t)(uint32_t)ARG(2);
    int cw = (int)(int32_t)(uint32_t)ARG(3), ch = (int)(int32_t)(uint32_t)ARG(4);
    uint32_t rop = (uint32_t)ARG(5);
    gobj *br = obj_of(d->brush);
    uint32_t c = rop == 0x42u ? 0x000000u : rop == 0xFF0062u ? 0xFFFFFFu : (br ? br->color : 0xFFFFFF);
    if (!(br && br->style == BS_NULL && rop == 0x00F00021u)) fill(d, x, y, x + cw, y + ch, px(c));
    RET(1);
}

/* --------------------------------------------------------- memory DCs */

static void g_CreateCompatibleDC(w32 *w) {
    int sw = 0, sh = 0;
    (void)w32_desktop_bits(&sw, &sh);
    for (int i = 0; i < MAX_DC; i++) if (!g_dc[i].used) {
        gdc *d = &g_dc[i];
        memset(d, 0, sizeof *d);
        d->used = 1;
        /* A fresh memory DC holds a 1x1 monochrome bitmap on Windows; a
         * program is expected to select a real one before it draws. Giving it
         * one pixel rather than nothing means a stray draw clips instead of
         * writing through a null pointer. */
        d->bits = calloc(1, sizeof(uint32_t));
        if (!d->bits) { d->used = 0; RET(0); return; }
        d->own_bits = 1; d->sw = d->sh = 1; d->cr = d->cb = 1;
        dc_defaults(d);
        RET(dch_of(d));
        return;
    }
    RET(0);
}
static void g_DeleteDC(w32 *w) { w32_dc_release(ARG(0)); RET(1); }

static void g_CreateCompatibleBitmap(w32 *w) {
    int cw = (int)(int32_t)(uint32_t)ARG(1), ch = (int)(int32_t)(uint32_t)ARG(2);
    if (cw <= 0 || ch <= 0 || (int64_t)cw * ch > 64 * 1024 * 1024) { RET(0); return; }
    stock_init();
    gobj *o = obj_new(G_BITMAP);
    if (!o) { RET(0); return; }
    o->bw = cw; o->bh = ch;
    o->bits = calloc((size_t)cw * ch, sizeof(uint32_t));
    if (!o->bits) { memset(o, 0, sizeof *o); RET(0); return; }
    for (size_t i = 0; i < (size_t)cw * ch; i++) o->bits[i] = 0xFF000000u;
    RET(handle_of(o));
}
/* CreateDIBSection(hdc, bmi, usage, ppvBits, hSection, offset): a 32-bit
 * top-down section is what almost every caller asks for, and the guest has to
 * be able to write to the pixels, so the bits live in guest memory. */
static void g_CreateDIBSection(w32 *w) {
    uint64_t bmi = ARG(1), out = ARG(3);
    if (!bmi) { RET(0); return; }
    int cw = (int)(int32_t)w32_read(w, bmi + 4, 4);
    int ch = (int)(int32_t)w32_read(w, bmi + 8, 4);
    int bpp = (int)w32_read(w, bmi + 14, 2);
    int flip = ch < 0;                          /* negative height: top-down */
    if (ch < 0) ch = -ch;
    (void)flip;
    if (cw <= 0 || ch <= 0 || (int64_t)cw * ch > 32 * 1024 * 1024) { RET(0); return; }
    if (bpp != 32 && bpp != 24 && bpp != 0) { RET(0); return; }
    stock_init();
    gobj *o = obj_new(G_BITMAP);
    if (!o) { RET(0); return; }
    o->bw = cw; o->bh = ch;
    uint64_t g = w32_alloc(w, (uint64_t)cw * ch * 4 + 4096, 0);
    if (!g) { memset(o, 0, sizeof *o); RET(0); return; }
    o->bits = (uint32_t *)W32P(w, g);
    if (out) w32_write(w, out, (int)w32_ptrsize(w), g);
    RET(handle_of(o));
}

/* BitBlt(dst, x, y, w, h, src, sx, sy, rop). SRCCOPY is the one that matters;
 * the pattern ROPs go to PatBlt's colours so a clear still clears. */
static void blt(w32 *w, int stretch) {
    gdc *dd = dc_of(ARG(0));
    if (!dd) { RET(0); return; }
    int dx = (int)(int32_t)(uint32_t)ARG(1), dy = (int)(int32_t)(uint32_t)ARG(2);
    int dw = (int)(int32_t)(uint32_t)ARG(3), dh = (int)(int32_t)(uint32_t)ARG(4);
    gdc *sd = dc_of(ARG(5));
    int sx = (int)(int32_t)(uint32_t)ARG(6), sy = (int)(int32_t)(uint32_t)ARG(7);
    int sw2 = stretch ? (int)(int32_t)(uint32_t)ARG(8) : dw;
    int sh2 = stretch ? (int)(int32_t)(uint32_t)ARG(9) : dh;
    uint32_t rop = (uint32_t)(stretch ? ARG(10) : ARG(8));
    if (!sd || rop != 0x00CC0020u) {            /* not SRCCOPY: fall back to the brush */
        gobj *br = obj_of(dd->brush);
        uint32_t c = rop == 0x42u ? 0x000000u : rop == 0xFF0062u ? 0xFFFFFFu : (br ? br->color : 0xFFFFFF);
        if (sd && rop != 0x42u && rop != 0xFF0062u) { /* unknown ROP with a source: copy */ }
        else { fill(dd, dx, dy, dx + dw, dy + dh, px(c)); RET(1); return; }
    }
    if (!sd || dw <= 0 || dh <= 0 || sw2 <= 0 || sh2 <= 0) { RET(0); return; }
    for (int y = 0; y < dh; y++) {
        int syy = sy + (stretch ? y * sh2 / dh : y) + sd->oy;
        if (syy < sd->ct || syy >= sd->cb) continue;
        const uint32_t *srow = sd->bits + (size_t)syy * sd->sw;
        for (int x = 0; x < dw; x++) {
            int sxx = sx + (stretch ? x * sw2 / dw : x) + sd->ox;
            if (sxx < sd->cl || sxx >= sd->cr) continue;
            plot(dd, dx + x, dy + y, srow[sxx]);
        }
    }
    (void)w;
    RET(1);
}
static void g_BitBlt(w32 *w)     { blt(w, 0); }
static void g_StretchBlt(w32 *w) { blt(w, 1); }
static void g_SetStretchBltMode(w32 *w) { (void)w; RET(1); }

/* SaveDC/RestoreDC keep a small stack of the selected objects and modes,
 * which is all a program actually saves across a paint. */
enum { MAX_SAVE = 8 };
static struct { uint64_t hdc; int depth; gdc snap[MAX_SAVE]; } g_save[MAX_DC];
static void g_SaveDC(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    int i = (int)(d - g_dc);
    if (g_save[i].depth >= MAX_SAVE) { RET(0); return; }
    g_save[i].snap[g_save[i].depth++] = *d;
    RET(g_save[i].depth);
}
static void g_RestoreDC(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    int i = (int)(d - g_dc);
    int want = (int)(int32_t)(uint32_t)ARG(1);
    int to = want < 0 ? g_save[i].depth + want : want - 1;
    if (to < 0 || to >= g_save[i].depth) { RET(0); return; }
    int own = d->own_bits; uint32_t *bits = d->bits;
    *d = g_save[i].snap[to];
    d->own_bits = own; d->bits = bits;          /* never restore over a live allocation */
    g_save[i].depth = to;
    RET(1);
}

static void g_IntersectClipRect(w32 *w) {
    w32_gdi_clip_to(ARG(0), (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2),
                            (int)(int32_t)(uint32_t)ARG(3), (int)(int32_t)(uint32_t)ARG(4));
    RET(1);
}
static void g_SelectClipRgn(w32 *w) { (void)w; RET(1); }
static void g_ExcludeClipRect(w32 *w) { (void)w; RET(1); }
static void g_CreateRectRgn(w32 *w) {
    stock_init();
    gobj *o = obj_new(G_REGION);
    RET(o ? handle_of(o) : 0);
}
static void g_CreateRectRgnIndirect(w32 *w) { g_CreateRectRgn(w); }
/* Regions. Nothing here clips to a shape, so a region is a rectangle and
 * combining two of them is the bounding box -- which is what a caller
 * clipping a window to a rounded rectangle gets: the whole window. Visibly
 * square corners rather than a missing window. */
static void g_CombineRgn(w32 *w) { (void)w; RET(2); }        /* SIMPLEREGION */
static void g_GetRgnBox(w32 *w) {
    uint64_t r = ARG(1);
    if (!r) { RET(0); return; }
    int sw = 0, sh = 0;
    (void)w32_desktop_bits(&sw, &sh);
    w32_write(w, r,      4, 0);
    w32_write(w, r + 4,  4, 0);
    w32_write(w, r + 8,  4, (uint64_t)(uint32_t)sw);
    w32_write(w, r + 12, 4, (uint64_t)(uint32_t)sh);
    RET(2);
}
static void g_OffsetRgn(w32 *w) { (void)w; RET(2); }
static void g_SetRectRgn(w32 *w) { (void)w; RET(1); }
static void g_PtInRegion(w32 *w) { (void)w; RET(1); }

static void g_GetClipBox(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d || !ARG(1)) { RET(0); return; }
    uint64_t p = ARG(1);
    w32_write(w, p,      4, (uint64_t)(uint32_t)(d->cl - d->ox));
    w32_write(w, p + 4,  4, (uint64_t)(uint32_t)(d->ct - d->oy));
    w32_write(w, p + 8,  4, (uint64_t)(uint32_t)(d->cr - d->ox));
    w32_write(w, p + 12, 4, (uint64_t)(uint32_t)(d->cb - d->oy));
    RET(2);
}
static void g_SetMapMode(w32 *w) { (void)w; RET(1); }          /* MM_TEXT only */
static void g_GetMapMode(w32 *w) { (void)w; RET(1); }
static void g_SetViewportOrgEx(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    if (ARG(3)) { w32_write(w, ARG(3), 4, 0); w32_write(w, ARG(3) + 4, 4, 0); }
    RET(1);
}
static void g_GetDCBrushColor(w32 *w) { gdc *d = dc_of(ARG(0)); RET(d ? d->bkcolor : 0xFFFFFF); }
static void g_SetDCBrushColor(w32 *w) {
    gobj *o = obj_of(w32_stock_object(DC_BRUSH));
    uint32_t old = o ? o->color : 0;
    if (o) o->color = (uint32_t)ARG(1);
    RET(old);
}
static void g_SetDCPenColor(w32 *w) {
    gobj *o = obj_of(w32_stock_object(DC_PEN));
    uint32_t old = o ? o->color : 0;
    if (o) o->color = (uint32_t)ARG(1);
    RET(old);
}
static void g_GdiFlush(w32 *w) { (void)w; RET(1); }
static void g_GdiSetBatchLimit(w32 *w) { (void)w; RET(1); }
static void g_GetCurrentObject(w32 *w) {
    gdc *d = dc_of(ARG(0));
    if (!d) { RET(0); return; }
    switch ((int)ARG(1)) {
    case 1:  RET(d->brush); return;             /* OBJ_BRUSH */
    case 2:  RET(d->pen);   return;             /* OBJ_PEN */
    case 6:  RET(d->font);  return;             /* OBJ_FONT */
    case 7:  RET(d->bitmap); return;            /* OBJ_BITMAP */
    default: RET(0); return;
    }
}
static void g_GetObjectType(w32 *w) {
    gobj *o = obj_of(ARG(0));
    if (dc_of(ARG(0))) { RET(3); return; }      /* OBJ_DC */
    if (!o) { RET(0); return; }
    switch (o->type) {
    case G_PEN:    RET(2); return;
    case G_BRUSH:  RET(1); return;
    case G_FONT:   RET(6); return;
    case G_BITMAP: RET(7); return;
    case G_REGION: RET(8); return;
    default:       RET(0); return;
    }
}

#define F(n, a) { #n, a, 0, g_##n, 0 }
const w32_api w32_gdi32[] = {
    F(GetStockObject, 1),
    F(CreateSolidBrush, 1), F(CreateBrushIndirect, 1), F(CreatePatternBrush, 1), F(CreateHatchBrush, 2),
    F(CreatePen, 3), F(CreatePenIndirect, 1),
    F(CreateFontIndirectA, 1), F(CreateFontIndirectW, 1), F(CreateFontA, 14), F(CreateFontW, 14),
    F(DeleteObject, 1), F(SelectObject, 2), F(GetObjectA, 3), F(GetObjectW, 3),
    F(GetObjectType, 1), F(GetCurrentObject, 2),
    F(GetDeviceCaps, 2),
    F(SetTextColor, 2), F(GetTextColor, 1), F(SetBkColor, 2), F(GetBkColor, 1),
    F(SetBkMode, 2), F(GetBkMode, 1), F(SetTextAlign, 2), F(GetTextAlign, 1),
    F(TextOutA, 5), F(TextOutW, 5), F(ExtTextOutA, 8), F(ExtTextOutW, 8),
    F(GetTextExtentPoint32A, 4), F(GetTextExtentPoint32W, 4),
    F(GetTextExtentPointA, 4), F(GetTextExtentPointW, 4),
    F(GetTextMetricsA, 2), F(GetTextMetricsW, 2),
    F(Rectangle, 5), F(Ellipse, 5), F(RoundRect, 7),
    F(MoveToEx, 4), F(LineTo, 3), F(Polyline, 3), F(SetPixel, 4), F(GetPixel, 3), F(PatBlt, 6),
    F(CreateCompatibleDC, 1), F(DeleteDC, 1), F(CreateCompatibleBitmap, 3), F(CreateDIBSection, 6),
    F(BitBlt, 9), F(StretchBlt, 11), F(SetStretchBltMode, 2),
    F(SaveDC, 1), F(RestoreDC, 2),
    F(IntersectClipRect, 5), F(SelectClipRgn, 2), F(ExcludeClipRect, 5),
    F(CreateRectRgn, 4), F(CreateRectRgnIndirect, 1), F(GetClipBox, 2),
    F(CombineRgn, 4), F(GetRgnBox, 2), F(OffsetRgn, 3), F(SetRectRgn, 5), F(PtInRegion, 3),
    F(SetMapMode, 2), F(GetMapMode, 1), F(SetViewportOrgEx, 4),
    F(GetDCBrushColor, 1), F(SetDCBrushColor, 2), F(SetDCPenColor, 2),
    F(GdiFlush, 0), F(GdiSetBatchLimit, 1),
    { 0, 0, 0, 0, 0 },
};
#undef F
