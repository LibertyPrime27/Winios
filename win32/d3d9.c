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

enum { D3DERR_NOTAVAILABLE_ = (int)0x8876086A };
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

/* The checksum of a frame, in one place.
 *
 * Every pixel that reaches this hook was computed with integer arithmetic --
 * the rasterizers in raster.c and d3d11_raster.c have no float in them by
 * construction -- so the same guest drawing the same thing has to produce the
 * same 32 bits on an x86 runner, under qemu on aarch64, and on a phone. That
 * is only a useful claim if all three compute the checksum the same way, so
 * winrun's `-frame` and the on-device diagnostics both call this rather than
 * each keeping a CRC32 of their own. (Plain CRC-32, the zlib polynomial.) */
uint32_t w32_frame_crc32(const void *data, size_t n) {
    static uint32_t tab[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* How the host asks a guest that is drawing frames to stop: Present starts
 * returning D3DERR_DEVICELOST. A game already has to handle that -- it is
 * what a real driver returns when the display mode changes or the machine
 * sleeps -- so a loop that checks its Present result exits on its own,
 * without the host reaching into a running guest. */
static volatile int g_lost;
void w32_d3d9_device_lost(int on) { g_lost = on ? 1 : 0; }
/* The same signal, read by d3d11's Present. "The window went away, stop
 * drawing" is one fact about the display, not one per graphics API, so both
 * ask the same flag rather than each keeping its own and one being stale. */
int w32_d3d9_lost(void) { return g_lost; }

/* object fields (64-bit slots after the header) */
enum { D3D_NDEV = 0 };                                        /* IDirect3D9 */
enum { DEV_PARENT = 0, DEV_FB, DEV_W, DEV_H, DEV_PITCH, DEV_BBSURF, DEV_FRAMES, DEV_HWND,
       DEV_FVF, DEV_STREAM, DEV_STREAM_OFF, DEV_STREAM_STRIDE,
       /* The rest of the state a draw call reads. All of it used to be
        * accepted and thrown away, which is why a game could set up a
        * perfectly good textured quad and get an untextured one -- or, more
        * often, nothing at all, because its vertex format was refused. */
       DEV_TEX0,                        /* the texture bound to stage 0 */
       DEV_ADDRU, DEV_ADDRV,            /* wrap or clamp, per axis */
       DEV_BLEND, DEV_SRCBLEND, DEV_DSTBLEND,
       DEV_VP_X, DEV_VP_Y, DEV_VP_W, DEV_VP_H,
       /* World, view and projection, each sixteen floats, stored as the raw
        * bits so the matrix that arrives is the matrix that is used. */
       DEV_MWORLD, DEV_MVIEW = DEV_MWORLD + 16, DEV_MPROJ = DEV_MVIEW + 16,
       DEV_INDEX = DEV_MPROJ + 16, DEV_INDEX_FMT, DEV_BASEVERTEX,
       DEV_DECL,                        /* a vertex declaration, if one is set */
       DEV_VSHADER, DEV_PSHADER,        /* created and bound; never executed */
       DEV_RT,                          /* the render target, or 0 for the back buffer */
       DEV_SC_X, DEV_SC_Y, DEV_SC_W, DEV_SC_H,
       DEV_NFIELDS };
enum { SURF_DEV = 0, SURF_BITS, SURF_W, SURF_H, SURF_PITCH, SURF_TEX, SURF_NFIELDS };
/* A texture is its level-0 surface plus the parent device. Mip levels are not
 * kept: the rasterizer point-samples one level, so storing the smaller ones
 * would be storing something nothing reads. A game that asks for four levels
 * gets four handles onto the same pixels, which is wrong in a way nobody can
 * see and right in the way that matters -- CreateTexture succeeds. */
enum { TEX_DEV = 0, TEX_BITS, TEX_W, TEX_H, TEX_PITCH, TEX_LEVELS, TEX_FMT, TEX_SURF,
       TEX_NFIELDS };
enum { IB_DEV = 0, IB_BITS, IB_BYTES, IB_FMT, IB_NFIELDS };
/* A vertex declaration, reduced to the same layout an FVF produces. */
enum { DECL_DEV = 0, DECL_STRIDE, DECL_POS, DECL_POSN, DECL_DIFFUSE, DECL_TEX0,
       DECL_TEX0N, DECL_NFIELDS };
enum { VB_DEV = 0, VB_BITS, VB_BYTES, VB_FVF, VB_NFIELDS };
enum { TAG_D3D9 = 1, TAG_DEVICE, TAG_SURFACE, TAG_VERTEXBUFFER,
       TAG_TEXTURE, TAG_INDEXBUFFER, TAG_DECL, TAG_SHADER };

static w32_com_class cls_d3d9, cls_device, cls_surface, cls_vbuf, cls_texture, cls_ibuf,
                     cls_decl, cls_shader;
void w32_d3d9_reset(void) {
    cls_d3d9.vtable = cls_device.vtable = cls_surface.vtable = cls_vbuf.vtable = 0;
    cls_texture.vtable = cls_ibuf.vtable = cls_decl.vtable = cls_shader.vtable = 0;
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
    uint64_t s = w32_com_new(w, &cls_surface, SURF_NFIELDS);
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
    void *p = W32PN(w, d, 32);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(p, 0, 32);
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
    void *p = W32PN(w, d, 24);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(p, 0, 24);
    w32_write(w, d + 4, 4, 1);                                /* Type */
    w32_write(w, d + 16, 4, w32_com_get(w, self, VB_BYTES));
    w32_write(w, d + 20, 4, w32_com_get(w, self, VB_FVF));
    RET(S_OK_);
}
/* Accept and do nothing, for the calls whose effect is invisible here.
 * Declared before the texture tables because they use it. */
static void d_ok(w32 *w);

/* ------------------------------------------------------- IDirect3DTexture9
 *
 * The one object whose absence stopped 2D games dead. A sprite is a textured
 * quad; every engine that draws one calls CreateTexture, fills it through
 * LockRect, and binds it with SetTexture. Until now CreateTexture was not in
 * the table at all, so the call landed on the unimplemented-slot stub and the
 * run stopped there -- before the game had drawn a single frame.
 *
 * The pixels live in guest memory, like everything else here, so the guest
 * locks and writes them directly and the rasterizer reads the same bytes.
 */
static void t_GetDevice(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, w32_ptrsize(w), w32_com_get(w, ARG(0), TEX_DEV));
    RET(S_OK_);
}
static void t_GetType(w32 *w) { (void)w; RET(3); }          /* D3DRTYPE_TEXTURE */
static void t_GetLevelCount(w32 *w) { RET(w32_com_get(w, ARG(0), TEX_LEVELS)); }
/* D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, Quality,
 * Width, Height -- eight DWORDs, the same in both bitnesses. */
static void t_GetLevelDesc(w32 *w) {
    uint64_t self = ARG(0), d = ARG(2);
    void *p = W32PN(w, d, 32);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(p, 0, 32);
    w32_write(w, d + 0,  4, w32_com_get(w, self, TEX_FMT));
    w32_write(w, d + 4,  4, 3);                              /* D3DRTYPE_TEXTURE */
    w32_write(w, d + 12, 4, 1);                              /* D3DPOOL_MANAGED */
    w32_write(w, d + 24, 4, w32_com_get(w, self, TEX_W));
    w32_write(w, d + 28, 4, w32_com_get(w, self, TEX_H));
    RET(S_OK_);
}
/* LockRect(Level, pLockedRect, pRect, Flags). D3DLOCKED_RECT is { INT Pitch;
 * void *pBits; } -- a DWORD then a pointer, so the pointer is at 4 on x86 and
 * at 8 on x64 once it is aligned.
 *
 * A sub-rectangle is honoured, because a texture atlas is filled one sprite
 * at a time and handing back the origin every time would stack every sprite
 * in the top-left corner. */
static void t_LockRect(w32 *w) {
    uint64_t self = ARG(0), out = ARG(2), rect = ARG(3);
    if (!out) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t bits = w32_com_get(w, self, TEX_BITS);
    int pitch = (int)w32_com_get(w, self, TEX_PITCH);
    int x = 0, y = 0;
    if (rect) { x = (int)(int32_t)w32_read(w, rect, 4); y = (int)(int32_t)w32_read(w, rect + 4, 4); }
    int ps = (int)w32_ptrsize(w);
    w32_write(w, out, 4, (uint64_t)(uint32_t)pitch);
    w32_write(w, out + (ps == 8 ? 8 : 4), ps, bits + (uint64_t)y * (unsigned)pitch + (uint64_t)x * 4);
    RET(S_OK_);
}
static void t_UnlockRect(w32 *w) { (void)w; RET(S_OK_); }
static void t_AddDirtyRect(w32 *w) { (void)w; RET(S_OK_); }
/* GetSurfaceLevel hands back a surface onto the same pixels, which is how a
 * program blits into a texture or uses one as a render target's source. */
static void t_GetSurfaceLevel(w32 *w) {
    uint64_t self = ARG(0), out = ARG(2);
    if (!out) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t surf = w32_com_get(w, self, TEX_SURF);
    if (!surf) {
        surf = w32_com_new(w, &cls_surface, SURF_NFIELDS);
        if (!surf) { RET(E_FAIL_); return; }
        w32_com_set(w, surf, SURF_DEV, w32_com_get(w, self, TEX_DEV));
        w32_com_set(w, surf, SURF_BITS, w32_com_get(w, self, TEX_BITS));
        w32_com_set(w, surf, SURF_W, w32_com_get(w, self, TEX_W));
        w32_com_set(w, surf, SURF_H, w32_com_get(w, self, TEX_H));
        w32_com_set(w, surf, SURF_PITCH, w32_com_get(w, self, TEX_PITCH));
        w32_com_set(w, surf, SURF_TEX, self);
        w32_com_set(w, self, TEX_SURF, surf);
    }
    w32_write(w, out, w32_ptrsize(w), surf);
    RET(S_OK_);
}

static const w32_api texture_full[22] = {
    [0]  = { "QueryInterface",  3, 0, w32_com_QueryInterface, 0 },
    [1]  = { "AddRef",          1, 0, w32_com_AddRef,         0 },
    [2]  = { "Release",         1, 0, w32_com_Release,        0 },
    [3]  = { "GetDevice",       2, 0, t_GetDevice,            0 },
    [4]  = { "SetPrivateData",  4, 0, d_ok,                   0 },
    [5]  = { "GetPrivateData",  4, 0, d_ok,                   0 },
    [6]  = { "FreePrivateData", 2, 0, d_ok,                   0 },
    [7]  = { "SetPriority",     2, 0, d_ok,                   0 },
    [8]  = { "GetPriority",     1, 0, d_ok,                   0 },
    [9]  = { "PreLoad",         1, 0, d_ok,                   0 },
    [10] = { "GetType",         1, 0, t_GetType,              0 },
    [11] = { "SetLOD",          2, 0, d_ok,                   0 },
    [12] = { "GetLOD",          1, 0, d_ok,                   0 },
    [13] = { "GetLevelCount",   1, 0, t_GetLevelCount,        0 },
    [14] = { "SetAutoGenFilterType", 2, 0, d_ok,              0 },
    [15] = { "GetAutoGenFilterType", 1, 0, d_ok,              0 },
    [16] = { "GenerateMipSubLevels", 1, 0, d_ok,              0 },
    [17] = { "GetLevelDesc",    3, 0, t_GetLevelDesc,         0 },
    [18] = { "GetSurfaceLevel", 3, 0, t_GetSurfaceLevel,      0 },
    [19] = { "LockRect",        4, 0, t_LockRect,             0 },
    [20] = { "UnlockRect",      2, 0, t_UnlockRect,           0 },
    [21] = { "AddDirtyRect",    2, 0, t_AddDirtyRect,         0 },
};

/* --------------------------------------------------- IDirect3DIndexBuffer9 */
static void ib_GetDevice(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, w32_ptrsize(w), w32_com_get(w, ARG(0), IB_DEV));
    RET(S_OK_);
}
static void ib_GetType(w32 *w) { (void)w; RET(2); }          /* D3DRTYPE_INDEXBUFFER */
static void ib_Lock(w32 *w) {
    uint64_t self = ARG(0), off = (uint32_t)ARG(1), out = ARG(3);
    if (!out) { RET(D3DERR_INVALIDCALL); return; }
    w32_write(w, out, w32_ptrsize(w), w32_com_get(w, self, IB_BITS) + off);
    RET(S_OK_);
}
static void ib_Unlock(w32 *w) { (void)w; RET(S_OK_); }
static void ib_GetDesc(w32 *w) {
    uint64_t self = ARG(0), d = ARG(1);
    void *p = W32PN(w, d, 24);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(p, 0, 24);
    w32_write(w, d + 0, 4, w32_com_get(w, self, IB_FMT));
    w32_write(w, d + 4, 4, 2);
    w32_write(w, d + 16, 4, w32_com_get(w, self, IB_BYTES));
    RET(S_OK_);
}
static const w32_api ibuf_methods[14] = {
    [0]  = { "QueryInterface", 3, 0, w32_com_QueryInterface, 0 },
    [1]  = { "AddRef",         1, 0, w32_com_AddRef,         0 },
    [2]  = { "Release",        1, 0, w32_com_Release,        0 },
    [3]  = { "GetDevice",      2, 0, ib_GetDevice,           0 },
    [10] = { "GetType",        1, 0, ib_GetType,             0 },
    [11] = { "Lock",           5, 0, ib_Lock,                0 },
    [12] = { "Unlock",         1, 0, ib_Unlock,              0 },
    [13] = { "GetDesc",        2, 0, ib_GetDesc,             0 },
};

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
    w32_note_activity();          /* a frame is not idling */
    if (g_present) g_present(g_present_ctx, W32P(w, fb), width, h, pitch);
    RET(S_OK_);
}

/* --- drawing -------------------------------------------------------------
 *
 * This used to accept exactly one vertex format -- D3DFVF_XYZRHW|DIFFUSE, an
 * untextured position already in screen space -- and refuse everything else
 * with a line on stderr. That was the right first step, because it isolated
 * vertex fetch, primitive assembly and rasterization from the transform
 * pipeline and the texture stages; but it also meant that essentially no real
 * 2D game drew anything, because a sprite is a *textured* quad and every
 * engine that puts one on screen sends D3DFVF_TEX1 with it.
 *
 * So the vertex fetch is now driven by the FVF rather than being one shape,
 * the fixed-function transform is applied when the position is not already
 * transformed, and the pixels go through the same integer rasterizer D3D11
 * draws with -- which already does texturing, colour modulation and alpha
 * blending, and is deterministic by construction. Two APIs, one rasterizer,
 * one set of frame checksums.
 */

/* Where each component sits in a vertex, given the format. Offsets are in
 * bytes from the start of the vertex; -1 means the format does not carry it.
 *
 * The order is fixed by the FVF specification and is not negotiable: position,
 * then blend weights, then normal, then point size, then diffuse, then
 * specular, then the texture coordinate sets. Getting the order wrong reads a
 * colour as a coordinate, which draws a quad somewhere off the screen. */
typedef struct {
    int stride;
    int pos, pos_n;      /* offset, and how many floats (3 for XYZ, 4 for XYZRHW) */
    int diffuse;
    int tex0;
    int tex0_n;
} fvf_layout;

static int fvf_tex_count(uint32_t fvf) { return (int)((fvf >> 8) & 0xF); }

static int fvf_decode(uint32_t fvf, fvf_layout *L) {
    memset(L, 0, sizeof *L);
    L->pos = L->diffuse = L->tex0 = -1;
    int at = 0;
    switch (fvf & FVF_POSITION_MASK) {
    case FVF_XYZ:    L->pos = 0; L->pos_n = 3; at = 12; break;
    case FVF_XYZRHW: L->pos = 0; L->pos_n = 4; at = 16; break;
    case 0x006:      L->pos = 0; L->pos_n = 3; at = 12 + 4; break;   /* XYZB1 */
    case 0x008:      L->pos = 0; L->pos_n = 3; at = 12 + 8; break;   /* XYZB2 */
    case 0x00A:      L->pos = 0; L->pos_n = 3; at = 12 + 12; break;  /* XYZB3 */
    case 0x00C:      L->pos = 0; L->pos_n = 3; at = 12 + 16; break;  /* XYZB4 */
    case 0x00E:      L->pos = 0; L->pos_n = 3; at = 12 + 20; break;  /* XYZB5 */
    default: return 0;                                   /* XYZW and friends */
    }
    if (fvf & 0x010) at += 12;                           /* D3DFVF_NORMAL */
    if (fvf & 0x020) at += 4;                            /* D3DFVF_PSIZE */
    if (fvf & FVF_DIFFUSE)  { L->diffuse = at; at += 4; }
    if (fvf & 0x080)        { at += 4; }                 /* D3DFVF_SPECULAR */
    int ntex = fvf_tex_count(fvf);
    if (ntex > 0) {
        /* The size of each coordinate set is two floats unless the format
         * says otherwise in the high bits, which almost nothing does. */
        uint32_t sizes = fvf >> 16;
        int n = (int)((sizes & 3) == 0 ? 2 : (sizes & 3) == 1 ? 3 : (sizes & 3) == 2 ? 4 : 1);
        L->tex0 = at; L->tex0_n = n;
        at += n * 4;
        for (int i = 1; i < ntex; i++) {
            int m = (int)(((sizes >> (2 * i)) & 3) == 0 ? 2 : ((sizes >> (2 * i)) & 3) == 1 ? 3
                        : ((sizes >> (2 * i)) & 3) == 2 ? 4 : 1);
            at += m * 4;
        }
    }
    L->stride = at;
    return at > 0;
}

static float rdf(const unsigned char *p) { float f; memcpy(&f, p, 4); return f; }

/* Read the four-by-four at a device field. Row-major, as D3DMATRIX is. */
static void read_matrix(w32 *w, uint64_t self, int field, float *m) {
    for (int i = 0; i < 16; i++) {
        uint32_t bits = (uint32_t)w32_com_get(w, self, field + i);
        memcpy(&m[i], &bits, 4);
    }
}
static void mat_mul(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float v = 0;
        for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = v;
    }
}
static int mat_is_identity(const float *m) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        if (m[r * 4 + c] != (r == c ? 1.0f : 0.0f)) return 0;
    return 1;
}

/* One vertex, fetched and put through whichever part of the pipeline its
 * format asks for. Returns 0 when the guest handed over a pointer we cannot
 * read, which it can and does. */
static int fetch_vertex(w32 *w, uint64_t self, uint64_t base, const fvf_layout *L,
                        int i, const float *mvp, int transformed,
                        int vx, int vy, int vw, int vh, d3d11_vertex *out) {
    uint64_t at = base + (uint64_t)L->stride * (unsigned)i;
    /* The stream pointer, the stride and the index all come from the program,
     * and DrawPrimitiveUP takes the vertices themselves as a raw pointer, so
     * this is the one check on the hot path. It costs a compare against the
     * cached mapping in the common case, which is the same mapping every
     * vertex of every draw lands in. */
    const unsigned char *p = (const unsigned char *)W32PN(w, at, L->stride);
    if (!p) return 0;
    memset(out, 0, sizeof *out);
    out->color = 0xFFFFFFFFu;
    out->w = 1.0f;

    float x = 0, y = 0, z = 0, rhw = 1;
    if (L->pos >= 0) {
        x = rdf(p + L->pos);
        y = rdf(p + L->pos + 4);
        z = rdf(p + L->pos + 8);
        if (L->pos_n == 4) rhw = rdf(p + L->pos + 12);
    }
    if (L->diffuse >= 0) memcpy(&out->color, p + L->diffuse, 4);
    if (L->tex0 >= 0) { out->u = rdf(p + L->tex0); out->v = rdf(p + L->tex0 + 4); }

    if (transformed) {
        /* XYZRHW: the program did the transform itself and these are pixels
         * already, which is what every 2D sprite engine sends. */
        out->x = x; out->y = y; out->z = z; out->w = rhw != 0 ? rhw : 1.0f;
        return 1;
    }
    /* Otherwise: world * view * projection, the perspective divide, and the
     * viewport map. Row vector times row-major matrix, which is D3D's
     * convention -- the other one draws everything transposed. */
    float cx = x * mvp[0] + y * mvp[4] + z * mvp[8]  + mvp[12];
    float cy = x * mvp[1] + y * mvp[5] + z * mvp[9]  + mvp[13];
    float cz = x * mvp[2] + y * mvp[6] + z * mvp[10] + mvp[14];
    float cw = x * mvp[3] + y * mvp[7] + z * mvp[11] + mvp[15];
    if (cw == 0) cw = 1e-6f;
    float ndx = cx / cw, ndy = cy / cw;
    out->x = (float)vx + (ndx * 0.5f + 0.5f) * (float)vw;
    out->y = (float)vy + (0.5f - ndy * 0.5f) * (float)vh;
    out->z = cz / cw;
    out->w = cw;
    (void)self;
    return 1;
}

/* The texture bound to stage 0, if there is one and it has pixels. */
static int bound_texture(w32 *w, uint64_t self, d3d11_texture *t) {
    uint64_t tex = w32_com_get(w, self, DEV_TEX0);
    if (!tex) return 0;
    uint64_t bits = w32_com_get(w, tex, TEX_BITS);
    int tw = (int)w32_com_get(w, tex, TEX_W), th = (int)w32_com_get(w, tex, TEX_H);
    int pitch = (int)w32_com_get(w, tex, TEX_PITCH);
    void *px = bits ? W32P(w, bits) : 0;
    if (!px || tw <= 0 || th <= 0) return 0;
    t->pixels = (const uint32_t *)px;
    t->w = tw; t->h = th; t->pitch_px = pitch / 4;
    return 1;
}

/* Which blend the render state adds up to. The rasterizer offers source-over
 * and additive, which between them cover what a 2D game does: SRCALPHA with
 * INVSRCALPHA is a sprite, SRCALPHA or ONE with ONE is a glow. Anything else
 * is drawn opaque rather than refused, because a wrong blend is a visible
 * picture and a refused draw is a blank screen. */
static int blend_mode_of(w32 *w, uint64_t self) {
    if (!w32_com_get(w, self, DEV_BLEND)) return D3D11_BLEND_NONE_;
    uint64_t dst = w32_com_get(w, self, DEV_DSTBLEND);
    if (dst == 2 /* D3DBLEND_ONE */) return D3D11_BLEND_ADD_;
    return D3D11_BLEND_OVER_;
}

static void draw_indexed(w32 *w, uint64_t self, uint64_t base, uint32_t fvf,
                         int prim_type, int prim_count,
                         const uint16_t *idx16, const uint32_t *idx32,
                         int nindices, int base_vertex) {
    /* A vertex declaration wins over the FVF when one is set, because that is
     * what the program is actually describing its vertices with -- an engine
     * that moved to declarations often leaves a stale FVF behind, and reading
     * that instead fetches the wrong bytes. */
    fvf_layout L;
    uint64_t decl = w32_com_get(w, self, DEV_DECL);
    int have_decl = 0;
    if (decl) {
        memset(&L, 0, sizeof L);
        L.pos     = (int)(int32_t)(uint32_t)w32_com_get(w, decl, DECL_POS);
        L.pos_n   = (int)w32_com_get(w, decl, DECL_POSN);
        L.diffuse = (int)(int32_t)(uint32_t)w32_com_get(w, decl, DECL_DIFFUSE);
        L.tex0    = (int)(int32_t)(uint32_t)w32_com_get(w, decl, DECL_TEX0);
        L.tex0_n  = (int)w32_com_get(w, decl, DECL_TEX0N);
        L.stride  = (int)w32_com_get(w, decl, DECL_STRIDE);
        have_decl = L.pos >= 0 && L.stride > 0;
    }
    if (!have_decl && !fvf_decode(fvf, &L)) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "winrun: d3d9: vertex format %#x has a position this "
                            "does not read; nothing drawn\n", (unsigned)fvf);
        }
        return;
    }
    int stride = (int)w32_com_get(w, self, DEV_STREAM_STRIDE);
    if (stride > 0) L.stride = stride;

    /* Where the pixels go: an off-screen render target if one is bound,
     * otherwise the back buffer. */
    d3d11_target t;
    uint64_t rt = w32_com_get(w, self, DEV_RT);
    if (rt) {
        t.pixels = (uint32_t *)W32P(w, w32_com_get(w, rt, SURF_BITS));
        t.w = (int)w32_com_get(w, rt, SURF_W);
        t.h = (int)w32_com_get(w, rt, SURF_H);
        t.pitch_px = (int)w32_com_get(w, rt, SURF_PITCH) / 4;
    } else {
        t.pixels = (uint32_t *)W32P(w, w32_com_get(w, self, DEV_FB));
        t.w = (int)w32_com_get(w, self, DEV_W);
        t.h = (int)w32_com_get(w, self, DEV_H);
        t.pitch_px = (int)w32_com_get(w, self, DEV_PITCH) / 4;
    }
    if (!t.pixels || t.w <= 0 || t.h <= 0) return;
    int vx = (int)w32_com_get(w, self, DEV_VP_X), vy = (int)w32_com_get(w, self, DEV_VP_Y);
    int vw = (int)w32_com_get(w, self, DEV_VP_W), vh = (int)w32_com_get(w, self, DEV_VP_H);
    if (vw <= 0 || vh <= 0) { vx = vy = 0; vw = t.w; vh = t.h; }
    t.clip_x = vx; t.clip_y = vy; t.clip_w = vw; t.clip_h = vh;

    d3d11_texture tex;
    int have_tex = L.tex0 >= 0 && bound_texture(w, self, &tex);
    int blend = blend_mode_of(w, self);
    int wrap = w32_com_get(w, self, DEV_ADDRU) != 3;      /* 3 is D3DTADDRESS_CLAMP */

    /* Four position floats means the program transformed them itself -- that
     * is what XYZRHW is, and what D3DDECLUSAGE_POSITIONT is in a declaration.
     * Three means it wants the pipeline. */
    int transformed = have_decl ? L.pos_n == 4
                                : (fvf & FVF_POSITION_MASK) == FVF_XYZRHW;
    float mvp[16];
    if (!transformed) {
        float world[16], view[16], proj[16], wv[16];
        read_matrix(w, self, DEV_MWORLD, world);
        read_matrix(w, self, DEV_MVIEW, view);
        read_matrix(w, self, DEV_MPROJ, proj);
        /* A program that never set a matrix leaves them zero, and a zero
         * matrix collapses every vertex onto the origin -- one dot instead of
         * a scene. Treat "never set" as identity, which is what the device
         * actually starts with. */
        static const float ID[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        int wz = 1, vz = 1, pz = 1;
        for (int i = 0; i < 16; i++) { if (world[i] != 0) wz = 0; if (view[i] != 0) vz = 0; if (proj[i] != 0) pz = 0; }
        if (wz) memcpy(world, ID, sizeof ID);
        if (vz) memcpy(view, ID, sizeof ID);
        if (pz) memcpy(proj, ID, sizeof ID);
        mat_mul(world, view, wv);
        mat_mul(wv, proj, mvp);
        (void)mat_is_identity;
    }

    for (int i = 0; i < prim_count; i++) {
        int a, b, c;
        switch (prim_type) {
        case PT_TRIANGLELIST:  a = 3 * i;  b = 3 * i + 1; c = 3 * i + 2; break;
        case PT_TRIANGLESTRIP: a = i;      b = i + 1;     c = i + 2;     break;
        case PT_TRIANGLEFAN:   a = 0;      b = i + 1;     c = i + 2;     break;
        default: {
            static int warned;
            if (!warned) { warned = 1; fprintf(stderr, "winrun: d3d9 primitive type %d is not drawn yet\n", prim_type); }
            return;
        }
        }
        if (idx16 || idx32) {
            if (a >= nindices || b >= nindices || c >= nindices) return;
            a = (int)(idx16 ? idx16[a] : idx32[a]) + base_vertex;
            b = (int)(idx16 ? idx16[b] : idx32[b]) + base_vertex;
            c = (int)(idx16 ? idx16[c] : idx32[c]) + base_vertex;
        }
        d3d11_vertex v0, v1, v2;
        if (!fetch_vertex(w, self, base, &L, a, mvp, transformed, vx, vy, vw, vh, &v0)) return;
        if (!fetch_vertex(w, self, base, &L, b, mvp, transformed, vx, vy, vw, vh, &v1)) return;
        if (!fetch_vertex(w, self, base, &L, c, mvp, transformed, vx, vy, vw, vh, &v2)) return;
        w32_d3d11_triangle(&t, &v0, &v1, &v2, have_tex ? &tex : 0, wrap, blend);
    }
}

static void draw_from(w32 *w, uint64_t self, uint64_t base, int stride, uint32_t fvf,
                      int prim_type, int prim_count) {
    (void)stride;
    draw_indexed(w, self, base, fvf, prim_type, prim_count, 0, 0, 0, 0);
}

/* CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture,
 * pSharedHandle). Everything is stored as 32-bit BGRA whatever the format
 * asked for: the rasterizer reads one layout, and a game that asked for a
 * 16-bit texture gets a wider one, which costs memory and changes nothing it
 * can observe through LockRect -- except the pitch, which is reported. */
static void d_CreateTexture(w32 *w) {
    uint64_t self = ARG(0), out = ARG(7);
    /* Every one of these is a 32-bit value in the source. On x64 it arrives
     * in the low half of a register and the high half is whatever was there
     * before, so it has to be masked -- reading StartIndex at full width is
     * what turned an index pointer into 0x17d3fb68fa000 and segfaulted the
     * host on the first indexed draw. */
    int tw = (int)(uint32_t)ARG(1), th = (int)(uint32_t)ARG(2);
    int levels = (int)(uint32_t)ARG(3);
    uint32_t fmt = (uint32_t)ARG(5);
    if (!out || tw <= 0 || th <= 0 || tw > 8192 || th > 8192) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t pitch = (uint64_t)tw * 4;
    uint64_t bits = w32_alloc(w, pitch * (unsigned)th, 0);
    if (!bits) { RET(E_FAIL_); return; }
    memset(W32P(w, bits), 0, (size_t)(pitch * (unsigned)th));
    uint64_t tex = w32_com_new(w, &cls_texture, TEX_NFIELDS);
    if (!tex) { RET(E_FAIL_); return; }
    w32_com_set(w, tex, TEX_DEV, self);
    w32_com_set(w, tex, TEX_BITS, bits);
    w32_com_set(w, tex, TEX_W, (uint64_t)tw);
    w32_com_set(w, tex, TEX_H, (uint64_t)th);
    w32_com_set(w, tex, TEX_PITCH, pitch);
    w32_com_set(w, tex, TEX_LEVELS, levels > 0 ? (uint64_t)levels : 1);
    w32_com_set(w, tex, TEX_FMT, fmt ? fmt : FMT_A8R8G8B8);
    w32_write(w, out, w32_ptrsize(w), tex);
    if (w->verbose) fprintf(stderr, "winrun: d3d9 texture %dx%d format %u at %#llx\n",
                            tw, th, (unsigned)fmt, (unsigned long long)bits);
    RET(S_OK_);
}

static void d_CreateIndexBuffer(w32 *w) {
    uint64_t self = ARG(0), length = (uint32_t)ARG(1), out = ARG(5);
    uint32_t fmt = (uint32_t)ARG(3);
    if (!out || !length) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t bits = w32_alloc(w, length, 0);
    if (!bits) { RET(E_FAIL_); return; }
    uint64_t ib = w32_com_new(w, &cls_ibuf, IB_NFIELDS);
    if (!ib) { RET(E_FAIL_); return; }
    w32_com_set(w, ib, IB_DEV, self);
    w32_com_set(w, ib, IB_BITS, bits);
    w32_com_set(w, ib, IB_BYTES, length);
    w32_com_set(w, ib, IB_FMT, fmt);
    w32_write(w, out, w32_ptrsize(w), ib);
    RET(S_OK_);
}

/* SetTexture(Stage, pTexture). Only stage 0 is read by the rasterizer, but a
 * higher stage is still accepted -- multitexturing is how a game does a
 * lightmap, and refusing the call would fail a setup that then draws its base
 * texture perfectly well. */
static void d_SetTexture(w32 *w) {
    if ((uint32_t)ARG(1) == 0) w32_com_set(w, ARG(0), DEV_TEX0, ARG(2));
    RET(S_OK_);
}
static void d_GetTexture(w32 *w) {
    uint64_t out = ARG(2);
    if (out) w32_write(w, out, w32_ptrsize(w),
                       (uint32_t)ARG(1) == 0 ? w32_com_get(w, ARG(0), DEV_TEX0) : 0);
    RET(S_OK_);
}

/* SetTransform(State, pMatrix). 256 is world, 2 is view, 3 is projection. */
static void d_SetTransform(w32 *w) {
    uint64_t self = ARG(0), m = ARG(2);
    uint32_t which = (uint32_t)ARG(1);
    if (!m) { RET(D3DERR_INVALIDCALL); return; }
    int field = which == 2 ? DEV_MVIEW : which == 3 ? DEV_MPROJ
              : (which == 256 || which == 0) ? DEV_MWORLD : -1;
    if (field < 0) { RET(S_OK_); return; }
    for (int i = 0; i < 16; i++)
        w32_com_set(w, self, field + i, w32_read(w, m + (unsigned)i * 4, 4));
    RET(S_OK_);
}
static void d_GetTransform(w32 *w) {
    uint64_t self = ARG(0), m = ARG(2);
    uint32_t which = (uint32_t)ARG(1);
    int field = which == 2 ? DEV_MVIEW : which == 3 ? DEV_MPROJ : DEV_MWORLD;
    if (m) for (int i = 0; i < 16; i++)
        w32_write(w, m + (unsigned)i * 4, 4, w32_com_get(w, self, field + i));
    RET(S_OK_);
}

/* SetRenderState(State, Value). Three of these change what is drawn:
 * D3DRS_ALPHABLENDENABLE (27) and the two blend factors (19 and 20). The rest
 * are accepted, because a game sets thirty of them at startup and failing one
 * it does not need would fail the startup. */
static void d_SetRenderState(w32 *w) {
    uint64_t self = ARG(0);
    uint32_t st = (uint32_t)ARG(1), v = (uint32_t)ARG(2);
    if (st == 27) w32_com_set(w, self, DEV_BLEND, v);
    else if (st == 19) w32_com_set(w, self, DEV_SRCBLEND, v);
    else if (st == 20) w32_com_set(w, self, DEV_DSTBLEND, v);
    RET(S_OK_);
}
/* SetSamplerState(Sampler, Type, Value). ADDRESSU is 1 and ADDRESSV is 2;
 * a sprite sheet clamps and a scrolling background wraps, and drawing one as
 * the other is a visible seam. */
static void d_SetSamplerState(w32 *w) {
    uint64_t self = ARG(0);
    uint32_t type = (uint32_t)ARG(2), v = (uint32_t)ARG(3);
    if ((uint32_t)ARG(1) == 0) {
        if (type == 1) w32_com_set(w, self, DEV_ADDRU, v);
        else if (type == 2) w32_com_set(w, self, DEV_ADDRV, v);
    }
    RET(S_OK_);
}

/* SetViewport(pViewport): X, Y, Width, Height, MinZ, MaxZ. */
static void d_SetViewport(w32 *w) {
    uint64_t self = ARG(0), v = ARG(1);
    if (!v) { RET(D3DERR_INVALIDCALL); return; }
    w32_com_set(w, self, DEV_VP_X, w32_read(w, v, 4));
    w32_com_set(w, self, DEV_VP_Y, w32_read(w, v + 4, 4));
    w32_com_set(w, self, DEV_VP_W, w32_read(w, v + 8, 4));
    w32_com_set(w, self, DEV_VP_H, w32_read(w, v + 12, 4));
    RET(S_OK_);
}
static void d_GetViewport(w32 *w) {
    uint64_t self = ARG(0), v = ARG(1);
    if (!v) { RET(D3DERR_INVALIDCALL); return; }
    int vw = (int)w32_com_get(w, self, DEV_VP_W), vh = (int)w32_com_get(w, self, DEV_VP_H);
    if (vw <= 0) { vw = (int)w32_com_get(w, self, DEV_W); vh = (int)w32_com_get(w, self, DEV_H); }
    w32_write(w, v, 4, w32_com_get(w, self, DEV_VP_X));
    w32_write(w, v + 4, 4, w32_com_get(w, self, DEV_VP_Y));
    w32_write(w, v + 8, 4, (uint64_t)(uint32_t)vw);
    w32_write(w, v + 12, 4, (uint64_t)(uint32_t)vh);
    w32_write(w, v + 16, 4, 0);
    w32_write(w, v + 20, 4, 0x3F800000u);                 /* 1.0f */
    RET(S_OK_);
}

static void d_SetIndices(w32 *w) {
    uint64_t self = ARG(0), ib = ARG(1);
    w32_com_set(w, self, DEV_INDEX, ib ? w32_com_get(w, ib, IB_BITS) : 0);
    w32_com_set(w, self, DEV_INDEX_FMT, ib ? w32_com_get(w, ib, IB_FMT) : 0);
    RET(S_OK_);
}

/* DrawIndexedPrimitive(Type, BaseVertexIndex, MinIndex, NumVertices,
 * StartIndex, PrimitiveCount). */
static void d_DrawIndexedPrimitive(w32 *w) {
    uint64_t self = ARG(0);
    uint64_t idx = w32_com_get(w, self, DEV_INDEX);
    if (!idx) { RET(D3DERR_INVALIDCALL); return; }
    /* D3DFMT_INDEX16 is 101, INDEX32 is 102. */
    int wide = w32_com_get(w, self, DEV_INDEX_FMT) == 102;
    uint32_t start = (uint32_t)ARG(5);
    uint64_t base = w32_com_get(w, self, DEV_STREAM) + w32_com_get(w, self, DEV_STREAM_OFF);
    int count = (int)(uint32_t)ARG(6);
    /* The index buffer is ours, but StartIndex and PrimitiveCount are not, and
     * draw_indexed will read up to three indices per primitive from here. The
     * span is computed in 64 bits because a PrimitiveCount of 0x7fffffff times
     * three is not representable in the int the loop uses. */
    uint64_t isz = wide ? 4 : 2;
    const void *p = W32PN(w, idx + (uint64_t)start * isz, ((uint64_t)(uint32_t)count * 3 + 3) * isz);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    draw_indexed(w, self, base, (uint32_t)w32_com_get(w, self, DEV_FVF),
                 (int)(uint32_t)ARG(1), count,
                 wide ? 0 : (const uint16_t *)p, wide ? (const uint32_t *)p : 0,
                 count * 3 + 3, (int)(int32_t)(uint32_t)ARG(2));
    RET(S_OK_);
}

/* DrawIndexedPrimitiveUP(Type, MinIndex, NumVertices, PrimitiveCount,
 * pIndexData, IndexFormat, pVertexData, VertexStride). */
static void d_DrawIndexedPrimitiveUP(w32 *w) {
    uint64_t self = ARG(0);
    int wide = (uint32_t)ARG(6) == 102;
    int count = (int)(uint32_t)ARG(4);
    /* pIndexData is the caller's own array rather than a buffer object, so
     * both the pointer and the extent of it are the program's word. */
    const void *p = W32PN(w, ARG(5), ((uint64_t)(uint32_t)count * 3 + 3) * (wide ? 4u : 2u));
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    w32_com_set(w, self, DEV_STREAM_STRIDE, (uint32_t)ARG(8));
    draw_indexed(w, self, ARG(7), (uint32_t)w32_com_get(w, self, DEV_FVF),
                 (int)(uint32_t)ARG(1), count,
                 wide ? 0 : (const uint16_t *)p, wide ? (const uint32_t *)p : 0,
                 count * 3 + 3, 0);
    RET(S_OK_);
}

/* ---- the rest of the device -------------------------------------------
 *
 * Seventy-eight of this interface's slots had no entry, and an entry that is
 * not there is not a no-op: the call lands on the unimplemented-slot stub,
 * which stops the run. So a game that set a vertex declaration, or a shader,
 * or a render target -- all of which a 2015-era 2D engine does before it
 * draws anything -- died during setup, having produced no frame and no clue.
 *
 * Everything below is either implemented or accepted. Accepting is the right
 * answer far more often than it looks: SetLight on a renderer with no
 * lighting changes nothing that can be observed, and failing it fails a
 * startup that would otherwise have worked. Where a caller reads an
 * out-parameter, it is filled with something true rather than left as
 * whatever was on the stack.
 */

/* A vertex declaration, which is what replaced the FVF. The elements are
 * D3DVERTEXELEMENT9: { WORD Stream, WORD Offset, BYTE Type, BYTE Method,
 * BYTE Usage, BYTE UsageIndex } -- eight bytes each, ending with the
 * D3DDECL_END sentinel whose stream is 0xFF. Parsing it into the same layout
 * an FVF produces means one vertex fetch serves both. */
static w32_com_class cls_decl;

static void decl_Release(w32 *w) { w32_com_Release(w); }
static const w32_api decl_methods[5] = {
    [0] = { "QueryInterface", 3, 0, w32_com_QueryInterface, 0 },
    [1] = { "AddRef",         1, 0, w32_com_AddRef,         0 },
    [2] = { "Release",        1, 0, decl_Release,           0 },
    [3] = { "GetDevice",      2, 0, d_ok,                   0 },
    [4] = { "GetDeclaration", 3, 0, d_ok,                   0 },
};

/* How many bytes a declaration type occupies, and how many floats of it are
 * worth reading. Only the handful an engine actually uses for position,
 * colour and texture coordinates are named; anything else is skipped by its
 * size, which keeps the offsets of what follows correct. */
static int decl_type_size(int t) {
    switch (t) {
    case 0: return 4;    case 1: return 8;    case 2: return 12;   case 3: return 16;
    case 4: return 4;    /* D3DCOLOR */
    case 5: return 4;    case 6: return 8;    case 7: return 4;    case 8: return 8;
    case 9: return 4;    case 10: return 8;   case 11: return 4;   case 12: return 8;
    case 13: return 4;   case 14: return 8;   case 15: return 4;   case 16: return 8;
    default: return 0;
    }
}

static void d_CreateVertexDeclaration(w32 *w) {
    uint64_t self = ARG(0), elems = ARG(1), out = ARG(2);
    if (!elems || !out) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t d = w32_com_new(w, &cls_decl, DECL_NFIELDS);
    if (!d) { RET(E_FAIL_); return; }
    w32_com_set(w, d, DECL_DEV, self);
    w32_com_set(w, d, DECL_POS, (uint64_t)(int64_t)-1);
    w32_com_set(w, d, DECL_DIFFUSE, (uint64_t)(int64_t)-1);
    w32_com_set(w, d, DECL_TEX0, (uint64_t)(int64_t)-1);
    int stride = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t e = elems + (uint64_t)i * 8;
        uint32_t stream = (uint32_t)w32_read(w, e, 2);
        if (stream == 0xFF) break;
        uint32_t off = (uint32_t)w32_read(w, e + 2, 2);
        int type = (int)w32_read(w, e + 4, 1);
        int usage = (int)w32_read(w, e + 6, 1);
        int uidx = (int)w32_read(w, e + 7, 1);
        int sz = decl_type_size(type);
        if (stream == 0) {
            if (usage == 0 && uidx == 0) {                 /* D3DDECLUSAGE_POSITION */
                w32_com_set(w, d, DECL_POS, off);
                w32_com_set(w, d, DECL_POSN, type == 3 ? 4 : 3);
            } else if (usage == 9 && uidx == 0) {          /* POSITIONT: already transformed */
                w32_com_set(w, d, DECL_POS, off);
                w32_com_set(w, d, DECL_POSN, 4);
            } else if (usage == 10 && uidx == 0) {         /* D3DDECLUSAGE_COLOR */
                w32_com_set(w, d, DECL_DIFFUSE, off);
            } else if (usage == 5 && uidx == 0) {          /* D3DDECLUSAGE_TEXCOORD */
                w32_com_set(w, d, DECL_TEX0, off);
                w32_com_set(w, d, DECL_TEX0N, type == 1 ? 2 : type == 2 ? 3 : 2);
            }
            if ((int)(off + (uint32_t)sz) > stride) stride = (int)(off + (uint32_t)sz);
        }
    }
    w32_com_set(w, d, DECL_STRIDE, (uint64_t)stride);
    w32_write(w, out, w32_ptrsize(w), d);
    RET(S_OK_);
}
static void d_SetVertexDeclaration(w32 *w) {
    w32_com_set(w, ARG(0), DEV_DECL, ARG(1));
    RET(S_OK_);
}
static void d_GetVertexDeclaration(w32 *w) {
    uint64_t out = ARG(1);
    if (out) w32_write(w, out, w32_ptrsize(w), w32_com_get(w, ARG(0), DEV_DECL));
    RET(S_OK_);
}

/* Shaders. Accepted, stored, and not executed -- the same position D3D11
 * takes, and for the same reason: running SM2/SM3 bytecode is a project of
 * its own, and refusing to create a shader stops a game that would otherwise
 * draw its whole world with the fixed-function interpretation. Said once, so
 * a report says what is actually happening rather than leaving someone to
 * wonder why the colours are wrong. */
static void note_shader_not_run(w32 *w, const char *which) {
    static int said;
    if (said) return;
    said = 1;
    fprintf(stderr, "winrun: d3d9: %s shaders are created and bound but not "
                    "executed; drawing uses the fixed-function interpretation "
                    "(texture modulated by vertex colour)\n", which);
    (void)w;
}
static w32_com_class cls_shader;
static const w32_api shader_methods[4] = {
    [0] = { "QueryInterface", 3, 0, w32_com_QueryInterface, 0 },
    [1] = { "AddRef",         1, 0, w32_com_AddRef,         0 },
    [2] = { "Release",        1, 0, w32_com_Release,        0 },
    [3] = { "GetDevice",      2, 0, d_ok,                   0 },
};
static void make_shader(w32 *w, uint64_t out, const char *which) {
    if (!out) { RET(D3DERR_INVALIDCALL); return; }
    note_shader_not_run(w, which);
    uint64_t sh = w32_com_new(w, &cls_shader, 2);
    if (!sh) { RET(E_FAIL_); return; }
    w32_write(w, out, w32_ptrsize(w), sh);
    RET(S_OK_);
}
static void d_CreateVertexShader(w32 *w) { make_shader(w, ARG(2), "vertex"); }
static void d_CreatePixelShader(w32 *w)  { make_shader(w, ARG(2), "pixel"); }
static void d_SetVertexShader(w32 *w) { w32_com_set(w, ARG(0), DEV_VSHADER, ARG(1)); RET(S_OK_); }
static void d_SetPixelShader(w32 *w)  { w32_com_set(w, ARG(0), DEV_PSHADER, ARG(1)); RET(S_OK_); }
static void d_GetVertexShader(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), w32_ptrsize(w), w32_com_get(w, ARG(0), DEV_VSHADER));
    RET(S_OK_);
}
static void d_GetPixelShader(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), w32_ptrsize(w), w32_com_get(w, ARG(0), DEV_PSHADER));
    RET(S_OK_);
}

/* Render targets. A game draws its world into an off-screen surface and then
 * blits it, which is how it does a screen shake or a post-processing pass;
 * with no render target the draw goes to the back buffer, which is where it
 * would have ended up anyway one step later. */
static void d_CreateRenderTarget(w32 *w) {
    int rw = (int)(uint32_t)ARG(1), rh = (int)(uint32_t)ARG(2);
    uint64_t out = ARG(7);
    if (!out || rw <= 0 || rh <= 0 || rw > 8192 || rh > 8192) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t bits = w32_alloc(w, (uint64_t)rw * rh * 4, 0);
    if (!bits) { RET(E_FAIL_); return; }
    memset(W32P(w, bits), 0, (size_t)rw * rh * 4);
    uint64_t s2 = w32_com_new(w, &cls_surface, SURF_NFIELDS);
    if (!s2) { RET(E_FAIL_); return; }
    w32_com_set(w, s2, SURF_DEV, ARG(0));
    w32_com_set(w, s2, SURF_BITS, bits);
    w32_com_set(w, s2, SURF_W, (uint64_t)rw);
    w32_com_set(w, s2, SURF_H, (uint64_t)rh);
    w32_com_set(w, s2, SURF_PITCH, (uint64_t)rw * 4);
    w32_write(w, out, w32_ptrsize(w), s2);
    RET(S_OK_);
}
static void d_CreateOffscreenPlainSurface(w32 *w) {
    /* Same shape, one argument fewer. */
    int rw = (int)(uint32_t)ARG(1), rh = (int)(uint32_t)ARG(2);
    uint64_t out = ARG(5);
    if (!out || rw <= 0 || rh <= 0 || rw > 8192 || rh > 8192) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t bits = w32_alloc(w, (uint64_t)rw * rh * 4, 0);
    if (!bits) { RET(E_FAIL_); return; }
    memset(W32P(w, bits), 0, (size_t)rw * rh * 4);
    uint64_t s2 = w32_com_new(w, &cls_surface, SURF_NFIELDS);
    if (!s2) { RET(E_FAIL_); return; }
    w32_com_set(w, s2, SURF_DEV, ARG(0));
    w32_com_set(w, s2, SURF_BITS, bits);
    w32_com_set(w, s2, SURF_W, (uint64_t)rw);
    w32_com_set(w, s2, SURF_H, (uint64_t)rh);
    w32_com_set(w, s2, SURF_PITCH, (uint64_t)rw * 4);
    w32_write(w, out, w32_ptrsize(w), s2);
    RET(S_OK_);
}
/* SetRenderTarget(Index, pSurface). Index 0 redirects drawing; a NULL surface
 * puts it back on the back buffer, which is also what index 0 means when a
 * program is finished with its off-screen pass. */
static void d_SetRenderTarget(w32 *w) {
    uint64_t self = ARG(0), surf = ARG(2);
    if ((uint32_t)ARG(1) != 0) { RET(S_OK_); return; }
    w32_com_set(w, self, DEV_RT, surf);
    RET(S_OK_);
}
static void d_GetRenderTarget(w32 *w) {
    uint64_t self = ARG(0), out = ARG(2);
    uint64_t rt = w32_com_get(w, self, DEV_RT);
    if (out) w32_write(w, out, w32_ptrsize(w), rt ? rt : w32_com_get(w, self, DEV_BBSURF));
    RET(S_OK_);
}
/* ColorFill(pSurface, pRect, color) -- how a game clears an off-screen
 * surface without a device-wide Clear. */
static void d_ColorFill(w32 *w) {
    uint64_t surf = ARG(1), rect = ARG(2);
    uint32_t colour = (uint32_t)ARG(3);
    if (!surf) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t *px = (uint32_t *)W32P(w, w32_com_get(w, surf, SURF_BITS));
    int sw = (int)w32_com_get(w, surf, SURF_W), sh = (int)w32_com_get(w, surf, SURF_H);
    int pitch = (int)w32_com_get(w, surf, SURF_PITCH) / 4;
    if (!px || sw <= 0 || sh <= 0) { RET(D3DERR_INVALIDCALL); return; }
    int l = 0, t = 0, r = sw, b = sh;
    if (rect) {
        l = (int)(int32_t)w32_read(w, rect, 4);      t = (int)(int32_t)w32_read(w, rect + 4, 4);
        r = (int)(int32_t)w32_read(w, rect + 8, 4);  b = (int)(int32_t)w32_read(w, rect + 12, 4);
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > sw) r = sw;
    if (b > sh) b = sh;
    for (int y = t; y < b; y++) for (int x = l; x < r; x++) px[(size_t)y * pitch + x] = colour;
    RET(S_OK_);
}
/* StretchRect(src, srcRect, dst, dstRect, filter): the blit a game uses to
 * put its off-screen surface on the back buffer. Point-sampled, like
 * everything else here. */
static void d_StretchRect(w32 *w) {
    uint64_t ss = ARG(1), sr = ARG(2), ds = ARG(3), dr = ARG(4);
    if (!ss || !ds) { RET(D3DERR_INVALIDCALL); return; }
    const uint32_t *sp = (const uint32_t *)W32P(w, w32_com_get(w, ss, SURF_BITS));
    uint32_t *dp = (uint32_t *)W32P(w, w32_com_get(w, ds, SURF_BITS));
    if (!sp || !dp) { RET(D3DERR_INVALIDCALL); return; }
    int sw = (int)w32_com_get(w, ss, SURF_W), sh = (int)w32_com_get(w, ss, SURF_H);
    int dw = (int)w32_com_get(w, ds, SURF_W), dh = (int)w32_com_get(w, ds, SURF_H);
    int sp_p = (int)w32_com_get(w, ss, SURF_PITCH) / 4, dp_p = (int)w32_com_get(w, ds, SURF_PITCH) / 4;
    int sl = 0, st = 0, srr = sw, sb = sh, dl = 0, dt = 0, drr = dw, db = dh;
    if (sr) { sl = (int)(int32_t)w32_read(w, sr, 4);  st = (int)(int32_t)w32_read(w, sr + 4, 4);
              srr = (int)(int32_t)w32_read(w, sr + 8, 4); sb = (int)(int32_t)w32_read(w, sr + 12, 4); }
    if (dr) { dl = (int)(int32_t)w32_read(w, dr, 4);  dt = (int)(int32_t)w32_read(w, dr + 4, 4);
              drr = (int)(int32_t)w32_read(w, dr + 8, 4); db = (int)(int32_t)w32_read(w, dr + 12, 4); }
    int dws = drr - dl, dhs = db - dt, sws = srr - sl, shs = sb - st;
    if (dws <= 0 || dhs <= 0 || sws <= 0 || shs <= 0) { RET(D3DERR_INVALIDCALL); return; }
    for (int y = 0; y < dhs; y++) {
        int dy = dt + y;
        if (dy < 0 || dy >= dh) continue;
        int sy = st + y * shs / dhs;
        if (sy < 0 || sy >= sh) continue;
        for (int x = 0; x < dws; x++) {
            int dx = dl + x;
            if (dx < 0 || dx >= dw) continue;
            int sx = sl + x * sws / dws;
            if (sx < 0 || sx >= sw) continue;
            dp[(size_t)dy * dp_p + dx] = sp[(size_t)sy * sp_p + sx];
        }
    }
    RET(S_OK_);
}
/* UpdateTexture(src, dst): a managed texture being pushed to video memory,
 * which here means one guest buffer copied to another. */
static void d_UpdateTexture(w32 *w) {
    uint64_t src = ARG(1), dst = ARG(2);
    if (!src || !dst) { RET(D3DERR_INVALIDCALL); return; }
    const uint32_t *sp = (const uint32_t *)W32P(w, w32_com_get(w, src, TEX_BITS));
    uint32_t *dp = (uint32_t *)W32P(w, w32_com_get(w, dst, TEX_BITS));
    int sw = (int)w32_com_get(w, src, TEX_W), sh = (int)w32_com_get(w, src, TEX_H);
    int dw = (int)w32_com_get(w, dst, TEX_W), dh = (int)w32_com_get(w, dst, TEX_H);
    if (!sp || !dp) { RET(D3DERR_INVALIDCALL); return; }
    int cw = sw < dw ? sw : dw, ch2 = sh < dh ? sh : dh;
    for (int y = 0; y < ch2; y++) memcpy(dp + (size_t)y * dw, sp + (size_t)y * sw, (size_t)cw * 4);
    RET(S_OK_);
}

/* SetScissorRect: the viewport already clips, so the scissor is folded into
 * it -- which is right whenever the scissor is inside the viewport, and that
 * is what a game uses it for. */
static void d_SetScissorRect(w32 *w) {
    uint64_t self = ARG(0), r = ARG(1);
    if (!r) { RET(S_OK_); return; }
    int l = (int)(int32_t)w32_read(w, r, 4),      t = (int)(int32_t)w32_read(w, r + 4, 4);
    int rr = (int)(int32_t)w32_read(w, r + 8, 4), b = (int)(int32_t)w32_read(w, r + 12, 4);
    w32_com_set(w, self, DEV_SC_X, (uint64_t)(uint32_t)l);
    w32_com_set(w, self, DEV_SC_Y, (uint64_t)(uint32_t)t);
    w32_com_set(w, self, DEV_SC_W, (uint64_t)(uint32_t)(rr - l));
    w32_com_set(w, self, DEV_SC_H, (uint64_t)(uint32_t)(b - t));
    RET(S_OK_);
}
static void d_GetScissorRect(w32 *w) {
    uint64_t self = ARG(0), r = ARG(1);
    if (!r) { RET(D3DERR_INVALIDCALL); return; }
    int x = (int)(int32_t)(uint32_t)w32_com_get(w, self, DEV_SC_X);
    int y = (int)(int32_t)(uint32_t)w32_com_get(w, self, DEV_SC_Y);
    int cw = (int)(int32_t)(uint32_t)w32_com_get(w, self, DEV_SC_W);
    int ch2 = (int)(int32_t)(uint32_t)w32_com_get(w, self, DEV_SC_H);
    if (cw <= 0) { x = y = 0; cw = (int)w32_com_get(w, self, DEV_W); ch2 = (int)w32_com_get(w, self, DEV_H); }
    w32_write(w, r, 4, (uint64_t)(uint32_t)x);
    w32_write(w, r + 4, 4, (uint64_t)(uint32_t)y);
    w32_write(w, r + 8, 4, (uint64_t)(uint32_t)(x + cw));
    w32_write(w, r + 12, 4, (uint64_t)(uint32_t)(y + ch2));
    RET(S_OK_);
}

/* Reset: a game that changes resolution, or that has just come back from
 * being minimised. The back buffer is kept and the device stops being lost,
 * which is the state the caller is trying to reach. */
static void d_Reset(w32 *w) {
    w32_d3d9_device_lost(0);
    RET(S_OK_);
}
static void d_GetCreationParameters(w32 *w) {
    uint64_t p = ARG(1);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    w32_write(w, p, 4, 0);                                   /* AdapterOrdinal */
    w32_write(w, p + 4, 4, 1);                               /* D3DDEVTYPE_HAL */
    w32_write(w, p + 8, w32_ptrsize(w), w32_com_get(w, ARG(0), DEV_HWND));
    RET(S_OK_);
}
/* CreateQuery(Type, ppQuery). A NULL out-pointer is the documented way to ask
 * whether a query type is supported, and answering yes to an occlusion query
 * a game then uses to decide what to draw would be answering wrongly. */
static void d_CreateQuery(w32 *w) {
    if (!ARG(2)) { RET(S_OK_); return; }
    RET((uint64_t)(uint32_t)D3DERR_NOTAVAILABLE_);
}
/* GetStreamSource(Number, ppStreamData, pOffset, pStride) */
static void d_GetStreamSource(w32 *w) {
    uint64_t self = ARG(0);
    if (ARG(2)) w32_write(w, ARG(2), w32_ptrsize(w), 0);
    if (ARG(3)) w32_write(w, ARG(3), 4, w32_com_get(w, self, DEV_STREAM_OFF));
    if (ARG(4)) w32_write(w, ARG(4), 4, w32_com_get(w, self, DEV_STREAM_STRIDE));
    RET(S_OK_);
}
static void d_GetIndices(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), w32_ptrsize(w), 0);
    RET(S_OK_);
}
/* Anything whose only out-parameter is a single DWORD the caller reads back.
 * Zero is the honest answer for all of them here. */
static void d_zero_out1(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, 0);
    RET(S_OK_);
}
static void d_zero_out2(w32 *w) {
    if (ARG(2)) w32_write(w, ARG(2), 4, 0);
    RET(S_OK_);
}
static void d_null_out2(w32 *w) {
    if (ARG(2)) w32_write(w, ARG(2), w32_ptrsize(w), 0);
    RET(S_OK_);
}
static void d_GetRasterStatus(w32 *w) {
    uint64_t p = ARG(2);
    if (p) { w32_write(w, p, 4, 0); w32_write(w, p + 4, 4, 0); }   /* not in vblank, scanline 0 */
    RET(S_OK_);
}
/* Shader constants. Stored nowhere, because no shader runs to read them --
 * but a program that sets a hundred of them at startup must not fail on the
 * first one. */
static void d_shader_const_set(w32 *w) { RET(S_OK_); }
static void d_shader_const_get(w32 *w) {
    /* Zeroed rather than left alone: a caller that reads back what it never
     * set gets a defined answer instead of its own uninitialised stack. */
    uint64_t out = ARG(2);
    uint32_t count = (uint32_t)ARG(3);
    if (out && count && count < 4096) {
        void *p = W32PN(w, out, (uint64_t)count * 16);
        if (!p) { RET(D3DERR_INVALIDCALL); return; }
        memset(p, 0, (size_t)count * 16);
    }
    RET(S_OK_);
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
    void *p = W32PN(w, c, 304);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(p, 0, 304);
    w32_write(w, c + 0, 4, 1);                       /* DeviceType = D3DDEVTYPE_HAL */
    RET(S_OK_);
}

#define NM(a) ((int)(sizeof (a) / sizeof (a)[0]))

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
    [44]  = { "SetTransform",           3, 0, d_SetTransform,         0 },
    [45]  = { "GetTransform",           3, 0, d_GetTransform,         0 },
    [46]  = { "MultiplyTransform",      3, 0, d_ok,                   0 },
    [47]  = { "SetViewport",            2, 0, d_SetViewport,          0 },
    [48]  = { "GetViewport",            2, 0, d_GetViewport,          0 },
    [57]  = { "SetRenderState",         3, 0, d_SetRenderState,       0 },
    [58]  = { "GetRenderState",         3, 0, d_ok,                   0 },
    [64]  = { "GetTexture",             3, 0, d_GetTexture,           0 },
    [65]  = { "SetTexture",             3, 0, d_SetTexture,           0 },
    [66]  = { "GetTextureStageState",   4, 0, d_ok,                   0 },
    [67]  = { "SetTextureStageState",   4, 0, d_ok,                   0 },
    [68]  = { "GetSamplerState",        4, 0, d_ok,                   0 },
    [69]  = { "SetSamplerState",        4, 0, d_SetSamplerState,      0 },
    [77]  = { "SetSoftwareVertexProcessing", 2, 0, d_ok,              0 },
    [23]  = { "CreateTexture",          9, 0, d_CreateTexture,        0 },
    [26]  = { "CreateVertexBuffer",     7, 0, d_CreateVertexBuffer,   0 },
    [27]  = { "CreateIndexBuffer",      7, 0, d_CreateIndexBuffer,    0 },
    [81]  = { "DrawPrimitive",          4, 0, d_DrawPrimitive,        0 },
    [82]  = { "DrawIndexedPrimitive",   7, 0, d_DrawIndexedPrimitive, 0 },
    [83]  = { "DrawPrimitiveUP",        5, 0, d_DrawPrimitiveUP,      0 },
    [84]  = { "DrawIndexedPrimitiveUP", 9, 0, d_DrawIndexedPrimitiveUP, 0 },
    [104] = { "SetIndices",             2, 0, d_SetIndices,           0 },
    [89]  = { "SetFVF",                 2, 0, d_SetFVF,               0 },
    [90]  = { "GetFVF",                 2, 0, d_GetFVF,               0 },
    [100] = { "SetStreamSource",        5, 0, d_SetStreamSource,      0 },

    /* The rest of the interface. Generated by walking d3d9.h's own
     * DECLARE_INTERFACE_ block, so the slot numbers and argument counts
     * are the header's rather than anyone's memory. Most are accepted and
     * do nothing, which is the correct behaviour for a renderer with no
     * lighting, no palettes and no patches -- and is very different from
     * having no entry at all, which stops the run. */
    [9  ] = { "GetCreationParameters",       2, 0, d_GetCreationParameters,   0 },
    [10 ] = { "SetCursorProperties",         4, 0, d_ok,                      0 },
    [11 ] = { "SetCursorPosition",           4, 0, d_ok,                      0 },
    [13 ] = { "CreateAdditionalSwapChain",   3, 0, d_null_out2,               0 },
    [14 ] = { "GetSwapChain",                3, 0, d_zero_out1,               0 },
    [16 ] = { "Reset",                       2, 0, d_Reset,                   0 },
    [19 ] = { "GetRasterStatus",             3, 0, d_GetRasterStatus,         0 },
    [20 ] = { "SetDialogBoxMode",            2, 0, d_ok,                      0 },
    [21 ] = { "SetGammaRamp",                4, 0, d_ok,                      0 },
    [22 ] = { "GetGammaRamp",                3, 0, d_zero_out2,               0 },
    [24 ] = { "CreateVolumeTexture",         10, 0, d_null_out2,               0 },
    [25 ] = { "CreateCubeTexture",           8, 0, d_null_out2,               0 },
    [28 ] = { "CreateRenderTarget",          9, 0, d_CreateRenderTarget,      0 },
    [29 ] = { "CreateDepthStencilSurface",   9, 0, d_ok,                      0 },
    [30 ] = { "UpdateSurface",               5, 0, d_ok,                      0 },
    [31 ] = { "UpdateTexture",               3, 0, d_UpdateTexture,           0 },
    [32 ] = { "GetRenderTargetData",         3, 0, d_zero_out2,               0 },
    [33 ] = { "GetFrontBufferData",          3, 0, d_zero_out2,               0 },
    [34 ] = { "StretchRect",                 6, 0, d_StretchRect,             0 },
    [35 ] = { "ColorFill",                   4, 0, d_ColorFill,               0 },
    [36 ] = { "CreateOffscreenPlainSurface", 7, 0, d_CreateOffscreenPlainSurface, 0 },
    [37 ] = { "SetRenderTarget",             3, 0, d_SetRenderTarget,         0 },
    [38 ] = { "GetRenderTarget",             3, 0, d_GetRenderTarget,         0 },
    [39 ] = { "SetDepthStencilSurface",      2, 0, d_ok,                      0 },
    [40 ] = { "GetDepthStencilSurface",      2, 0, d_zero_out1,               0 },
    [49 ] = { "SetMaterial",                 2, 0, d_ok,                      0 },
    [50 ] = { "GetMaterial",                 2, 0, d_zero_out2,               0 },
    [51 ] = { "SetLight",                    3, 0, d_ok,                      0 },
    [52 ] = { "GetLight",                    3, 0, d_zero_out2,               0 },
    [53 ] = { "LightEnable",                 3, 0, d_ok,                      0 },
    [54 ] = { "GetLightEnable",              3, 0, d_zero_out2,               0 },
    [55 ] = { "SetClipPlane",                3, 0, d_ok,                      0 },
    [56 ] = { "GetClipPlane",                3, 0, d_zero_out2,               0 },
    [59 ] = { "CreateStateBlock",            3, 0, d_null_out2,               0 },
    [60 ] = { "BeginStateBlock",             1, 0, d_ok,                      0 },
    [61 ] = { "EndStateBlock",               2, 0, d_zero_out1,               0 },
    [62 ] = { "SetClipStatus",               2, 0, d_ok,                      0 },
    [63 ] = { "GetClipStatus",               2, 0, d_zero_out2,               0 },
    [70 ] = { "ValidateDevice",              2, 0, d_ok,                      0 },
    [71 ] = { "SetPaletteEntries",           3, 0, d_ok,                      0 },
    [72 ] = { "GetPaletteEntries",           3, 0, d_zero_out2,               0 },
    [73 ] = { "SetCurrentTexturePalette",    2, 0, d_ok,                      0 },
    [74 ] = { "GetCurrentTexturePalette",    2, 0, d_zero_out1,               0 },
    [75 ] = { "SetScissorRect",              2, 0, d_SetScissorRect,          0 },
    [76 ] = { "GetScissorRect",              2, 0, d_GetScissorRect,          0 },
    [78 ] = { "GetSoftwareVertexProcessing", 1, 0, d_zero_out1,               0 },
    [79 ] = { "SetNPatchMode",               2, 0, d_ok,                      0 },
    [80 ] = { "GetNPatchMode",               1, 0, d_zero_out1,               0 },
    [85 ] = { "ProcessVertices",             7, 0, d_ok,                      0 },
    [86 ] = { "CreateVertexDeclaration",     3, 0, d_CreateVertexDeclaration, 0 },
    [87 ] = { "SetVertexDeclaration",        2, 0, d_SetVertexDeclaration,    0 },
    [88 ] = { "GetVertexDeclaration",        2, 0, d_GetVertexDeclaration,    0 },
    [91 ] = { "CreateVertexShader",          3, 0, d_CreateVertexShader,      0 },
    [92 ] = { "SetVertexShader",             2, 0, d_SetVertexShader,         0 },
    [93 ] = { "GetVertexShader",             2, 0, d_GetVertexShader,         0 },
    [94 ] = { "SetVertexShaderConstantF",    4, 0, d_shader_const_set,        0 },
    [95 ] = { "GetVertexShaderConstantF",    4, 0, d_shader_const_get,        0 },
    [96 ] = { "SetVertexShaderConstantI",    4, 0, d_shader_const_set,        0 },
    [97 ] = { "GetVertexShaderConstantI",    4, 0, d_shader_const_get,        0 },
    [98 ] = { "SetVertexShaderConstantB",    4, 0, d_shader_const_set,        0 },
    [99 ] = { "GetVertexShaderConstantB",    4, 0, d_shader_const_get,        0 },
    [101] = { "GetStreamSource",             5, 0, d_GetStreamSource,         0 },
    [102] = { "SetStreamSourceFreq",         3, 0, d_ok,                      0 },
    [103] = { "GetStreamSourceFreq",         3, 0, d_ok,                      0 },
    [105] = { "GetIndices",                  2, 0, d_GetIndices,              0 },
    [106] = { "CreatePixelShader",           3, 0, d_CreatePixelShader,       0 },
    [107] = { "SetPixelShader",              2, 0, d_SetPixelShader,          0 },
    [108] = { "GetPixelShader",              2, 0, d_GetPixelShader,          0 },
    [109] = { "SetPixelShaderConstantF",     4, 0, d_shader_const_set,        0 },
    [110] = { "GetPixelShaderConstantF",     4, 0, d_shader_const_get,        0 },
    [111] = { "SetPixelShaderConstantI",     4, 0, d_shader_const_set,        0 },
    [112] = { "GetPixelShaderConstantI",     4, 0, d_shader_const_get,        0 },
    [113] = { "SetPixelShaderConstantB",     4, 0, d_shader_const_set,        0 },
    [114] = { "GetPixelShaderConstantB",     4, 0, d_shader_const_get,        0 },
    [115] = { "DrawRectPatch",               4, 0, d_ok,                      0 },
    [116] = { "DrawTriPatch",                4, 0, d_ok,                      0 },
    [117] = { "DeletePatch",                 2, 0, d_ok,                      0 },
    [118] = { "CreateQuery",                 3, 0, d_CreateQuery,             0 },
};

/* The lengths, asserted rather than trusted. D3D11 taught this the expensive
 * way -- a table one entry short sends every call after it to the neighbouring
 * method, and the symptom appears somewhere else entirely. The numbers came
 * out of d3d9.h mechanically, by parsing its DECLARE_INTERFACE_ blocks, and
 * that same pass caught CreateTexture written at slot 20 when it is at 23:
 * three slots earlier is CreateVolumeTexture, so a game creating its first
 * sprite sheet would have created a volume instead. */
_Static_assert(NM(device_methods)  == 119, "IDirect3DDevice9 has 119 methods");
_Static_assert(NM(texture_full)    == 22,  "IDirect3DTexture9 has 22 methods");
_Static_assert(NM(ibuf_methods)    == 14,  "IDirect3DIndexBuffer9 has 14 methods");
_Static_assert(NM(vbuf_methods)    == 14,  "IDirect3DVertexBuffer9 has 14 methods");
_Static_assert(NM(surface_methods) == 17,  "IDirect3DSurface9 has 17 methods");

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
    char *id = W32PN(w, p, 1104);
    if (!id) { RET(D3DERR_INVALIDCALL); return; }
    memset(id, 0, 1104);
    const char *drv = "d12mt.dll", *desc = "xcore d3d9 on Metal (d12mt)", *dev = "\\\\.\\DISPLAY1";
    memcpy(id, drv, strlen(drv));
    memcpy(id + 512, desc, strlen(desc));
    memcpy(id + 1024, dev, strlen(dev));
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
    void *p = W32PN(w, c, 304);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(p, 0, 304);
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
static w32_com_class cls_texture = { "IDirect3DTexture9", texture_full, 22, TAG_TEXTURE, 0, {0} };
static w32_com_class cls_ibuf    = { "IDirect3DIndexBuffer9", ibuf_methods, 14, TAG_INDEXBUFFER, 0, {0} };
static w32_com_class cls_decl    = { "IDirect3DVertexDeclaration9", decl_methods, 5, TAG_DECL, 0, {0} };
static w32_com_class cls_shader  = { "IDirect3DShader9", shader_methods, 4, TAG_SHADER, 0, {0} };

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
