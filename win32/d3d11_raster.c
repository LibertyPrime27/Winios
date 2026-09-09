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
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
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

/* The same triangle, with a pixel shader deciding the colour. Coverage and
 * the barycentric weights are the integer arithmetic above; the varyings are
 * interpolated in float, perspective-correctly (over 1/w), with contraction
 * off so the same triangle is the same on every machine.
 *
 * Rows are dealt out in bands of eight: worker `k` of `nworkers` draws the
 * bands whose number is k modulo nworkers, and every worker walks every
 * triangle of the draw in order. That is what makes the threading invisible
 * to the result: a pixel belongs to exactly one worker, which draws the
 * triangles covering it in the order the program issued them, so blending
 * lands the same way it would single-threaded and the checksum of a frame
 * does not depend on how many cores the machine had. */
static void tri_rows(const d3d11_target *t, const d3d11_svertex *v0, const d3d11_svertex *v1, const d3d11_svertex *v2,
                     d3d11_pixel_fn fn, void *ctx, int blend_mode, int worker, int nworkers) {
    if (v0->w <= 0.0f || v1->w <= 0.0f || v2->w <= 0.0f) return;   /* behind the eye: no clipping here */
    int32_t x0 = fx(v0->x), y0 = fx(v0->y), x1 = fx(v1->x), y1 = fx(v1->y), x2 = fx(v2->x), y2 = fx(v2->y);
    int64_t area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;
    int flip = area < 0; if (flip) area = -area;
    int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int px0 = minx >> 4, px1 = (maxx >> 4) + 1, py0 = miny >> 4, py1 = (maxy >> 4) + 1;
    if (px0 < t->clip_x) px0 = t->clip_x;
    if (py0 < t->clip_y) py0 = t->clip_y;
    if (px1 > t->clip_x + t->clip_w) px1 = t->clip_x + t->clip_w;
    if (py1 > t->clip_y + t->clip_h) py1 = t->clip_y + t->clip_h;
    float iw0 = 1.0f / v0->w, iw1 = 1.0f / v1->w, iw2 = 1.0f / v2->w;
    float farea = (float)area;
    for (int py = py0; py < py1; py++) {
        if (nworkers > 1 && ((py >> 3) % nworkers) != worker) { py = (((py >> 3) + 1) << 3) - 1; continue; }
        uint32_t *row = t->pixels + (size_t)py * (size_t)t->pitch_px;
        int32_t sy = py * 16 + 8;
        for (int px = px0; px < px1; px++) {
            int32_t sx = px * 16 + 8;
            int64_t w0 = edge(x1, y1, x2, y2, sx, sy), w1 = edge(x2, y2, x0, y0, sx, sy), w2 = edge(x0, y0, x1, y1, sx, sy);
            if (flip) { w0 = -w0; w1 = -w1; w2 = -w2; }
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float l0 = (float)w0 / farea, l1 = (float)w1 / farea, l2 = (float)w2 / farea;
            float p0 = l0 * iw0, p1 = l1 * iw1, p2 = l2 * iw2, psum = p0 + p1 + p2;
            if (psum > 0.0f) { p0 /= psum; p1 /= psum; p2 /= psum; }
            float var[D3D11_MAX_VARY][4];
            for (int k = 0; k < D3D11_MAX_VARY; k++) for (int c = 0; c < 4; c++)
                var[k][c] = p0 * v0->var[k][c] + p1 * v1->var[k][c] + p2 * v2->var[k][c];
            float z = l0 * v0->z + l1 * v1->z + l2 * v2->z;
            int discard = 0;
            uint32_t src = fn(ctx, var, (float)px + 0.5f, (float)py + 0.5f, z, &discard);
            if (discard) continue;
            uint32_t *dst = &row[px];
            switch (blend_mode) {
            case D3D11_BLEND_NONE_: *dst = src | 0xFF000000u; break;
            case D3D11_BLEND_ADD_:  *dst = blend_add(src, *dst); break;
            default:                *dst = blend_over(src, *dst); break;
            }
        }
    }
}
void w32_d3d11_triangle_shaded(const d3d11_target *t, const d3d11_svertex *v0, const d3d11_svertex *v1, const d3d11_svertex *v2,
                               d3d11_pixel_fn fn, void *ctx, int blend_mode) {
    if (!t || !t->pixels || !fn) return;
    tri_rows(t, v0, v1, v2, fn, ctx, blend_mode, 0, 1);
}

/* --- the worker pool -------------------------------------------------------
 *
 * A shaded pixel is an interpreted shader, and a frame of a 2D engine is a
 * few hundred thousand of them: on one core of a phone that is the frame
 * rate. The cores are there, so a draw's triangles are rasterized by all of
 * them, each on its own bands of rows (see tri_rows). The threads are made
 * on first use and live for the process; the calling thread is worker 0 and
 * does its share rather than waiting.
 *
 * What the workers touch is the target's pixels (their own rows), the
 * vertices and the pixel function's context (read-only), and the shader
 * interpreters' scratch state, which is thread-local. Nothing here takes the
 * guest lock or calls into the guest.
 *
 * WINRUN_THREADS=n sets the count; 1 keeps everything on the calling thread,
 * which is also what a single-core machine gets. */
typedef struct { const d3d11_target *t; const d3d11_svertex *v; int ntri; d3d11_pixel_fn fn; void *ctx; int blend; } raster_job;
static struct {
    int n;                               /* workers including the caller; 0 until started */
    pthread_mutex_t mu; pthread_cond_t go, done;
    raster_job job; unsigned gen; int remaining;
} g_pool = { 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, { 0, 0, 0, 0, 0, 0 }, 0, 0 };

static void run_rows(const raster_job *j, int worker, int nworkers) {
    for (int i = 0; i < j->ntri; i++) tri_rows(j->t, &j->v[3 * i], &j->v[3 * i + 1], &j->v[3 * i + 2], j->fn, j->ctx, j->blend, worker, nworkers);
}
static void *worker_main(void *arg) {
    int me = (int)(intptr_t)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_pool.mu);
        while (g_pool.gen == seen) pthread_cond_wait(&g_pool.go, &g_pool.mu);
        seen = g_pool.gen;
        raster_job j = g_pool.job;
        int n = g_pool.n;
        pthread_mutex_unlock(&g_pool.mu);
        run_rows(&j, me, n);
        pthread_mutex_lock(&g_pool.mu);
        if (--g_pool.remaining == 0) pthread_cond_signal(&g_pool.done);
        pthread_mutex_unlock(&g_pool.mu);
    }
    return 0;
}
static void pool_start(void) {
    if (g_pool.n) return;
    int n = 0;
    const char *env = getenv("WINRUN_THREADS");
    if (env) n = atoi(env);
    if (n <= 0) { long c = sysconf(_SC_NPROCESSORS_ONLN); n = c > 0 ? (int)c : 1; }
    if (n > 8) n = 8;
    g_pool.n = 1;
    for (int i = 1; i < n; i++) {
        pthread_t th;
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (pthread_create(&th, &at, worker_main, (void *)(intptr_t)i) != 0) { pthread_attr_destroy(&at); break; }
        pthread_attr_destroy(&at);
        pthread_detach(th);
        g_pool.n++;
    }
}
int w32_raster_threads(void) { pool_start(); return g_pool.n; }

void w32_d3d11_triangles_shaded(const d3d11_target *t, const d3d11_svertex *v, int ntri, d3d11_pixel_fn fn, void *ctx, int blend_mode) {
    if (!t || !t->pixels || !fn || ntri <= 0) return;
    pool_start();
    /* A small draw is not worth waking the pool for: the hand-off costs
     * about as much as a few hundred pixels. */
    float area = 0.0f;
    for (int i = 0; i < ntri && area < 4096.0f; i++) {
        const d3d11_svertex *a = &v[3 * i], *b = &v[3 * i + 1], *c = &v[3 * i + 2];
        float minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
        float maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
        float miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
        float maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
        area += (maxx - minx) * (maxy - miny) * 0.5f;
    }
    raster_job j = { t, v, ntri, fn, ctx, blend_mode };
    if (g_pool.n == 1 || area < 4096.0f) { run_rows(&j, 0, 1); return; }
    pthread_mutex_lock(&g_pool.mu);
    g_pool.job = j;
    g_pool.remaining = g_pool.n - 1;
    g_pool.gen++;
    pthread_cond_broadcast(&g_pool.go);
    pthread_mutex_unlock(&g_pool.mu);
    run_rows(&j, 0, g_pool.n);
    pthread_mutex_lock(&g_pool.mu);
    while (g_pool.remaining > 0) pthread_cond_wait(&g_pool.done, &g_pool.mu);
    pthread_mutex_unlock(&g_pool.mu);
}
