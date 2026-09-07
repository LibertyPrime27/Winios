/* d3d9.dll -- Direct3D 9 as a DLL the guest can load and call.
 *
 * This is the shape the target games use: Fallout 3 and New Vegas are 32-bit
 * MSVC programs that call `Direct3DCreate9`, ask it for a device, and drive
 * the whole renderer through the device's vtable. So d3d9 is a host DLL like
 * kernel32 -- one exported function -- and everything after that is COM
 * objects built by com.c, whose vtable slots are the same `int3` stubs an
 * import uses.
 *
 * What is here is the frame: create the interface, create a device with a
 * back buffer, clear it, present it, and hand the pixels back through a
 * locked surface. The back buffer is *guest* memory, which is what makes both
 * ends work -- the guest can lock and read it directly, and the host can hand
 * the same bytes to Metal without a copy through a second address space.
 *
 * Drawing is not here yet. DrawPrimitive and the shader entry points are
 * unimplemented slots, which report themselves rather than jumping into
 * nothing; d12mt already compiles D3D9 SM3 shaders to MSL (27/27 on both
 * devices) and joining the two is the next step, not this one.
 *
 * Vtable order is load-bearing: the guest computes `lpVtbl[n]` itself, so a
 * slot in the wrong place is an indirect call to the wrong function. Every
 * table below is written with explicit slot numbers taken from d3d9.h for
 * that reason, and its length is the real interface's length so that even the
 * slots we do not implement land somewhere that says so.
 */
#include "w32.h"

#include <stdio.h>
#include <string.h>

enum { S_OK_ = 0, E_FAIL_ = (int)0x80004005, D3DERR_INVALIDCALL = (int)0x8876086C,
       D3DERR_DEVICELOST = (int)0x88760868 };
enum { D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2, D3DCLEAR_STENCIL = 4 };
enum { FMT_X8R8G8B8 = 22, FMT_A8R8G8B8 = 21 };
/* the flexible vertex format bits we understand, and the primitive types */
enum { FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_DIFFUSE = 0x040, FVF_TEX1 = 0x100,
       FVF_POSITION_MASK = 0x400E };
enum { PT_POINTLIST = 1, PT_LINELIST, PT_LINESTRIP, PT_TRIANGLELIST, PT_TRIANGLESTRIP, PT_TRIANGLEFAN };

/* where a presented frame goes */
static w32_present_fn g_present;
static void *g_present_ctx;
void w32_set_present(w32_present_fn fn, void *ctx) { g_present = fn; g_present_ctx = ctx; }
/* So a second consumer can chain rather than displace the first -- winrun's
 * input script wants to see every frame go by without taking the frame away
 * from whoever is drawing it. */
w32_present_fn w32_get_present(void **ctx) { if (ctx) *ctx = g_present_ctx; return g_present; }

/* How the host asks a guest that is drawing frames to stop: Present starts
 * returning D3DERR_DEVICELOST. A game already has to handle that -- it is
 * what a real driver returns when the display mode changes or the machine
 * sleeps -- so a loop that checks its Present result exits on its own,
 * without the host reaching into a running guest. */
static volatile int g_lost;
void w32_d3d9_device_lost(int on) { g_lost = on ? 1 : 0; }

/* object fields (64-bit slots after the header) */
enum { D3D_NDEV = 0 };                                        /* IDirect3D9 */
enum { DEV_PARENT = 0, DEV_FB, DEV_W, DEV_H, DEV_PITCH, DEV_BBSURF, DEV_FRAMES, DEV_HWND,
       DEV_FVF, DEV_STREAM, DEV_STREAM_OFF, DEV_STREAM_STRIDE, DEV_NFIELDS };
enum { SURF_DEV = 0, SURF_BITS, SURF_W, SURF_H, SURF_PITCH };
enum { VB_DEV = 0, VB_BITS, VB_BYTES, VB_FVF, VB_NFIELDS };
enum { TAG_D3D9 = 1, TAG_DEVICE, TAG_SURFACE, TAG_VERTEXBUFFER };

static w32_com_class cls_d3d9, cls_device, cls_surface, cls_vbuf;
void w32_d3d9_reset(void) {
    cls_d3d9.vtable = cls_device.vtable = cls_surface.vtable = cls_vbuf.vtable = 0;
    g_lost = 0;
}

/* The display we claim to be. A game picks a back-buffer size from this when
 * it asks for a windowed device without saying how big. */
/* The mode we report. Read from user32.c rather than declared again here:
 * a game asks both, and two constants that must match are one constant. */
#define SCREEN_W d3d_screen_w()
#define SCREEN_H d3d_screen_h()
static int d3d_screen_w(void) { int v = 1280; w32_screen_size(&v, 0); return v; }
static int d3d_screen_h(void) { int v = 720;  w32_screen_size(0, &v); return v; }

/* ------------------------------------------------------ IDirect3DSurface9 */

static uint64_t surface_new(w32 *w, uint64_t dev, uint64_t bits, uint32_t width, uint32_t height, uint32_t pitch) {
    uint64_t s = w32_com_new(w, &cls_surface, 5);
    if (!s) return 0;
    w32_com_set(w, s, SURF_DEV, dev);
    w32_com_set(w, s, SURF_BITS, bits);
    w32_com_set(w, s, SURF_W, width);
    w32_com_set(w, s, SURF_H, height);
    w32_com_set(w, s, SURF_PITCH, pitch);
    return s;
}
static void s_GetDevice(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, w32_ptrsize(w), w32_com_get(w, ARG(0), SURF_DEV));
    RET(S_OK_);
}
static void s_GetDesc(w32 *w) {
    uint64_t self = ARG(0), d = ARG(1);
    if (!d) { RET(D3DERR_INVALIDCALL); return; }
    memset(W32P(w, d), 0, 32);
    w32_write(w, d +  0, 4, FMT_X8R8G8B8);           /* Format */
    w32_write(w, d +  4, 4, 1);                      /* Type = D3DRTYPE_SURFACE */
    w32_write(w, d + 12, 4, 0);                      /* Pool = D3DPOOL_DEFAULT */
    w32_write(w, d + 24, 4, w32_com_get(w, self, SURF_W));
    w32_write(w, d + 28, 4, w32_com_get(w, self, SURF_H));
    RET(S_OK_);
}
/* D3DLOCKED_RECT is { INT Pitch; void *pBits; } -- the pointer is 4-aligned on
 * x86 and 8-aligned on x64, so the second field moves. */
static void s_LockRect(w32 *w) {
    uint64_t self = ARG(0), lr = ARG(1);
    if (!lr) { RET(D3DERR_INVALIDCALL); return; }
    int psz = (int)w32_ptrsize(w);
    w32_write(w, lr, 4, w32_com_get(w, self, SURF_PITCH));
    w32_write(w, lr + psz, psz, w32_com_get(w, self, SURF_BITS));
    RET(S_OK_);
}
static void s_UnlockRect(w32 *w) { RET(S_OK_); }
static void s_GetType(w32 *w) { RET(1); }                    /* D3DRTYPE_SURFACE */

/* IDirect3DSurface9: IUnknown, then IDirect3DResource9, then its own. */
static const w32_api surface_methods[17] = {
    [0]  = { "QueryInterface", 3, 0, w32_com_QueryInterface, 0 },
    [1]  = { "AddRef",         1, 0, w32_com_AddRef,         0 },
    [2]  = { "Release",        1, 0, w32_com_Release,        0 },
    [3]  = { "GetDevice",      2, 0, s_GetDevice,            0 },
    [10] = { "GetType",        1, 0, s_GetType,              0 },
    [12] = { "GetDesc",        2, 0, s_GetDesc,              0 },
    [13] = { "LockRect",       4, 0, s_LockRect,             0 },
    [14] = { "UnlockRect",     1, 0, s_UnlockRect,           0 },
};

/* ------------------------------------------------- IDirect3DVertexBuffer9 */

/* The vertices live in guest memory: the guest locks the buffer and writes
 * them itself, and the rasterizer reads the same bytes. Nothing is copied,
 * and a Metal backend later uploads from exactly this pointer. */
static void vb_Lock(w32 *w) {
    uint64_t self = ARG(0), offset = ARG(1), size = ARG(2), out = ARG(3);
    uint64_t bytes = w32_com_get(w, self, VB_BYTES), bits = w32_com_get(w, self, VB_BITS);
    if (!out || offset > bytes || (size && offset + size > bytes)) { RET(D3DERR_INVALIDCALL); return; }
    w32_write(w, out, w32_ptrsize(w), bits + offset);
    RET(S_OK_);
}
static void vb_Unlock(w32 *w) { RET(S_OK_); }
static void vb_GetType(w32 *w) { RET(1); }                    /* D3DRTYPE_VERTEXBUFFER is 1 in D3DRESOURCETYPE */
static void vb_GetDevice(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, w32_ptrsize(w), w32_com_get(w, ARG(0), VB_DEV));
    RET(S_OK_);
}
/* D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF */
static void vb_GetDesc(w32 *w) {
    uint64_t self = ARG(0), d = ARG(1);
    if (!d) { RET(D3DERR_INVALIDCALL); return; }
    memset(W32P(w, d), 0, 24);
    w32_write(w, d + 4, 4, 1);                                /* Type */
    w32_write(w, d + 16, 4, w32_com_get(w, self, VB_BYTES));
    w32_write(w, d + 20, 4, w32_com_get(w, self, VB_FVF));
    RET(S_OK_);
}
static const w32_api vbuf_methods[14] = {
    [0]  = { "QueryInterface", 3, 0, w32_com_QueryInterface, 0 },
    [1]  = { "AddRef",         1, 0, w32_com_AddRef,         0 },
    [2]  = { "Release",        1, 0, w32_com_Release,        0 },
    [3]  = { "GetDevice",      2, 0, vb_GetDevice,           0 },
    [10] = { "GetType",        1, 0, vb_GetType,             0 },
    [11] = { "Lock",           5, 0, vb_Lock,                0 },
    [12] = { "Unlock",         1, 0, vb_Unlock,              0 },
    [13] = { "GetDesc",        2, 0, vb_GetDesc,             0 },
};

/* ------------------------------------------------------- IDirect3DDevice9 */

static void d_GetDirect3D(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, w32_ptrsize(w), w32_com_get(w, ARG(0), DEV_PARENT));
    RET(S_OK_);
}
static void d_TestCooperativeLevel(w32 *w) { RET(g_lost ? D3DERR_DEVICELOST : S_OK_); }
static void d_GetAvailableTextureMem(w32 *w) { RET(256u << 20); }
static void d_GetNumberOfSwapChains(w32 *w) { RET(1); }
static void d_BeginScene(w32 *w) { RET(S_OK_); }
static void d_EndScene(w32 *w) { RET(S_OK_); }
/* Accepted and ignored. There is nothing to draw with yet, so remembering
 * render state would be pretending; the argument count in each table entry is
 * what matters, because an x86 stdcall callee pops its own arguments. */
static void d_ok(w32 *w) { RET(S_OK_); }

/* Clear(Count, pRects, Flags, Color, Z, Stencil). Only whole-target colour
 * clears are honoured; a rect list is rare in a game's frame and would be
 * dishonest to accept silently, so it is clamped to the whole surface and
 * said so once. */
static void d_Clear(w32 *w) {
    uint64_t self = ARG(0);
    uint32_t count = (uint32_t)ARG(1), flags = (uint32_t)ARG(3), color = (uint32_t)ARG(4);
    if (!(flags & D3DCLEAR_TARGET)) { RET(S_OK_); return; }        /* depth/stencil only: nothing to draw into yet */
    uint64_t fb = w32_com_get(w, self, DEV_FB);
    uint32_t h = (uint32_t)w32_com_get(w, self, DEV_H), pitch = (uint32_t)w32_com_get(w, self, DEV_PITCH);
    uint32_t width = (uint32_t)w32_com_get(w, self, DEV_W);
    static int warned;
    if (count && !warned) { warned = 1; fprintf(stderr, "winrun: d3d9 Clear with %u rects: clearing the whole target\n", count); }
    uint8_t *p = W32P(w, fb);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(p + (size_t)y * pitch);
        for (uint32_t x = 0; x < width; x++) row[x] = color;
    }
    RET(S_OK_);
}

static void d_Present(w32 *w) {
    uint64_t self = ARG(0);
    if (g_lost) { RET(D3DERR_DEVICELOST); return; }
    uint64_t fb = w32_com_get(w, self, DEV_FB);
    int width = (int)w32_com_get(w, self, DEV_W), h = (int)w32_com_get(w, self, DEV_H);
    int pitch = (int)w32_com_get(w, self, DEV_PITCH);
    w32_com_set(w, self, DEV_FRAMES, w32_com_get(w, self, DEV_FRAMES) + 1);
    if (g_present) g_present(g_present_ctx, W32P(w, fb), width, h, pitch);
    RET(S_OK_);
}

/* --- drawing ---
 *
 * One vertex format for now: D3DFVF_XYZRHW | D3DFVF_DIFFUSE, which is a
 * position already in screen space and a colour. That is deliberately the
 * format with no transform pipeline behind it -- it isolates the parts that
 * have never run before (vertex fetch, primitive assembly, rasterization,
 * the render target) from the parts that have not been written yet (the
 * world/view/projection matrices, lighting, texture stages). Anything else
 * is refused rather than drawn wrong.
 */
static int vertex_stride_for(uint32_t fvf) {
    /* the position bits are their own field (D3DFVF_POSITION_MASK); masking
     * with 0xFF instead would have swallowed D3DFVF_DIFFUSE and never matched */
    if ((fvf & FVF_POSITION_MASK) == FVF_XYZRHW && (fvf & FVF_DIFFUSE)) return 20;
    return 0;
}

/* Read vertex i out of guest memory. Returns 0 if it is not somewhere we can
 * read, which a guest can arrange by handing us a bad pointer. */
static int fetch_vertex(w32 *w, uint64_t base, int stride, int i, float *xy, uint32_t *color) {
    uint64_t at = base + (uint64_t)stride * (unsigned)i;
    const void *p = W32P(w, at);
    if (!p) return 0;
    uint32_t bits[5];
    memcpy(bits, p, 20);
    memcpy(&xy[0], &bits[0], 4);
    memcpy(&xy[1], &bits[1], 4);
    *color = bits[4];
    return 1;
}

static void draw_from(w32 *w, uint64_t self, uint64_t base, int stride, uint32_t fvf,
                      int prim_type, int prim_count) {
    if (!vertex_stride_for(fvf)) {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "winrun: d3d9 draw with FVF %#x: only XYZRHW|DIFFUSE is drawn\n", (unsigned)fvf); }
        return;
    }
    if (stride <= 0) stride = vertex_stride_for(fvf);
    void *target = W32P(w, w32_com_get(w, self, DEV_FB));
    int width = (int)w32_com_get(w, self, DEV_W), h = (int)w32_com_get(w, self, DEV_H);
    int pitch = (int)w32_com_get(w, self, DEV_PITCH);
    if (!target) return;

    for (int i = 0; i < prim_count; i++) {
        int a, b, c;
        switch (prim_type) {
        case PT_TRIANGLELIST:  a = 3 * i;     b = 3 * i + 1; c = 3 * i + 2; break;
        /* a strip alternates winding; the rasterizer accepts either, so the
         * vertices go in order and the fill is the same shape either way */
        case PT_TRIANGLESTRIP: a = i;         b = i + 1;     c = i + 2;     break;
        case PT_TRIANGLEFAN:   a = 0;         b = i + 1;     c = i + 2;     break;
        default: {
            static int warned;
            if (!warned) { warned = 1; fprintf(stderr, "winrun: d3d9 primitive type %d is not drawn yet\n", prim_type); }
            return;
        }
        }
        float p0[2], p1[2], p2[2]; uint32_t c0, c1, c2;
        if (!fetch_vertex(w, base, stride, a, p0, &c0)) return;
        if (!fetch_vertex(w, base, stride, b, p1, &c1)) return;
        if (!fetch_vertex(w, base, stride, c, p2, &c2)) return;
        w32_raster_triangle(target, width, h, pitch, p0, c0, p1, c1, p2, c2);
    }
}

static void d_SetFVF(w32 *w) { w32_com_set(w, ARG(0), DEV_FVF, ARG(1)); RET(S_OK_); }
static void d_GetFVF(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, 4, w32_com_get(w, ARG(0), DEV_FVF));
    RET(S_OK_);
}
/* SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride) */
static void d_SetStreamSource(w32 *w) {
    uint64_t self = ARG(0), vb = ARG(2);
    if (ARG(1) != 0) { RET(D3DERR_INVALIDCALL); return; }      /* one stream is enough for now */
    w32_com_set(w, self, DEV_STREAM, vb ? w32_com_get(w, vb, VB_BITS) : 0);
    w32_com_set(w, self, DEV_STREAM_OFF, ARG(3));
    w32_com_set(w, self, DEV_STREAM_STRIDE, ARG(4));
    RET(S_OK_);
}
/* DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount) */
static void d_DrawPrimitive(w32 *w) {
    uint64_t self = ARG(0);
    uint64_t base = w32_com_get(w, self, DEV_STREAM);
    int stride = (int)w32_com_get(w, self, DEV_STREAM_STRIDE);
    if (!base) { RET(D3DERR_INVALIDCALL); return; }
    base += w32_com_get(w, self, DEV_STREAM_OFF) + (uint64_t)stride * ARG(2);
    draw_from(w, self, base, stride, (uint32_t)w32_com_get(w, self, DEV_FVF), (int)ARG(1), (int)ARG(3));
    RET(S_OK_);
}
/* DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, Stride) */
static void d_DrawPrimitiveUP(w32 *w) {
    uint64_t self = ARG(0);
    if (!ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    draw_from(w, self, ARG(3), (int)ARG(4), (uint32_t)w32_com_get(w, self, DEV_FVF),
              (int)ARG(1), (int)ARG(2));
    RET(S_OK_);
}

/* CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle) */
static void d_CreateVertexBuffer(w32 *w) {
    uint64_t self = ARG(0), length = ARG(1), fvf = ARG(3), out = ARG(5);
    if (!out || !length) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t bits = w32_alloc(w, length, 0);
    if (!bits) { RET(E_FAIL_); return; }
    uint64_t vb = w32_com_new(w, &cls_vbuf, VB_NFIELDS);
    if (!vb) { RET(E_FAIL_); return; }
    w32_com_set(w, vb, VB_DEV, self);
    w32_com_set(w, vb, VB_BITS, bits);
    w32_com_set(w, vb, VB_BYTES, length);
    w32_com_set(w, vb, VB_FVF, fvf);
    w32_write(w, out, w32_ptrsize(w), vb);
    if (w->verbose) fprintf(stderr, "winrun: d3d9 vertex buffer %llu bytes at %#llx\n",
                            (unsigned long long)length, (unsigned long long)bits);
    RET(S_OK_);
}

static void d_GetBackBuffer(w32 *w) {
    uint64_t self = ARG(0), out = ARG(4);
    if (!out) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t s = w32_com_get(w, self, DEV_BBSURF);
    if (!s) {
        s = surface_new(w, self, w32_com_get(w, self, DEV_FB), (uint32_t)w32_com_get(w, self, DEV_W),
                        (uint32_t)w32_com_get(w, self, DEV_H), (uint32_t)w32_com_get(w, self, DEV_PITCH));
        w32_com_set(w, self, DEV_BBSURF, s);
    } else {
        w32_write(w, s + w32_ptrsize(w), 4, w32_read(w, s + w32_ptrsize(w), 4) + 1);   /* the caller owns a reference */
    }
    w32_write(w, out, w32_ptrsize(w), s);
    RET(s ? S_OK_ : E_FAIL_);
}

static void d_GetDisplayMode(w32 *w) {
    uint64_t m = ARG(2);
    if (!m) { RET(D3DERR_INVALIDCALL); return; }
    w32_write(w, m + 0, 4, SCREEN_W); w32_write(w, m + 4, 4, SCREEN_H);
    w32_write(w, m + 8, 4, 60); w32_write(w, m + 12, 4, FMT_X8R8G8B8);
    RET(S_OK_);
}
/* D3DCAPS9 is 304 bytes of flags on both bitnesses. Zeroing it and naming the
 * device is honest: a guest that branches on a capability bit takes the
 * conservative path, which is the one most likely to work. */
static void d_GetDeviceCaps(w32 *w) {
    uint64_t c = ARG(1);
    if (!c) { RET(D3DERR_INVALIDCALL); return; }
    memset(W32P(w, c), 0, 304);
    w32_write(w, c + 0, 4, 1);                       /* DeviceType = D3DDEVTYPE_HAL */
    RET(S_OK_);
}

/* Slot numbers are from d3d9.h and nothing else -- the guest indexes this
 * array itself, so an entry in the wrong place is an indirect call to the
 * wrong function. d3dtest.exe calls through the real header's macros, which
 * is what keeps them honest: the first version of this table was missing
 * CreateDepthStencilSurface at 29, and everything after it was one slot out. */
static const w32_api device_methods[119] = {
    [0]   = { "QueryInterface",         3, 0, w32_com_QueryInterface, 0 },
    [1]   = { "AddRef",                 1, 0, w32_com_AddRef,         0 },
    [2]   = { "Release",                1, 0, w32_com_Release,        0 },
    [3]   = { "TestCooperativeLevel",   1, 0, d_TestCooperativeLevel, 0 },
    [4]   = { "GetAvailableTextureMem", 1, 0, d_GetAvailableTextureMem, 0 },
    [5]   = { "EvictManagedResources",  1, 0, d_ok,                   0 },
    [6]   = { "GetDirect3D",            2, 0, d_GetDirect3D,          0 },
    [7]   = { "GetDeviceCaps",          2, 0, d_GetDeviceCaps,        0 },
    [8]   = { "GetDisplayMode",         3, 0, d_GetDisplayMode,       0 },
    [12]  = { "ShowCursor",             2, 0, d_ok,                   0 },
    [15]  = { "GetNumberOfSwapChains",  1, 0, d_GetNumberOfSwapChains, 0 },
    [17]  = { "Present",                5, 0, d_Present,              0 },
    [18]  = { "GetBackBuffer",          5, 0, d_GetBackBuffer,        0 },
    [41]  = { "BeginScene",             1, 0, d_BeginScene,           0 },
    [42]  = { "EndScene",               1, 0, d_EndScene,             0 },
    [43]  = { "Clear",                  7, 0, d_Clear,                0 },
    [44]  = { "SetTransform",           3, 0, d_ok,                   0 },
    [47]  = { "SetViewport",            2, 0, d_ok,                   0 },
    [57]  = { "SetRenderState",         3, 0, d_ok,                   0 },
    [65]  = { "SetTexture",             3, 0, d_ok,                   0 },
    [67]  = { "SetTextureStageState",   4, 0, d_ok,                   0 },
    [69]  = { "SetSamplerState",        4, 0, d_ok,                   0 },
    [77]  = { "SetSoftwareVertexProcessing", 2, 0, d_ok,              0 },
    [26]  = { "CreateVertexBuffer",     7, 0, d_CreateVertexBuffer,   0 },
    [81]  = { "DrawPrimitive",          4, 0, d_DrawPrimitive,        0 },
    [83]  = { "DrawPrimitiveUP",        5, 0, d_DrawPrimitiveUP,      0 },
    [89]  = { "SetFVF",                 2, 0, d_SetFVF,               0 },
    [90]  = { "GetFVF",                 2, 0, d_GetFVF,               0 },
    [100] = { "SetStreamSource",        5, 0, d_SetStreamSource,      0 },
};

/* ------------------------------------------------------------ IDirect3D9 */

static void i_GetAdapterCount(w32 *w) { RET(1); }
static void i_GetAdapterDisplayMode(w32 *w) {
    uint64_t m = ARG(2);
    if (!m) { RET(D3DERR_INVALIDCALL); return; }
    w32_write(w, m + 0, 4, SCREEN_W); w32_write(w, m + 4, 4, SCREEN_H);
    w32_write(w, m + 8, 4, 60); w32_write(w, m + 12, 4, FMT_X8R8G8B8);
    RET(S_OK_);
}
static void i_GetAdapterModeCount(w32 *w) { RET(1); }
/* EnumAdapterModes(Adapter, Format, Mode, pMode) -- one mode, index 0. */
static void i_EnumAdapterModes(w32 *w) {
    uint64_t m = ARG(4);
    if (!m || ARG(3) != 0) { RET(D3DERR_INVALIDCALL); return; }
    w32_write(w, m + 0, 4, SCREEN_W); w32_write(w, m + 4, 4, SCREEN_H);
    w32_write(w, m + 8, 4, 60); w32_write(w, m + 12, 4, FMT_X8R8G8B8);
    RET(S_OK_);
}
/* D3DADAPTER_IDENTIFIER9: two 512-byte strings, a 32-byte device name, then
 * ids. Games print this and some check the vendor id, so it says what we are
 * rather than impersonating a card that would imply drivers we do not have. */
static void i_GetAdapterIdentifier(w32 *w) {
    uint64_t p = ARG(3);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(W32P(w, p), 0, 1104);
    const char *drv = "d12mt.dll", *desc = "xcore d3d9 on Metal (d12mt)", *dev = "\\\\.\\DISPLAY1";
    memcpy((char *)W32P(w, p), drv, strlen(drv));
    memcpy((char *)W32P(w, p + 512), desc, strlen(desc));
    memcpy((char *)W32P(w, p + 1024), dev, strlen(dev));
    w32_write(w, p + 1064, 4, 0x106B);               /* VendorId: Apple */
    w32_write(w, p + 1068, 4, 1);                    /* DeviceId */
    RET(S_OK_);
}
static void i_CheckDeviceType(w32 *w) { RET(S_OK_); }
static void i_CheckDeviceFormat(w32 *w) { RET(S_OK_); }
static void i_CheckDeviceMultiSampleType(w32 *w) { RET(S_OK_); }
static void i_CheckDepthStencilMatch(w32 *w) { RET(S_OK_); }
static void i_CheckDeviceFormatConversion(w32 *w) { RET(S_OK_); }
static void i_GetDeviceCaps(w32 *w) {
    uint64_t c = ARG(3);
    if (!c) { RET(D3DERR_INVALIDCALL); return; }
    memset(W32P(w, c), 0, 304);
    w32_write(w, c + 0, 4, 1);
    RET(S_OK_);
}
static void i_RegisterSoftwareDevice(w32 *w) { RET(S_OK_); }
static void i_GetAdapterMonitor(w32 *w) { RET(1); }

/* CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice).
 * BackBufferWidth/Height/Format sit at offsets 0/4/8 of D3DPRESENT_PARAMETERS
 * in both bitnesses (the first pointer in it comes later), so the fields we
 * need are read the same way for a PE32 and a PE32+ guest. Zero means "use
 * the window", and with no windows yet that is the display size. */
static void i_CreateDevice(w32 *w) {
    uint64_t self = ARG(0), pp = ARG(5), out = ARG(6);
    if (!pp || !out) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t bw = (uint32_t)w32_read(w, pp + 0, 4), bh = (uint32_t)w32_read(w, pp + 4, 4);
    if (!bw) bw = SCREEN_W;
    if (!bh) bh = SCREEN_H;
    uint32_t pitch = bw * 4;
    uint64_t fb = w32_alloc(w, (uint64_t)pitch * bh, 0);
    if (!fb) { fprintf(stderr, "winrun: d3d9: no memory for a %ux%u back buffer\n", bw, bh); RET(E_FAIL_); return; }

    uint64_t dev = w32_com_new(w, &cls_device, DEV_NFIELDS);
    if (!dev) { RET(E_FAIL_); return; }
    w32_com_set(w, dev, DEV_PARENT, self);
    w32_com_set(w, dev, DEV_FB, fb);
    w32_com_set(w, dev, DEV_W, bw);
    w32_com_set(w, dev, DEV_H, bh);
    w32_com_set(w, dev, DEV_PITCH, pitch);
    w32_com_set(w, dev, DEV_HWND, ARG(3));       /* hFocusWindow */
    w32_com_set(w, self, D3D_NDEV, w32_com_get(w, self, D3D_NDEV) + 1);
    w32_write(w, out, w32_ptrsize(w), dev);
    if (w->verbose) fprintf(stderr, "winrun: d3d9 device %ux%u, back buffer at %#llx\n", bw, bh, (unsigned long long)fb);
    RET(S_OK_);
}

static const w32_api d3d9_methods[17] = {
    [0]  = { "QueryInterface",               3, 0, w32_com_QueryInterface,        0 },
    [1]  = { "AddRef",                       1, 0, w32_com_AddRef,                0 },
    [2]  = { "Release",                      1, 0, w32_com_Release,               0 },
    [3]  = { "RegisterSoftwareDevice",       2, 0, i_RegisterSoftwareDevice,      0 },
    [4]  = { "GetAdapterCount",              1, 0, i_GetAdapterCount,             0 },
    [5]  = { "GetAdapterIdentifier",         4, 0, i_GetAdapterIdentifier,        0 },
    [6]  = { "GetAdapterModeCount",          3, 0, i_GetAdapterModeCount,         0 },
    [7]  = { "EnumAdapterModes",             5, 0, i_EnumAdapterModes,            0 },
    [8]  = { "GetAdapterDisplayMode",        3, 0, i_GetAdapterDisplayMode,       0 },
    [9]  = { "CheckDeviceType",              6, 0, i_CheckDeviceType,             0 },
    [10] = { "CheckDeviceFormat",            7, 0, i_CheckDeviceFormat,           0 },
    [11] = { "CheckDeviceMultiSampleType",   7, 0, i_CheckDeviceMultiSampleType,  0 },
    [12] = { "CheckDepthStencilMatch",       6, 0, i_CheckDepthStencilMatch,      0 },
    [13] = { "CheckDeviceFormatConversion",  5, 0, i_CheckDeviceFormatConversion, 0 },
    [14] = { "GetDeviceCaps",                4, 0, i_GetDeviceCaps,               0 },
    [15] = { "GetAdapterMonitor",            2, 0, i_GetAdapterMonitor,           0 },
    [16] = { "CreateDevice",                 7, 0, i_CreateDevice,                0 },
};

static w32_com_class cls_d3d9    = { "IDirect3D9",        d3d9_methods,    17,  TAG_D3D9,    0, {0} };
static w32_com_class cls_device  = { "IDirect3DDevice9",  device_methods,  119, TAG_DEVICE,  0, {0} };
static w32_com_class cls_surface = { "IDirect3DSurface9", surface_methods, 17,  TAG_SURFACE, 0, {0} };
static w32_com_class cls_vbuf    = { "IDirect3DVertexBuffer9", vbuf_methods, 14, TAG_VERTEXBUFFER, 0, {0} };

/* --------------------------------------------------------- the export */

static void d3d9_Direct3DCreate9(w32 *w) {
    uint64_t o = w32_com_new(w, &cls_d3d9, 1);
    if (w->verbose) fprintf(stderr, "winrun: Direct3DCreate9(%u) = %#llx\n", (unsigned)ARG(0), (unsigned long long)o);
    RET(o);
}
static void d3d9_D3DPERF_nop(w32 *w) { RET(0); }

#define F(n, a)  { #n, a, 0, d3d9_##n, 0 }
const w32_api w32_d3d9[] = {
    F(Direct3DCreate9, 1),
    { "Direct3DCreate9@4", 1, 0, d3d9_Direct3DCreate9, 0 },      /* if an import library decorated it */
    { "D3DPERF_BeginEvent", 2, 0, d3d9_D3DPERF_nop, 0 },
    { "D3DPERF_EndEvent",   0, 0, d3d9_D3DPERF_nop, 0 },
    { "D3DPERF_SetMarker",  2, 0, d3d9_D3DPERF_nop, 0 },
    { "D3DPERF_GetStatus",  0, 0, d3d9_D3DPERF_nop, 0 },
    { 0, 0, 0, 0, 0 },
};
