/* A reference rasterizer for d3d9's draw calls.
 *
 * Not the renderer -- the renderer is Metal, on the device. This is what makes
 * a draw call checkable without a GPU: the same guest program that draws a
 * triangle on the iPad draws it here on a Linux runner and under qemu, and the
 * checksum of the resulting frame has to match. Every layer of this project has
 * been verified that way (silicon vectors, difftest, the win32 suite), and a
 * GPU-only draw path would have been the first one with no coverage at all.
 *
 * It is also the honest fallback: a device where the GPU path is unavailable
 * still draws, slowly.
 *
 * Determinism is the whole point, so the arithmetic is chosen for it. Vertex
 * positions are converted to 28.4 fixed point by multiplying by 16 -- exact for
 * a float, being a power of two -- and everything after that is integer. No
 * floating-point rounding, so no difference between an x86 runner, qemu on
 * aarch64, and an M3, and nothing a compiler can contract into an fma behind
 * our backs.
 */
#include "w32.h"

#include <stdint.h>
#include <string.h>

/* Edge function in fixed point: positive on one side of ab, negative on the
 * other, zero on the line. Its magnitude is twice the triangle's area, which
 * is what makes it double as a barycentric weight. */
static int64_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
    return (int64_t)(bx - ax) * (py - ay) - (int64_t)(by - ay) * (px - ax);
}

static int32_t fx(float v) { return (int32_t)(v * 16.0f); }   /* exact: 16 is a power of two */

/* One gouraud triangle, clipped to the target. Colours are D3DCOLOR (ARGB in
 * a word, which is B,G,R,A in memory) and interpolate per channel. */
void w32_raster_triangle(void *target, int width, int height, int pitch,
                         const float *xy0, uint32_t c0,
                         const float *xy1, uint32_t c1,
                         const float *xy2, uint32_t c2) {
    int32_t x0 = fx(xy0[0]), y0 = fx(xy0[1]);
    int32_t x1 = fx(xy1[0]), y1 = fx(xy1[1]);
    int32_t x2 = fx(xy2[0]), y2 = fx(xy2[1]);

    int64_t area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;                       /* degenerate */
    int flip = area < 0;                         /* accept either winding: D3D's default cull is off here */
    if (flip) area = -area;

    int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int px0 = minx >> 4, px1 = (maxx >> 4) + 1;
    int py0 = miny >> 4, py1 = (maxy >> 4) + 1;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > width) px1 = width;
    if (py1 > height) py1 = height;

    for (int py = py0; py < py1; py++) {
        uint32_t *row = (uint32_t *)((uint8_t *)target + (size_t)py * pitch);
        int32_t sy = py * 16 + 8;                /* pixel centre */
        for (int px = px0; px < px1; px++) {
            int32_t sx = px * 16 + 8;
            int64_t w0 = edge(x1, y1, x2, y2, sx, sy);
            int64_t w1 = edge(x2, y2, x0, y0, sx, sy);
            int64_t w2 = edge(x0, y0, x1, y1, sx, sy);
            if (flip) { w0 = -w0; w1 = -w1; w2 = -w2; }
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            uint32_t out = 0;
            for (int ch = 0; ch < 4; ch++) {
                int64_t v = (int64_t)((c0 >> (ch * 8)) & 0xFF) * w0
                          + (int64_t)((c1 >> (ch * 8)) & 0xFF) * w1
                          + (int64_t)((c2 >> (ch * 8)) & 0xFF) * w2;
                out |= (uint32_t)((v / area) & 0xFF) << (ch * 8);
            }
            row[px] = out;
        }
    }
}
