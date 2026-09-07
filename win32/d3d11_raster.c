/* The triangles a Direct3D 11 draw call turns into.
 *
 * `raster.c` next door draws a gouraud triangle for d3d9 and is the wrong
 * shape for this: a D3D11 draw carries a texture, a sampler, per-vertex
 * texture coordinates and a blend mode, and a 2D game's entire output is
 * "this sprite, at this place, tinted this colour, blended over what is
 * already there". So this is that triangle.
 *
 * The arithmetic is integer, for the same reason every other layer in this
 * project uses integer arithmetic: a frame drawn on an x86 runner, under
 * qemu on aarch64 and on a phone has to be the same frame, or a checksum is
 * not a test. Coordinates go to 28.4 fixed point by multiplying by 16, which
 * is exact for a float; the barycentric weights are the edge functions
 * themselves; and the texture coordinate at a pixel is interpolated in the
 * same fixed point. No floating point after the setup, so nothing a compiler
 * can contract into an fma behind our backs.
 *
 * What it does not do is perspective. A 2D engine draws with w = 1 and the
 * difference is nothing; a 3D one would see textures swim across a triangle.
 * That is a real limitation and it is written down rather than discovered.
 */
#include "w32.h"

#include <stdint.h>
#include <string.h>

/* A pixel is 0xAARRGGBB in a word, which is B,G,R,A in memory -- the same
 * order the Metal view uploads and the same order d3d9 already uses. */
static uint32_t pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* src over dst, with premultiplication done here rather than assumed of the
 * source -- which is what D3D11's usual SRC_ALPHA / INV_SRC_ALPHA pair
 * means, and what a 2D engine sets. Integer, rounding to nearest. */
static uint32_t blend_over(uint32_t src, uint32_t dst) {
    uint32_t sa = src >> 24;
    if (sa == 255) return src;
    if (sa == 0) return dst;
    uint32_t ia = 255 - sa;
    uint32_t r = (((src >> 16) & 0xFF) * sa + ((dst >> 16) & 0xFF) * ia + 127) / 255;
    uint32_t g = (((src >> 8) & 0xFF) * sa + ((dst >> 8) & 0xFF) * ia + 127) / 255;
    uint32_t b = ((src & 0xFF) * sa + (dst & 0xFF) * ia + 127) / 255;
    uint32_t a = (sa * 255 + (dst >> 24) * ia + 127) / 255;
    return pack(a > 255 ? 255 : a, r, g, b);
}
static uint32_t blend_add(uint32_t src, uint32_t dst) {
    uint32_t sa = src >> 24;
    uint32_t r = ((src >> 16) & 0xFF) * sa / 255 + ((dst >> 16) & 0xFF);
    uint32_t g = ((src >> 8) & 0xFF) * sa / 255 + ((dst >> 8) & 0xFF);
    uint32_t b = (src & 0xFF) * sa / 255 + (dst & 0xFF);
    return pack(dst >> 24, r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

/* Sampling. Point filtering, and the address mode applied per axis. Bilinear
 * would be four fetches and a weighted sum per pixel and is not the thing
 * standing between here and a game on screen -- point sampling is what a
 * pixel-art 2D game wants anyway, and it is what the frame checksum can be
 * asserted against. */
static uint32_t sample(const d3d11_texture *t, int32_t u16, int32_t v16, int wrap) {
    if (!t || !t->pixels || t->w <= 0 || t->h <= 0) return 0xFFFFFFFFu;
    /* u16/v16 are 16.16 texture coordinates. Scale into texels, floor, then
     * address. */
    int64_t tx = ((int64_t)u16 * t->w) >> 16;
    int64_t ty = ((int64_t)v16 * t->h) >> 16;
    if (wrap) {
        tx %= t->w; if (tx < 0) tx += t->w;
        ty %= t->h; if (ty < 0) ty += t->h;
    } else {
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
        if (tx >= t->w) tx = t->w - 1;
        if (ty >= t->h) ty = t->h - 1;
    }
    return t->pixels[(size_t)ty * (size_t)t->pitch_px + (size_t)tx];
}

static uint32_t modulate(uint32_t tex, uint32_t col) {
    uint32_t a = ((tex >> 24) * (col >> 24) + 127) / 255;
    uint32_t r = (((tex >> 16) & 0xFF) * ((col >> 16) & 0xFF) + 127) / 255;
    uint32_t g = (((tex >> 8) & 0xFF) * ((col >> 8) & 0xFF) + 127) / 255;
    uint32_t b = ((tex & 0xFF) * (col & 0xFF) + 127) / 255;
    return pack(a, r, g, b);
}

static int64_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
    return (int64_t)(bx - ax) * (py - ay) - (int64_t)(by - ay) * (px - ax);
}
static int32_t fx(float v) { return (int32_t)(v * 16.0f); }   /* exact: 16 is a power of two */

void w32_d3d11_triangle(const d3d11_target *t, const d3d11_vertex *v0,
                        const d3d11_vertex *v1, const d3d11_vertex *v2,
                        const d3d11_texture *tex, int wrap, int blend_mode) {
    if (!t || !t->pixels) return;
    int32_t x0 = fx(v0->x), y0 = fx(v0->y);
    int32_t x1 = fx(v1->x), y1 = fx(v1->y);
    int32_t x2 = fx(v2->x), y2 = fx(v2->y);

    int64_t area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;                        /* degenerate: no pixels */
    int flip = area < 0;                          /* either winding draws: culling is off */
    if (flip) area = -area;

    int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int px0 = minx >> 4, px1 = (maxx >> 4) + 1;
    int py0 = miny >> 4, py1 = (maxy >> 4) + 1;
    if (px0 < t->clip_x) px0 = t->clip_x;
    if (py0 < t->clip_y) py0 = t->clip_y;
    if (px1 > t->clip_x + t->clip_w) px1 = t->clip_x + t->clip_w;
    if (py1 > t->clip_y + t->clip_h) py1 = t->clip_y + t->clip_h;

    /* Texture coordinates in 16.16, so the interpolation below stays in
     * integers all the way to the fetch. */
    int32_t u[3] = { (int32_t)(v0->u * 65536.0f), (int32_t)(v1->u * 65536.0f), (int32_t)(v2->u * 65536.0f) };
    int32_t vv[3] = { (int32_t)(v0->v * 65536.0f), (int32_t)(v1->v * 65536.0f), (int32_t)(v2->v * 65536.0f) };
    uint32_t c[3] = { v0->color, v1->color, v2->color };

    for (int py = py0; py < py1; py++) {
        uint32_t *row = t->pixels + (size_t)py * (size_t)t->pitch_px;
        int32_t sy = py * 16 + 8;                 /* pixel centre */
        for (int px = px0; px < px1; px++) {
            int32_t sx = px * 16 + 8;
            int64_t w0 = edge(x1, y1, x2, y2, sx, sy);
            int64_t w1 = edge(x2, y2, x0, y0, sx, sy);
            int64_t w2 = edge(x0, y0, x1, y1, sx, sy);
            if (flip) { w0 = -w0; w1 = -w1; w2 = -w2; }
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;

            /* The weights sum to `area`, so a barycentric interpolation is a
             * weighted sum divided by it -- no reciprocal, no float. */
            int32_t uu = (int32_t)(((int64_t)u[0] * w0 + (int64_t)u[1] * w1 + (int64_t)u[2] * w2) / area);
            int32_t vvv = (int32_t)(((int64_t)vv[0] * w0 + (int64_t)vv[1] * w1 + (int64_t)vv[2] * w2) / area);
            uint32_t col = 0;
            for (int ch = 0; ch < 4; ch++) {
                int64_t s = (int64_t)((c[0] >> (ch * 8)) & 0xFF) * w0
                          + (int64_t)((c[1] >> (ch * 8)) & 0xFF) * w1
                          + (int64_t)((c[2] >> (ch * 8)) & 0xFF) * w2;
                col |= (uint32_t)((s / area) & 0xFF) << (ch * 8);
            }

            uint32_t src = tex ? modulate(sample(tex, uu, vvv, wrap), col) : col;
            uint32_t *dst = &row[px];
            switch (blend_mode) {
            case D3D11_BLEND_NONE_: *dst = src | 0xFF000000u; break;
            case D3D11_BLEND_ADD_:  *dst = blend_add(src, *dst); break;
            default:                *dst = blend_over(src, *dst); break;
            }
        }
    }
}

/* Filling the whole target, which is what ClearRenderTargetView does and
 * what every frame starts with. */
void w32_d3d11_clear(const d3d11_target *t, uint32_t argb) {
    if (!t || !t->pixels) return;
    for (int y = t->clip_y; y < t->clip_y + t->clip_h; y++) {
        uint32_t *row = t->pixels + (size_t)y * (size_t)t->pitch_px;
        for (int x = t->clip_x; x < t->clip_x + t->clip_w; x++) row[x] = argb;
    }
}
