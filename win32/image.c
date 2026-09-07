/* DIBs, icons and cursors, turned into pixels. See image.h.
 *
 * The DIB is a format with more corners than its age suggests: rows are
 * padded to four bytes and stored bottom-up unless the height is negative,
 * the palette entries are BGRX rather than RGB, 16- and 32-bit forms can
 * carry arbitrary channel masks in place of the palette, and the 32-bit form
 * may or may not mean anything by its fourth byte. Each of those is one line
 * here and each of them is a picture drawn wrong if it is missing.
 */
#include "image.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void w32_image_free(w32_image *im) {
    if (im) { free(im->px); im->px = 0; im->w = im->h = 0; }
}

/* How far right a mask's lowest set bit is, and how wide it is: a 16-bit DIB
 * with BI_BITFIELDS can put red anywhere. */
static void mask_shift(uint32_t m, int *sh, int *bits) {
    int s = 0, b = 0;
    if (!m) { *sh = 0; *bits = 0; return; }
    while (!(m & 1)) { m >>= 1; s++; }
    while (m & 1) { m >>= 1; b++; }
    *sh = s; *bits = b;
}
/* Scale an n-bit channel to 8 bits by replication, which is what makes 5-bit
 * white come out 255 rather than 248. */
static uint32_t expand(uint32_t v, int bits) {
    if (bits <= 0) return 0;
    if (bits >= 8) return (v >> (bits - 8)) & 0xFF;
    uint32_t r = v << (8 - bits);
    return r | (r >> bits);
}

/* The shared core. `icon` says the header's height covers a colour image and
 * a 1-bit AND mask stacked under it. */
static int decode(const uint8_t *d, size_t n, int icon, w32_image *out) {
    if (!d || n < 40 || !out) return 0;
    uint32_t hdr = rd32(d);
    if (hdr < 40 || hdr > n) return 0;                 /* BITMAPCOREHEADER is 12 and long dead */
    int w = (int)(int32_t)rd32(d + 4);
    int h = (int)(int32_t)rd32(d + 8);
    int planes = (int)rd16(d + 12), bpp = (int)rd16(d + 14);
    uint32_t comp = rd32(d + 16);
    uint32_t clrused = rd32(d + 32);
    if (icon) h /= 2;
    int topdown = 0;
    if (h < 0) { h = -h; topdown = 1; }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || planes != 1) return 0;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    /* BI_RGB and BI_BITFIELDS only. RLE4/RLE8 exist and essentially nothing
     * ships them any more; refusing is better than half-decoding. */
    if (comp != 0 && comp != 3) return 0;

    size_t at = hdr;
    uint32_t rmask = 0, gmask = 0, bmask = 0;
    if (comp == 3) {
        if (at + 12 > n) return 0;
        rmask = rd32(d + at); gmask = rd32(d + at + 4); bmask = rd32(d + at + 8);
        /* A v4/v5 header carries the masks inside itself rather than after
         * it, and then the pixels start where the header ends. */
        if (hdr >= 56) { rmask = rd32(d + 40); gmask = rd32(d + 44); bmask = rd32(d + 48); }
        else at += 12;
    } else if (bpp == 16) { rmask = 0x7C00; gmask = 0x03E0; bmask = 0x001F; }
      else if (bpp == 32 || bpp == 24) { rmask = 0xFF0000; gmask = 0xFF00; bmask = 0xFF; }

    uint32_t pal[256];
    int npal = 0;
    if (bpp <= 8) {
        npal = clrused ? (int)clrused : (1 << bpp);
        if (npal > 256) npal = 256;
        if (at + (size_t)npal * 4 > n) return 0;
        for (int i = 0; i < npal; i++) {
            const uint8_t *e = d + at + (size_t)i * 4;
            pal[i] = 0xFF000000u | (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0];
        }
        at += (size_t)npal * 4;
    }

    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (at + stride * (size_t)h > n) return 0;

    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) return 0;

    int rs, rb, gs, gb, bs, bb;
    mask_shift(rmask, &rs, &rb); mask_shift(gmask, &gs, &gb); mask_shift(bmask, &bs, &bb);
    /* A 32-bit DIB's fourth byte is alpha only if something in it is not
     * zero. Plenty of tools write BI_RGB 32-bit with the high byte left at
     * zero, and honouring that literally makes the whole image invisible --
     * which is the single most common way an icon disappears. */
    int has_alpha = 0;
    if (bpp == 32) {
        for (size_t y = 0; y < (size_t)h && !has_alpha; y++) {
            const uint8_t *row = d + at + y * stride;
            for (int x = 0; x < w; x++) if (row[(size_t)x * 4 + 3]) { has_alpha = 1; break; }
        }
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + at + (size_t)(topdown ? y : h - 1 - y) * stride;
        uint32_t *dst = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t v;
            switch (bpp) {
            case 1:  { int i = (row[x >> 3] >> (7 - (x & 7))) & 1;
                       v = pal[i < npal ? i : 0]; break; }
            case 4:  { int i = (x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4);
                       v = pal[i < npal ? i : 0]; break; }
            case 8:  { int i = row[x]; v = pal[i < npal ? i : 0]; break; }
            case 16: { uint32_t s = rd16(row + (size_t)x * 2);
                       v = 0xFF000000u | expand((s & rmask) >> rs, rb) << 16
                                       | expand((s & gmask) >> gs, gb) << 8
                                       | expand((s & bmask) >> bs, bb); break; }
            case 24: { const uint8_t *p = row + (size_t)x * 3;
                       v = 0xFF000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; break; }
            default: { const uint8_t *p = row + (size_t)x * 4;
                       uint32_t a = has_alpha ? p[3] : 0xFF;
                       v = a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; break; }
            }
            dst[x] = v;
        }
    }

    /* The AND mask. Where it is set the pixel is transparent -- which is how
     * every icon below 32-bit says so, and how a 32-bit icon whose alpha
     * channel a tool left blank still comes out with a shape. */
    if (icon && !(bpp == 32 && has_alpha)) {
        size_t mstride = (((size_t)w + 31) / 32) * 4;
        size_t mat = at + stride * (size_t)h;
        if (mat + mstride * (size_t)h <= n) {
            for (int y = 0; y < h; y++) {
                const uint8_t *row = d + mat + (size_t)(h - 1 - y) * mstride;
                uint32_t *dst = px + (size_t)y * w;
                for (int x = 0; x < w; x++)
                    if ((row[x >> 3] >> (7 - (x & 7))) & 1) dst[x] &= 0x00FFFFFFu;
            }
        }
    }

    out->w = w; out->h = h; out->px = px;
    return 1;
}

int w32_dib_decode(const uint8_t *d, size_t n, w32_image *out)  { return decode(d, n, 0, out); }
int w32_icon_decode(const uint8_t *d, size_t n, w32_image *out) { return decode(d, n, 1, out); }

/* ------------------------------------------------------------- icon sets */

/* Pick an image out of a directory. Preferring the exact size asked for, then
 * the next size up (scaling down looks far better than scaling up), then the
 * largest there is. A PNG entry -- the 256x256 form Vista introduced -- is
 * skipped: decoding it needs an inflate, which this layer deliberately does
 * not depend on, and every icon that has one also has a DIB at the sizes a
 * title bar or a list actually uses. */
static int score_size(int want, int sz) {
    if (!want) return sz;                         /* largest wins */
    if (sz == want) return 1000000;
    if (sz > want) return 500000 - (sz - want);   /* down-scale: good */
    return sz;                                    /* up-scale: last resort */
}

int w32_icon_group(const uint8_t *dir, size_t n, int want,
                   w32_res_fetch fetch, void *ctx, w32_image *out, int *hx, int *hy) {
    if (!dir || n < 6 || !fetch) return 0;
    int type = (int)rd16(dir + 2);                /* 1 icon, 2 cursor */
    int count = (int)rd16(dir + 4);
    if (count <= 0 || count > 256 || 6 + (size_t)count * 14 > n) return 0;
    int best = -1, best_score = -1, best_id = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = dir + 6 + (size_t)i * 14;
        int sz = e[0] ? e[0] : 256;
        int s = score_size(want, sz);
        if (s > best_score) { best_score = s; best = i; best_id = (int)rd16(e + 12); }
    }
    if (best < 0) return 0;
    uint32_t len = 0;
    const uint8_t *img = fetch(ctx, best_id, &len);
    if (!img || len < 40) return 0;
    /* A PNG entry starts with the eight-byte signature. Try the next-best
     * DIB rather than giving up on the icon. */
    if (img[0] == 0x89 && img[1] == 'P') {
        int alt = -1, alt_score = -1, alt_id = 0;
        for (int i = 0; i < count; i++) {
            if (i == best) continue;
            const uint8_t *e = dir + 6 + (size_t)i * 14;
            int sz = e[0] ? e[0] : 256;
            int s = score_size(want, sz);
            if (s > alt_score) { alt_score = s; alt = i; alt_id = (int)rd16(e + 12); }
        }
        if (alt < 0) return 0;
        img = fetch(ctx, alt_id, &len);
        if (!img || len < 40 || (img[0] == 0x89 && img[1] == 'P')) return 0;
        best = alt;
    }
    if (type == 2) {
        /* A cursor resource is two words of hot spot, then the DIB. */
        if (len < 4) return 0;
        if (hx) *hx = (int)rd16(img);
        if (hy) *hy = (int)rd16(img + 2);
        return w32_icon_decode(img + 4, len - 4, out);
    }
    if (hx) *hx = 0;
    if (hy) *hy = 0;
    return w32_icon_decode(img, len, out);
}

/* A whole .ico or .cur file: the same directory, but each entry carries an
 * offset into this file rather than a resource id. */
int w32_ico_file(const uint8_t *d, size_t n, int want, w32_image *out, int *hx, int *hy) {
    if (!d || n < 6) return 0;
    int type = (int)rd16(d + 2), count = (int)rd16(d + 4);
    if (count <= 0 || count > 256 || 6 + (size_t)count * 16 > n) return 0;
    int best = -1, best_score = -1;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = d + 6 + (size_t)i * 16;
        int sz = e[0] ? e[0] : 256;
        int s = score_size(want, sz);
        uint32_t off = rd32(e + 12), len = rd32(e + 8);
        if ((size_t)off + len > n || len < 40) continue;
        if (d[off] == 0x89 && d[off + 1] == 'P') continue;      /* PNG entry */
        if (s > best_score) { best_score = s; best = i; }
    }
    if (best < 0) return 0;
    const uint8_t *e = d + 6 + (size_t)best * 16;
    uint32_t off = rd32(e + 12), len = rd32(e + 8);
    if (hx) *hx = type == 2 ? (int)rd16(e + 4) : 0;
    if (hy) *hy = type == 2 ? (int)rd16(e + 6) : 0;
    return w32_icon_decode(d + off, len, out);
}

/* ------------------------------------------------------- what the system owns */

/* The stock cursors, as 1-bit art. They are drawn here because they belong to
 * the system rather than to any program: SetCursor(LoadCursor(NULL, IDC_WAIT))
 * has to show an hourglass, and there is nowhere else for one to come from.
 *
 * Each row is a string: ' ' transparent, '.' white, 'X' black. That is the
 * same three-value encoding the real ones use -- a colour image and a mask --
 * and it reads as the picture it draws, which a hex table would not.
 */
static const char *CUR_ARROW[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ",
    "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
    "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ",
    "      XX    ", 0
};
static const char *CUR_IBEAM[] = {
    "XXX XXX", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ",
    "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ",
    "   X   ", "XXX XXX", 0
};
static const char *CUR_WAIT[] = {
    "XXXXXXXXXXXX", "X..........X", "X..........X", " X........X ",
    " X.XXXXXX.X ", "  X.XXXX.X  ", "   X.XX.X   ", "   X.XX.X   ",
    "  X.X..X.X  ", " X.X....X.X ", " X.XXXXXX.X ", "X..........X",
    "X....XX....X", "XXXXXXXXXXXX", 0
};
static const char *CUR_CROSS[] = {
    "     X     ", "     X     ", "     X     ", "     X     ",
    "     X     ", "XXXXXXXXXXX", "     X     ", "     X     ",
    "     X     ", "     X     ", "     X     ", 0
};
static const char *CUR_HAND[] = {
    "   XX       ", "  X..X      ", "  X..X      ", "  X..X      ",
    "  X..XXX    ", "  X..X..XXX ", "  X..X..X..X", "  X..X..X..X",
    " XX..X..X..X", "X..X.......X", "X..........X", " X.........X",
    "  X........X", "   X.......X", "    X......X", "     XXXXXXX", 0
};
static const char *CUR_SIZENS[] = {
    "    X    ", "   X.X   ", "  X...X  ", " X.....X ", "XXXX.XXXX",
    "   X.X   ", "   X.X   ", "   X.X   ", "XXXX.XXXX", " X.....X ",
    "  X...X  ", "   X.X   ", "    X    ", 0
};
static const char *CUR_SIZEWE[] = {
    "   X       X   ", "  X.X     X.X  ", " X..X     X..X ", "X...XXXXXXX...X",
    "X.............X", "X...XXXXXXX...X", " X..X     X..X ", "  X.X     X.X  ",
    "   X       X   ", 0
};

static int from_art(const char **art, w32_image *out) {
    int h = 0, w = 0;
    for (; art[h]; h++) { int l = (int)strlen(art[h]); if (l > w) w = l; }
    if (!h || !w) return 0;
    uint32_t *px = (uint32_t *)calloc((size_t)w * h, 4);
    if (!px) return 0;
    for (int y = 0; y < h; y++) {
        int l = (int)strlen(art[y]);
        for (int x = 0; x < w; x++) {
            char c = x < l ? art[y][x] : ' ';
            px[(size_t)y * w + x] = c == 'X' ? 0xFF000000u
                                  : c == '.' ? 0xFFFFFFFFu : 0u;
        }
    }
    out->w = w; out->h = h; out->px = px;
    return 1;
}

int w32_stock_cursor(int id, w32_image *out, int *hx, int *hy) {
    const char **art;
    int ax = 0, ay = 0;
    switch (id) {
    case 32513: art = CUR_IBEAM;  ax = 3; ay = 7;  break;   /* IDC_IBEAM */
    case 32514: art = CUR_WAIT;   ax = 6; ay = 7;  break;   /* IDC_WAIT */
    case 32515: art = CUR_CROSS;  ax = 5; ay = 5;  break;   /* IDC_CROSS */
    case 32642: art = CUR_SIZENS; ax = 4; ay = 6;  break;   /* IDC_SIZENWSE-ish */
    case 32643: art = CUR_SIZENS; ax = 4; ay = 6;  break;
    case 32644: art = CUR_SIZEWE; ax = 7; ay = 4;  break;   /* IDC_SIZEWE */
    case 32645: art = CUR_SIZENS; ax = 4; ay = 6;  break;   /* IDC_SIZENS */
    case 32649: art = CUR_HAND;   ax = 5; ay = 0;  break;   /* IDC_HAND */
    default:    art = CUR_ARROW;  ax = 0; ay = 0;  break;   /* IDC_ARROW and the rest */
    }
    if (hx) *hx = ax;
    if (hy) *hy = ay;
    return from_art(art, out);
}

/* The message-box symbols and the default application icon. Drawn rather than
 * stored, because a circle and a bar scale to whatever size is asked for and
 * a 32x32 bitmap does not -- and MessageBox on a tablet asks for a big one. */
int w32_stock_icon(int id, int size, w32_image *out) {
    if (size < 8) size = 8;
    if (size > 256) size = 256;
    uint32_t *px = (uint32_t *)calloc((size_t)size * size, 4);
    if (!px) return 0;
    int r = size / 2 - 1;
    int cx = size / 2, cy = size / 2;
    /* IDI_ERROR 32513, IDI_QUESTION 32514, IDI_WARNING 32515,
     * IDI_INFORMATION 32516; anything else is the application icon. */
    uint32_t face = id == 32513 ? 0xFFCC2222u
                  : id == 32515 ? 0xFFDDAA00u
                  : id == 32514 || id == 32516 ? 0xFF2266CCu
                  : 0xFF7799BBu;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        int dx = x - cx, dy = y - cy;
        int inside;
        if (id == 32515) {
            /* A triangle, which is what a warning is. */
            int ty = y - 1, h = size - 2;
            inside = ty >= 0 && ty < h && (x * 2 >= size - ty * (size - 2) / h - 2)
                     && (x * 2 <= size + ty * (size - 2) / h + 2);
        } else if (id == 32513 || id == 32514 || id == 32516) {
            inside = dx * dx + dy * dy <= r * r;
        } else {
            inside = x >= size / 8 && x < size - size / 8 && y >= size / 8 && y < size - size / 8;
        }
        px[(size_t)y * size + x] = inside ? face : 0u;
    }
    /* The glyph on top, in white: a cross, a bar and dot, or a question mark
     * approximated by a bar and dot as well -- at 32 pixels the difference is
     * two pixels and the colour is what people read. */
    int t = size / 10 + 1;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        int dx = x - cx, dy = y - cy, hit = 0;
        if (id == 32513) {                                  /* X */
            hit = (dx - dy < t && dy - dx < t && dx > -r / 2 && dx < r / 2)
               || (dx + dy < t && -(dx + dy) < t && dx > -r / 2 && dx < r / 2);
        } else if (id == 32515 || id == 32516 || id == 32514) {
            int top = id == 32516 ? 1 : 0;                  /* i has the dot on top */
            int bar_lo = top ? -r / 4 : -r / 2, bar_hi = top ? r / 2 : r / 4;
            hit = (dx > -t / 2 - 1 && dx <= t / 2 && dy >= bar_lo && dy <= bar_hi)
               || (dx > -t / 2 - 1 && dx <= t / 2
                   && dy >= (top ? -r * 3 / 4 : r / 2) && dy <= (top ? -r / 2 : r * 3 / 4));
        }
        if (hit && px[(size_t)y * size + x]) px[(size_t)y * size + x] = 0xFFFFFFFFu;
    }
    out->w = size; out->h = size; out->px = px;
    return 1;
}
