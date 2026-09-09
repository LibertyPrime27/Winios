/* Direct3D 11.
 *
 * This is the API a game made in the last decade draws through. GameMaker,
 * Unity and Unreal all target it on Windows, and until now this project had
 * nothing behind it: d3d9.c existed and went through a software rasterizer,
 * and D3D10, 11 and 12 did not exist at all. A game would load, resolve
 * every other import, ask for a device, and stop.
 *
 * What is here
 * ------------
 * The object model and the draw path: a device, an immediate context, a
 * swap chain, buffers, 2D textures, the three view types, input layouts,
 * shader objects, and the state objects for blending, sampling,
 * rasterization and depth. A draw call fetches vertices through the input
 * layout, transforms them, assembles triangles by topology, and rasterizes
 * them into the swap chain's back buffer, which Present hands to the same
 * display path every other frame in this project goes through.
 *
 * What is not here, and why it matters
 * ------------------------------------
 * The shaders are not executed. A D3D11 program hands over compiled DXBC --
 * bytecode for a GPU that does not exist here -- and running it is a shader
 * interpreter, which is its own project and not this one.
 *
 * Instead the pipeline is *interpreted*: the shader's signature is read
 * (see dxbc.c), which says which part of a vertex is a position, a texture
 * coordinate and a colour, and then the stage does what the overwhelmingly
 * common shader does with them. The vertex stage transforms the position by
 * the 4x4 matrix in the first constant buffer, if one is bound, and passes
 * the rest through. The pixel stage samples texture 0 and multiplies by the
 * interpolated colour.
 *
 * That is exactly right for how a 2D engine draws -- which is nearly all of
 * what a 2D game does -- and it is wrong for a shader that does something of
 * its own: a palette swap, a blur, a lighting pass. Those will draw, and
 * they will draw the unshaded version. The run report says so, once per
 * shader, because a wrong picture with no explanation is worse than a
 * missing one.
 *
 * Two other honest limits. There is no depth buffer: draws land in the order
 * they are issued, which is what a 2D engine wants and what a 3D one does
 * not. And interpolation is affine rather than perspective-correct, which is
 * invisible at w = 1 and swims across a triangle in a 3D scene.
 *
 * Everything below the shaders is real. The formats are decoded from the
 * actual DXGI values, the input layout is walked as the program declared it,
 * the topologies are assembled properly, and the arithmetic is integer, so a
 * frame is identical on an x86 runner, under qemu on aarch64, and on a
 * phone. That is what makes it testable rather than plausible.
 */
#define _GNU_SOURCE
#include "w32.h"
#include "dxbc.h"
#include "dxbc_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    S_OK_ = 0, S_FALSE_ = 1,
    E_FAIL_ = (int)0x80004005, E_INVALIDARG_ = (int)0x80070057,
    E_OUTOFMEMORY_ = (int)0x8007000E, E_NOINTERFACE_ = (int)0x80004002,
    DXGI_ERROR_UNSUPPORTED_ = (int)0x887A0004,
    DXGI_ERROR_NOT_FOUND_   = (int)0x887A0002,
    DXGI_ERROR_INVALID_CALL_ = (int)0x887A0001,
    DXGI_ERROR_DEVICE_REMOVED_ = (int)0x887A0005,
};

/* Class tags. Distinct from d3d9's so that handing a d3d9 surface to a
 * d3d11 call is caught rather than misread. */
enum {
    TAG_D11_DEVICE = 0x3B11, TAG_D11_CONTEXT, TAG_D11_SWAPCHAIN,
    TAG_D11_TEXTURE, TAG_D11_BUFFER, TAG_D11_RTV, TAG_D11_SRV, TAG_D11_DSV,
    TAG_D11_LAYOUT, TAG_D11_VS, TAG_D11_PS, TAG_D11_SAMPLER,
    TAG_D11_BLEND, TAG_D11_RASTER, TAG_D11_DEPTH, TAG_D11_QUERY,
    TAG_DXGI_FACTORY, TAG_DXGI_DEVICE, TAG_DXGI_ADAPTER, TAG_DXGI_OUTPUT,
};

/* ---------------------------------------------------------------- formats */

enum {
    FMT_UNKNOWN            = 0,
    FMT_R32G32B32A32_FLOAT = 2,
    FMT_R32G32B32_FLOAT    = 6,
    FMT_R16G16B16A16_FLOAT = 10,
    FMT_R32G32_FLOAT       = 16,
    FMT_R8G8B8A8_UNORM     = 28,
    FMT_R8G8B8A8_UNORM_SRGB = 29,
    FMT_R16G16_FLOAT       = 34,
    FMT_R32_FLOAT          = 41,
    FMT_R8G8B8A8_UINT      = 30,
    FMT_B8G8R8A8_UNORM     = 87,
    FMT_B8G8R8X8_UNORM     = 88,
    FMT_R16_UINT           = 57,
    FMT_R32_UINT           = 42,
    FMT_R8_UNORM           = 61,
};

static int format_bytes(uint32_t f) {
    switch (f) {
    case FMT_R32G32B32A32_FLOAT: return 16;
    case FMT_R32G32B32_FLOAT:    return 12;
    case FMT_R16G16B16A16_FLOAT: return 8;
    case FMT_R32G32_FLOAT:       return 8;
    case FMT_R8G8B8A8_UNORM: case FMT_R8G8B8A8_UNORM_SRGB: case FMT_R8G8B8A8_UINT:
    case FMT_B8G8R8A8_UNORM: case FMT_B8G8R8X8_UNORM:
    case FMT_R16G16_FLOAT: case FMT_R32_FLOAT: case FMT_R32_UINT: return 4;
    case FMT_R16_UINT:           return 2;
    case FMT_R8_UNORM:           return 1;
    default:                     return 4;
    }
}

/* A float out of guest memory. The guest's floats are IEEE 754 little-endian
 * and so are ours, so this is a reinterpretation rather than a conversion --
 * done through memcpy because casting a pointer would be an aliasing
 * violation the compiler is allowed to act on. */
static float rdf(w32 *w, uint64_t at) {
    uint32_t bits = (uint32_t)w32_read(w, at, 4);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
/* Half-precision, which a compact vertex format uses for texture
 * coordinates. Five bits of exponent, ten of mantissa, bias 15. */
static float rdhalf(w32 *w, uint64_t at) {
    uint32_t h = (uint32_t)w32_read(w, at, 2);
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (!man) bits = sign << 31;                       /* zero */
        else {                                             /* subnormal */
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3FF;
            bits = (sign << 31) | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) bits = (sign << 31) | 0x7F800000u | (man << 13);   /* inf/nan */
    else bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* One vertex attribute, read in whatever format the input layout declared.
 * Missing components default to (0,0,0,1), which is what the hardware does
 * and what a shader written against a three-component position expects. */
static void read_attr(w32 *w, uint64_t at, uint32_t fmt, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (fmt) {
    case FMT_R32G32B32A32_FLOAT: out[3] = rdf(w, at + 12);            /* fall through */
    case FMT_R32G32B32_FLOAT:    out[2] = rdf(w, at + 8);             /* fall through */
    case FMT_R32G32_FLOAT:       out[1] = rdf(w, at + 4);             /* fall through */
    case FMT_R32_FLOAT:          out[0] = rdf(w, at);
        break;
    case FMT_R16G16B16A16_FLOAT:
        out[0] = rdhalf(w, at); out[1] = rdhalf(w, at + 2);
        out[2] = rdhalf(w, at + 4); out[3] = rdhalf(w, at + 6);
        break;
    case FMT_R16G16_FLOAT:
        out[0] = rdhalf(w, at); out[1] = rdhalf(w, at + 2);
        break;
    case FMT_R8G8B8A8_UNORM: case FMT_R8G8B8A8_UNORM_SRGB: {
        uint32_t v = (uint32_t)w32_read(w, at, 4);
        out[0] = (float)(v & 0xFF) / 255.0f;
        out[1] = (float)((v >> 8) & 0xFF) / 255.0f;
        out[2] = (float)((v >> 16) & 0xFF) / 255.0f;
        out[3] = (float)((v >> 24) & 0xFF) / 255.0f;
        break;
    }
    case FMT_B8G8R8A8_UNORM: case FMT_B8G8R8X8_UNORM: {
        uint32_t v = (uint32_t)w32_read(w, at, 4);
        out[2] = (float)(v & 0xFF) / 255.0f;
        out[1] = (float)((v >> 8) & 0xFF) / 255.0f;
        out[0] = (float)((v >> 16) & 0xFF) / 255.0f;
        out[3] = (float)((v >> 24) & 0xFF) / 255.0f;
        break;
    }
    default:
        out[0] = rdf(w, at);
        break;
    }
}

static uint32_t pack_color(const float c[4]) {
    int r = (int)(c[0] * 255.0f + 0.5f), g = (int)(c[1] * 255.0f + 0.5f);
    int b = (int)(c[2] * 255.0f + 0.5f), a = (int)(c[3] * 255.0f + 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* --------------------------------------------------------- parsed shaders */

/* The signatures of every shader the program has created. Held here rather
 * than in guest memory because they are ours: the guest gave us a blob and
 * has no idea we looked inside it. */
enum { MAX_SHADERS = 64 };
static dxbc_info  g_shaders[MAX_SHADERS];
static dxbc_prog *g_progs[MAX_SHADERS];         /* the decoded code, or NULL with the reason below */
static char       g_prog_why[MAX_SHADERS][96];
static char       g_shader_noted[MAX_SHADERS];
static int        g_nshaders;

void w32_d3d11_reset(void) {
    for (int i = 0; i < MAX_SHADERS; i++) { dxbc_free(&g_shaders[i]); dxbc_prog_free(g_progs[i]); g_progs[i] = 0; g_prog_why[i][0] = 0; }
    memset(g_shaders, 0, sizeof g_shaders);
    memset(g_shader_noted, 0, sizeof g_shader_noted);
    g_nshaders = 0;
}

static int shader_record(w32 *w, uint64_t blob, uint64_t len) {
    if (g_nshaders >= MAX_SHADERS) return -1;
    int i = g_nshaders++;
    /* Both the blob and its length come from the program, and dxbc_parse
     * walks the whole thing chunk by chunk, so a length that runs past the
     * end of the allocation is read out of bounds before anything notices. */
    const void *p = len ? W32PN(w, blob, len) : 0;
    if (p) dxbc_parse(p, (size_t)len, &g_shaders[i]);
    /* Decoded now, once, so a draw only runs it. A container with signatures
     * and no code -- the hand-built ones in the tests -- has nothing to run. */
    if (g_shaders[i].code && g_shaders[i].code_words) {
        g_progs[i] = dxbc_prog_new(g_shaders[i].code, g_shaders[i].code_words, g_prog_why[i], sizeof g_prog_why[i]);
        if (w->verbose) fprintf(stderr, "winrun: d3d11: shader %d: %s\n", i, g_progs[i] ? "decoded, will run" : g_prog_why[i]);
    }
    return i;
}
static const dxbc_info *shader_info(int i) {
    return (i >= 0 && i < g_nshaders) ? &g_shaders[i] : 0;
}
/* Said once per shader, the first time one is actually used to draw. Once
 * per shader rather than once per draw, because a game issues thousands of
 * draws a second and the report has to stay readable -- and once overall
 * would hide how many different shaders were skipped. */
static void note_shader_not_run(w32 *w, int i) {
    if (i < 0 || i >= MAX_SHADERS || g_shader_noted[i]) return;
    g_shader_noted[i] = 1;
    static char *labels[MAX_SHADERS];
    if (!labels[i]) {
        labels[i] = malloc(200);
        if (!labels[i]) return;
        if (g_prog_why[i][0])
            snprintf(labels[i], 200, "d3d11: shader %d uses %s, so it drew with the built-in pipeline instead of its own code", i, g_prog_why[i]);
        else if (g_shaders[i].code_words)
            snprintf(labels[i], 200, "d3d11: shader %d could not be paired with a runnable shader for the other stage; the built-in pipeline drew", i);
        else
            snprintf(labels[i], 200, "d3d11: shader %d has no code (signatures only); the built-in pipeline drew", i);
    }
    w32_note_refused(w, labels[i]);
}

/* ------------------------------------------------------------ object state
 *
 * Field slots. Every object's state lives in guest memory after the vtable
 * pointer and reference count -- see com.c -- so there is nothing host-side
 * to free and an object is exactly as valid as the memory it sits in.
 */
enum { DEV_CTX = 0, DEV_SWAP, DEV_FLAGS, DEV_N };
enum {
    CTX_DEV = 0, CTX_RTV, CTX_VB, CTX_VB_STRIDE, CTX_VB_OFFSET,
    CTX_IB, CTX_IB_FMT, CTX_IB_OFFSET, CTX_LAYOUT, CTX_VS, CTX_PS,
    CTX_TOPOLOGY, CTX_VSCB0, CTX_PSCB0, CTX_SRV0, CTX_SAMPLER0, CTX_BLEND,
    CTX_VP_X, CTX_VP_Y, CTX_VP_W, CTX_VP_H, CTX_DRAWS,
    CTX_VSCB1, CTX_VSCB2, CTX_VSCB3, CTX_PSCB1, CTX_PSCB2, CTX_PSCB3,
    CTX_SRV1, CTX_SRV2, CTX_SRV3, CTX_SAMPLER1, CTX_SAMPLER2, CTX_SAMPLER3, CTX_N
};
enum { SC_DEV = 0, SC_TEX, SC_W, SC_H, SC_FRAMES, SC_HWND, SC_WAITABLE, SC_N };
enum { TEX_W = 0, TEX_H, TEX_PITCH, TEX_PIXELS, TEX_FMT, TEX_BIND, TEX_USAGE, TEX_N };
enum { BUF_DATA = 0, BUF_SIZE, BUF_BIND, BUF_USAGE, BUF_STRIDE, BUF_N };
enum { VIEW_RES = 0, VIEW_FMT, VIEW_N };
enum { LAY_ELEMS = 0, LAY_COUNT, LAY_SHADER, LAY_N };
enum { SH_BLOB = 0, SH_LEN, SH_INFO, SH_N };
enum { SMP_FILTER = 0, SMP_ADDRU, SMP_ADDRV, SMP_N };
enum { BL_ENABLE = 0, BL_SRC, BL_DST, BL_OP, BL_N };
enum { RS_FILL = 0, RS_CULL, RS_SCISSOR, RS_N };

static w32_com_class cls_device, cls_context, cls_swapchain, cls_texture,
                     cls_buffer, cls_rtv, cls_srv, cls_dsv, cls_layout,
                     cls_vshader, cls_pshader, cls_sampler, cls_blend,
                     cls_raster, cls_depth, cls_query,
                     cls_factory, cls_dxgidev, cls_adapter;

/* Write an interface pointer into an out-parameter, or report that the
 * caller passed none. A D3D11 Create* with a null out-pointer is a
 * documented way of asking "would this succeed?", so it is not an error. */
static int out_ptr(w32 *w, uint64_t out, uint64_t obj) {
    if (!out) return 1;
    if (!obj) { w32_write(w, out, (int)w32_ptrsize(w), 0); return 0; }
    w32_write(w, out, (int)w32_ptrsize(w), obj);
    return 1;
}

/* --------------------------------------------------------------- resources */

/* A texture's pixels live in guest memory: the program may Map it and write
 * to it directly, and a copy on our side would then be stale. */
static uint64_t make_texture(w32 *w, int width, int height, uint32_t fmt,
                             uint32_t bind, uint32_t usage) {
    if (width <= 0 || height <= 0 || (int64_t)width * height > 64 * 1024 * 1024) return 0;
    uint64_t obj = w32_com_new(w, &cls_texture, TEX_N);
    if (!obj) return 0;
    uint64_t bytes = (uint64_t)width * (uint64_t)height * 4;
    uint64_t px = w32_alloc(w, bytes + 4096, 0);
    if (!px) return 0;
    w32_com_set(w, obj, TEX_W, (uint64_t)width);
    w32_com_set(w, obj, TEX_H, (uint64_t)height);
    w32_com_set(w, obj, TEX_PITCH, (uint64_t)width * 4);
    w32_com_set(w, obj, TEX_PIXELS, px);
    w32_com_set(w, obj, TEX_FMT, fmt);
    w32_com_set(w, obj, TEX_BIND, bind);
    w32_com_set(w, obj, TEX_USAGE, usage);
    return obj;
}

/* The texture behind a view, whichever kind of view it is. */
static uint64_t view_texture(w32 *w, uint64_t view) {
    if (!view) return 0;
    int tag = w32_com_tag(w, view);
    if (tag != TAG_D11_RTV && tag != TAG_D11_SRV && tag != TAG_D11_DSV) return 0;
    uint64_t res = w32_com_get(w, view, VIEW_RES);
    return w32_com_tag(w, res) == TAG_D11_TEXTURE ? res : 0;
}

static int texture_target(w32 *w, uint64_t tex, d3d11_target *t) {
    if (!tex) return 0;
    memset(t, 0, sizeof *t);
    t->w = (int)w32_com_get(w, tex, TEX_W);
    t->h = (int)w32_com_get(w, tex, TEX_H);
    t->pitch_px = (int)(w32_com_get(w, tex, TEX_PITCH) / 4);
    t->pixels = W32P(w, w32_com_get(w, tex, TEX_PIXELS));
    if (!t->pixels || t->w <= 0 || t->h <= 0) return 0;
    t->clip_x = 0; t->clip_y = 0; t->clip_w = t->w; t->clip_h = t->h;
    return 1;
}

/* ------------------------------------------------------------ ID3D11Device */

static void dev_CreateBuffer(w32 *w) {
    /* (desc, initialData, out): D3D11_BUFFER_DESC is ByteWidth, Usage,
     * BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride -- six
     * DWORDs, the same in both bitnesses. */
    uint64_t desc = ARG(1), init = ARG(2), out = ARG(3);
    if (!desc) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t size = (uint32_t)w32_read(w, desc, 4);
    uint32_t usage = (uint32_t)w32_read(w, desc + 4, 4);
    uint32_t bind = (uint32_t)w32_read(w, desc + 8, 4);
    uint32_t stride = (uint32_t)w32_read(w, desc + 20, 4);
    if (!size || size > 256u * 1024 * 1024) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    /* D3D11_SUBRESOURCE_DATA is { pSysMem, SysMemPitch, SysMemSlicePitch }.
     * The initial contents are checked before anything is allocated, so a
     * program that hands over a stale pSysMem gets the E_INVALIDARG the
     * debug layer would give it rather than a half-built buffer. */
    const void *sys_mem = 0;
    if (init) {
        uint64_t src = w32_read(w, init, (int)w32_ptrsize(w));
        if (src && !(sys_mem = W32PN(w, src, size))) {
            out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return;
        }
    }
    uint64_t obj = w32_com_new(w, &cls_buffer, BUF_N);
    uint64_t data = obj ? w32_alloc(w, size + 4096, 0) : 0;
    if (!obj || !data) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    w32_com_set(w, obj, BUF_DATA, data);
    w32_com_set(w, obj, BUF_SIZE, size);
    w32_com_set(w, obj, BUF_BIND, bind);
    w32_com_set(w, obj, BUF_USAGE, usage);
    w32_com_set(w, obj, BUF_STRIDE, stride);
    if (sys_mem) memcpy(W32P(w, data), sys_mem, size);
    out_ptr(w, out, obj);
    RET(S_OK_);
}

static void dev_CreateTexture2D(w32 *w) {
    /* D3D11_TEXTURE2D_DESC: Width, Height, MipLevels, ArraySize, Format,
     * SampleDesc{Count,Quality}, Usage, BindFlags, CPUAccessFlags,
     * MiscFlags -- eleven DWORDs. */
    uint64_t desc = ARG(1), init = ARG(2), out = ARG(3);
    if (!desc) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    int width = (int)(uint32_t)w32_read(w, desc, 4);
    int height = (int)(uint32_t)w32_read(w, desc + 4, 4);
    uint32_t fmt = (uint32_t)w32_read(w, desc + 16, 4);
    uint32_t usage = (uint32_t)w32_read(w, desc + 28, 4);
    uint32_t bind = (uint32_t)w32_read(w, desc + 32, 4);
    uint64_t obj = make_texture(w, width, height, fmt, bind, usage);
    if (!obj) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    if (init) {
        uint64_t src = w32_read(w, init, (int)w32_ptrsize(w));
        uint32_t src_pitch = (uint32_t)w32_read(w, init + w32_ptrsize(w), 4);
        if (src) {
            uint64_t dst = w32_com_get(w, obj, TEX_PIXELS);
            uint32_t dst_pitch = (uint32_t)w32_com_get(w, obj, TEX_PITCH);
            if (!src_pitch) src_pitch = dst_pitch;
            uint32_t n = dst_pitch < src_pitch ? dst_pitch : src_pitch;
            /* Every row but the last spans a whole pitch, so the span the
             * copy touches is pitch * (height - 1) + n. In 64-bit
             * arithmetic: a SysMemPitch near 2^32 multiplied out in 32 bits
             * wraps to something small and would sail through the check. */
            const uint8_t *sp = W32PN(w, src, (uint64_t)src_pitch * (uint64_t)(height - 1) + n);
            if (!sp) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
            for (int y = 0; y < height; y++)
                memcpy((uint8_t *)W32P(w, dst) + (size_t)y * dst_pitch,
                       sp + (size_t)y * src_pitch, n);
        }
    }
    out_ptr(w, out, obj);
    RET(S_OK_);
}

static uint64_t make_view(w32 *w, w32_com_class *cls, uint64_t res, uint64_t desc) {
    uint64_t obj = w32_com_new(w, cls, VIEW_N);
    if (!obj) return 0;
    w32_com_set(w, obj, VIEW_RES, res);
    w32_com_set(w, obj, VIEW_FMT, desc ? w32_read(w, desc, 4) : 0);
    return obj;
}
static void dev_CreateRenderTargetView(w32 *w) {
    uint64_t obj = make_view(w, &cls_rtv, ARG(1), ARG(2));
    out_ptr(w, ARG(3), obj);
    RET(obj ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void dev_CreateShaderResourceView(w32 *w) {
    uint64_t obj = make_view(w, &cls_srv, ARG(1), ARG(2));
    out_ptr(w, ARG(3), obj);
    RET(obj ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void dev_CreateDepthStencilView(w32 *w) {
    /* There is no depth buffer -- see the note at the top of the file -- but
     * the view is real, so a program that creates one, binds it and never
     * gets occlusion is in a defined state rather than a failing one. */
    uint64_t obj = make_view(w, &cls_dsv, ARG(1), ARG(2));
    out_ptr(w, ARG(3), obj);
    RET(obj ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}

/* CreateInputLayout(descs, count, shaderBytecode, shaderLen, out). The
 * element array is copied into our own guest allocation: the caller's is
 * usually a local that goes out of scope the moment this returns. */
static void dev_CreateInputLayout(w32 *w) {
    uint64_t descs = ARG(1), out = ARG(5);
    uint32_t count = (uint32_t)ARG(2);
    if (!descs || !count || count > 32) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    int ps = (int)w32_ptrsize(w);
    uint32_t stride = (uint32_t)(ps == 8 ? 32 : 28);
    uint64_t copy = w32_alloc(w, (uint64_t)stride * count + 64, 0);
    if (!copy) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    const void *elems = W32PN(w, descs, (uint64_t)stride * count);
    if (!elems) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    memcpy(W32P(w, copy), elems, (size_t)stride * count);
    /* The semantic names are pointers into the caller's memory, and those
     * strings are almost always literals in the image, which outlive the
     * call. Copying them too would mean rewriting the pointers; pointing at
     * a literal is what every real program does here. */
    uint64_t obj = w32_com_new(w, &cls_layout, LAY_N);
    if (!obj) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    w32_com_set(w, obj, LAY_ELEMS, copy);
    w32_com_set(w, obj, LAY_COUNT, count);
    out_ptr(w, out, obj);
    RET(S_OK_);
}

static void make_shader(w32 *w, w32_com_class *cls, int tag_unused) {
    (void)tag_unused;
    uint64_t blob = ARG(1), len = ARG(2), out = ARG(4);
    uint64_t obj = w32_com_new(w, cls, SH_N);
    if (!obj) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    w32_com_set(w, obj, SH_BLOB, blob);
    w32_com_set(w, obj, SH_LEN, len);
    w32_com_set(w, obj, SH_INFO, (uint64_t)(uint32_t)shader_record(w, blob, len));
    out_ptr(w, out, obj);
    RET(S_OK_);
}
static void dev_CreateVertexShader(w32 *w) { make_shader(w, &cls_vshader, 0); }
static void dev_CreatePixelShader(w32 *w)  { make_shader(w, &cls_pshader, 0); }

/* D3D11_SAMPLER_DESC: Filter, AddressU, AddressV, AddressW, MipLODBias,
 * MaxAnisotropy, ComparisonFunc, BorderColor[4], MinLOD, MaxLOD. */
static void dev_CreateSamplerState(w32 *w) {
    uint64_t desc = ARG(1), out = ARG(2);
    uint64_t obj = w32_com_new(w, &cls_sampler, SMP_N);
    if (!obj) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    if (desc) {
        w32_com_set(w, obj, SMP_FILTER, w32_read(w, desc, 4));
        w32_com_set(w, obj, SMP_ADDRU, w32_read(w, desc + 4, 4));
        w32_com_set(w, obj, SMP_ADDRV, w32_read(w, desc + 8, 4));
    }
    out_ptr(w, out, obj);
    RET(S_OK_);
}

/* D3D11_BLEND_DESC: AlphaToCoverageEnable, IndependentBlendEnable, then
 * eight render-target entries of { BOOL BlendEnable; SrcBlend; DestBlend;
 * BlendOp; SrcBlendAlpha; DestBlendAlpha; BlendOpAlpha; UINT8 WriteMask }.
 * Only the first entry is read: nothing here renders to more than one
 * target at a time. */
static void dev_CreateBlendState(w32 *w) {
    uint64_t desc = ARG(1), out = ARG(2);
    uint64_t obj = w32_com_new(w, &cls_blend, BL_N);
    if (!obj) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    if (desc) {
        uint64_t rt = desc + 8;
        w32_com_set(w, obj, BL_ENABLE, w32_read(w, rt, 4));
        w32_com_set(w, obj, BL_SRC, w32_read(w, rt + 4, 4));
        w32_com_set(w, obj, BL_DST, w32_read(w, rt + 8, 4));
        w32_com_set(w, obj, BL_OP, w32_read(w, rt + 12, 4));
    }
    out_ptr(w, out, obj);
    RET(S_OK_);
}
static void dev_CreateRasterizerState(w32 *w) {
    uint64_t desc = ARG(1), out = ARG(2);
    uint64_t obj = w32_com_new(w, &cls_raster, RS_N);
    if (!obj) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    if (desc) {
        w32_com_set(w, obj, RS_FILL, w32_read(w, desc, 4));
        w32_com_set(w, obj, RS_CULL, w32_read(w, desc + 4, 4));
    }
    out_ptr(w, out, obj);
    RET(S_OK_);
}
static void dev_CreateDepthStencilState(w32 *w) {
    uint64_t obj = w32_com_new(w, &cls_depth, 4);
    out_ptr(w, ARG(2), obj);
    RET(obj ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void dev_CreateQuery(w32 *w) {
    uint64_t obj = w32_com_new(w, &cls_query, 4);
    out_ptr(w, ARG(2), obj);
    RET(obj ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}

static void dev_GetImmediateContext(w32 *w) {
    uint64_t ctx = w32_com_get(w, ARG(0), DEV_CTX);
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), ctx);
    RET(0);
}
static void dev_GetFeatureLevel(w32 *w) { (void)w; RET(0xB000u); }        /* 11_0 */
static void dev_GetCreationFlags(w32 *w) { RET(w32_com_get(w, ARG(0), DEV_FLAGS)); }
static void dev_GetDeviceRemovedReason(w32 *w) { (void)w; RET(S_OK_); }
static void dev_CheckFormatSupport(w32 *w) {
    /* What we can do with a format: use it as a texture, a render target and
     * a vertex buffer element. Saying so honestly lets a program pick a
     * format we can actually read rather than one it assumes. */
    uint32_t fmt = (uint32_t)ARG(1);
    uint32_t support = 0;
    switch (fmt) {
    case FMT_B8G8R8A8_UNORM: case FMT_R8G8B8A8_UNORM: case FMT_R8G8B8A8_UNORM_SRGB:
        support = 0x20 | 0x80 | 0x4000;    /* TEXTURE2D | SHADER_SAMPLE | RENDER_TARGET */
        break;
    case FMT_R32G32B32A32_FLOAT: case FMT_R32G32B32_FLOAT:
    case FMT_R32G32_FLOAT: case FMT_R32_FLOAT: case FMT_R16G16B16A16_FLOAT:
        support = 0x2;                     /* IA_VERTEX_BUFFER */
        break;
    case FMT_R16_UINT: case FMT_R32_UINT:
        support = 0x4;                     /* IA_INDEX_BUFFER */
        break;
    default: support = 0; break;
    }
    if (ARG(2)) w32_write(w, ARG(2), 4, support);
    RET(support ? S_OK_ : (uint64_t)(uint32_t)E_FAIL_);
}
/* CheckFeatureSupport(feature, data, size): what optional capabilities the
 * device has. Zeroing the structure says "none of them", which is true and
 * is what a program needs to hear before it decides whether to use one. */
static void dev_CheckFeatureSupport(w32 *w) {
    uint64_t data = ARG(2);
    uint32_t size = (uint32_t)ARG(3);
    if (data && size && size < 4096) {
        void *p = W32PN(w, data, size);
        if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
        memset(p, 0, size);
    }
    RET(S_OK_);
}
static void dev_CheckMultisampleQualityLevels(w32 *w) {
    /* One sample, no multisampling. A program that asks for four and is told
     * there are no quality levels falls back to one, which is what it would
     * do on hardware that cannot do it either. */
    if (ARG(3)) w32_write(w, ARG(3), 4, (uint32_t)ARG(2) == 1 ? 1 : 0);
    RET(S_OK_);
}
static void dev_SetPrivateData(w32 *w) { (void)w; RET(S_OK_); }
static void dev_GetPrivateData(w32 *w) { if (ARG(2)) w32_write(w, ARG(2), 4, 0); RET((uint64_t)(uint32_t)DXGI_ERROR_NOT_FOUND_); }
static void dev_SetPrivateDataInterface(w32 *w) { (void)w; RET(S_OK_); }
static void dev_SetExceptionMode(w32 *w) { (void)w; RET(S_OK_); }
static void dev_GetExceptionMode(w32 *w) { (void)w; RET(0); }
static void dev_OpenSharedResource(w32 *w) { out_ptr(w, ARG(3), 0); RET((uint64_t)(uint32_t)E_FAIL_); }

/* ----------------------------------------------------- ID3D11DeviceContext
 *
 * The state setters are almost all the same shape: take the first element of
 * an array of interface pointers and remember it. Only the first, and that
 * is a real limit rather than a shortcut -- a draw here reads one vertex
 * buffer, one texture and one constant buffer, because that is what the
 * pipeline below can consume. A game binding a second texture gets the
 * first, which is the same picture for everything that does not sample two.
 */
static uint64_t first_ptr(w32 *w, uint64_t array, uint32_t count) {
    if (!array || !count) return 0;
    return w32_read(w, array, (int)w32_ptrsize(w));
}

static void ctx_IASetVertexBuffers(w32 *w) {
    /* (startSlot, numBuffers, ppBuffers, pStrides, pOffsets) */
    uint64_t self = ARG(0);
    uint32_t n = (uint32_t)ARG(2);
    w32_com_set(w, self, CTX_VB, first_ptr(w, ARG(3), n));
    w32_com_set(w, self, CTX_VB_STRIDE, ARG(4) && n ? w32_read(w, ARG(4), 4) : 0);
    w32_com_set(w, self, CTX_VB_OFFSET, ARG(5) && n ? w32_read(w, ARG(5), 4) : 0);
    RET(0);
}
static void ctx_IASetIndexBuffer(w32 *w) {
    uint64_t self = ARG(0);
    w32_com_set(w, self, CTX_IB, ARG(1));
    w32_com_set(w, self, CTX_IB_FMT, ARG(2));
    w32_com_set(w, self, CTX_IB_OFFSET, ARG(3));
    RET(0);
}
static void ctx_IASetInputLayout(w32 *w) { w32_com_set(w, ARG(0), CTX_LAYOUT, ARG(1)); RET(0); }
static void ctx_IASetPrimitiveTopology(w32 *w) { w32_com_set(w, ARG(0), CTX_TOPOLOGY, ARG(1)); RET(0); }
static void ctx_VSSetShader(w32 *w) { w32_com_set(w, ARG(0), CTX_VS, ARG(1)); RET(0); }
static void ctx_PSSetShader(w32 *w) { w32_com_set(w, ARG(0), CTX_PS, ARG(1)); RET(0); }
/* Four slots of each, which is what a 2D engine's shaders reach for: the
 * matrices in cb0, maybe a second buffer, a texture or two. (start, count,
 * array) -> the fields for slots start..start+count-1. */
static const int VSCB_F[4] = { CTX_VSCB0, CTX_VSCB1, CTX_VSCB2, CTX_VSCB3 }, PSCB_F[4] = { CTX_PSCB0, CTX_PSCB1, CTX_PSCB2, CTX_PSCB3 };
static const int SRV_F[4] = { CTX_SRV0, CTX_SRV1, CTX_SRV2, CTX_SRV3 }, SMP_F[4] = { CTX_SAMPLER0, CTX_SAMPLER1, CTX_SAMPLER2, CTX_SAMPLER3 };
static void set_slots(w32 *w, uint64_t self, const int fields[4], uint32_t start, uint32_t count, uint64_t array) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = start + i;
        if (slot >= 4) break;
        w32_com_set(w, self, fields[slot], array ? w32_read(w, array + (uint64_t)i * w32_ptrsize(w), (int)w32_ptrsize(w)) : 0);
    }
}
static void ctx_VSSetConstantBuffers(w32 *w) { set_slots(w, ARG(0), VSCB_F, (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3)); RET(0); }
static void ctx_PSSetConstantBuffers(w32 *w) { set_slots(w, ARG(0), PSCB_F, (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3)); RET(0); }
static void ctx_PSSetShaderResources(w32 *w) { set_slots(w, ARG(0), SRV_F, (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3)); RET(0); }
static void ctx_PSSetSamplers(w32 *w) { set_slots(w, ARG(0), SMP_F, (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3)); RET(0); }
static void ctx_OMSetRenderTargets(w32 *w) {
    w32_com_set(w, ARG(0), CTX_RTV, first_ptr(w, ARG(2), (uint32_t)ARG(1)));
    RET(0);
}
static void ctx_OMSetBlendState(w32 *w) { w32_com_set(w, ARG(0), CTX_BLEND, ARG(1)); RET(0); }
static void ctx_OMSetDepthStencilState(w32 *w) { (void)w; RET(0); }
static void ctx_RSSetState(w32 *w) { (void)w; RET(0); }
static void ctx_RSSetScissorRects(w32 *w) { (void)w; RET(0); }
/* D3D11_VIEWPORT is five floats: TopLeftX, TopLeftY, Width, Height, MinDepth
 * -- and MaxDepth, six in all. The first four decide where a draw lands. */
static void ctx_RSSetViewports(w32 *w) {
    uint64_t self = ARG(0), vps = ARG(2);
    if (!ARG(1) || !vps) { RET(0); return; }
    w32_com_set(w, self, CTX_VP_X, (uint64_t)(int64_t)rdf(w, vps));
    w32_com_set(w, self, CTX_VP_Y, (uint64_t)(int64_t)rdf(w, vps + 4));
    w32_com_set(w, self, CTX_VP_W, (uint64_t)(int64_t)rdf(w, vps + 8));
    w32_com_set(w, self, CTX_VP_H, (uint64_t)(int64_t)rdf(w, vps + 12));
    RET(0);
}
static void ctx_GetDevice(w32 *w) {
    uint64_t d = w32_com_get(w, ARG(0), CTX_DEV);
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), d);
    RET(0);
}

/* Map / Unmap. A dynamic buffer is written every frame this way, so it is
 * the hottest path in the whole file -- and it costs nothing, because the
 * buffer's bytes are already guest memory and Map is handing back their
 * address. D3D11_MAPPED_SUBRESOURCE is { pData, RowPitch, DepthPitch }. */
static void ctx_Map(w32 *w) {
    uint64_t res = ARG(1), out = ARG(5);
    if (!res || !out) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    int ps = (int)w32_ptrsize(w);
    int tag = w32_com_tag(w, res);
    uint64_t data = 0, pitch = 0, slice = 0;
    if (tag == TAG_D11_BUFFER) {
        data = w32_com_get(w, res, BUF_DATA);
        pitch = slice = w32_com_get(w, res, BUF_SIZE);
    } else if (tag == TAG_D11_TEXTURE) {
        data = w32_com_get(w, res, TEX_PIXELS);
        pitch = w32_com_get(w, res, TEX_PITCH);
        slice = pitch * w32_com_get(w, res, TEX_H);
    } else { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    w32_write(w, out, ps, data);
    w32_write(w, out + ps, 4, pitch);
    w32_write(w, out + ps + 4, 4, slice);
    /* D3D11_MAP_WRITE_DISCARD is 4: the caller promises to write the whole
     * thing and the contents before are undefined. Leaving them alone is a
     * legal choice and the one that costs nothing. */
    RET(S_OK_);
}
static void ctx_Unmap(w32 *w) { (void)w; RET(0); }

/* UpdateSubresource(res, sub, box, data, rowPitch, depthPitch). The box is
 * a region; NULL means the whole thing, which is what a constant-buffer
 * update always passes. */
static void ctx_UpdateSubresource(w32 *w) {
    uint64_t res = ARG(1), box = ARG(3), src = ARG(4);
    uint32_t row_pitch = (uint32_t)ARG(5);
    if (!res || !src) { RET(0); return; }
    int tag = w32_com_tag(w, res);
    if (tag == TAG_D11_BUFFER) {
        uint64_t dst = w32_com_get(w, res, BUF_DATA);
        uint32_t size = (uint32_t)w32_com_get(w, res, BUF_SIZE);
        uint32_t off = 0, n = size;
        if (box) {                                  /* D3D11_BOX: left, top, front, right, bottom, back */
            uint32_t l = (uint32_t)w32_read(w, box, 4), r = (uint32_t)w32_read(w, box + 12, 4);
            if (r > l && r <= size) { off = l; n = r - l; }
        }
        const void *sp = W32PN(w, src, n);
        if (dst && sp) memcpy((uint8_t *)W32P(w, dst) + off, sp, n);
    } else if (tag == TAG_D11_TEXTURE) {
        uint64_t dst = w32_com_get(w, res, TEX_PIXELS);
        uint32_t dpitch = (uint32_t)w32_com_get(w, res, TEX_PITCH);
        int h = (int)w32_com_get(w, res, TEX_H);
        if (!row_pitch) row_pitch = dpitch;
        uint32_t n = dpitch < row_pitch ? dpitch : row_pitch;
        /* The source is h rows a row_pitch apart, of which only the last is
         * shorter than a pitch. UpdateSubresource returns void, so a source
         * that is not there can only be dropped -- there is no status word
         * for the caller to read. */
        const uint8_t *sp = h > 0 ? W32PN(w, src, (uint64_t)row_pitch * (uint64_t)(h - 1) + n) : 0;
        if (dst && sp) for (int y = 0; y < h; y++)
            memcpy((uint8_t *)W32P(w, dst) + (size_t)y * dpitch, sp + (size_t)y * row_pitch, n);
    }
    RET(0);
}

static void ctx_CopyResource(w32 *w) {
    uint64_t dst = ARG(1), src = ARG(2);
    if (w32_com_tag(w, dst) != TAG_D11_TEXTURE || w32_com_tag(w, src) != TAG_D11_TEXTURE) { RET(0); return; }
    uint64_t dp = w32_com_get(w, dst, TEX_PIXELS), sp = w32_com_get(w, src, TEX_PIXELS);
    int dh = (int)w32_com_get(w, dst, TEX_H), sh = (int)w32_com_get(w, src, TEX_H);
    uint32_t dpitch = (uint32_t)w32_com_get(w, dst, TEX_PITCH), spitch = (uint32_t)w32_com_get(w, src, TEX_PITCH);
    int h = dh < sh ? dh : sh;
    uint32_t n = dpitch < spitch ? dpitch : spitch;
    if (dp && sp) for (int y = 0; y < h; y++)
        memcpy((uint8_t *)W32P(w, dp) + (size_t)y * dpitch,
               (const uint8_t *)W32P(w, sp) + (size_t)y * spitch, n);
    RET(0);
}

/* ClearRenderTargetView(view, float rgba[4]). The colour arrives as four
 * floats in the caller's memory, not as a packed word. */
static void ctx_ClearRenderTargetView(w32 *w) {
    uint64_t tex = view_texture(w, ARG(1));
    d3d11_target t;
    if (!tex || !texture_target(w, tex, &t)) { RET(0); return; }
    float c[4] = { rdf(w, ARG(2)), rdf(w, ARG(2) + 4), rdf(w, ARG(2) + 8), rdf(w, ARG(2) + 12) };
    w32_d3d11_clear(&t, pack_color(c));
    RET(0);
}
static void ctx_ClearDepthStencilView(w32 *w) { (void)w; RET(0); }   /* no depth buffer */
static void ctx_ClearState(w32 *w) {
    uint64_t self = ARG(0);
    for (int i = CTX_RTV; i < CTX_DRAWS; i++) w32_com_set(w, self, i, 0);
    RET(0);
}
static void ctx_Flush(w32 *w) { (void)w; RET(0); }
static void ctx_GetType(w32 *w) { (void)w; RET(0); }                 /* IMMEDIATE */
static void ctx_GetContextFlags(w32 *w) { (void)w; RET(0); }
static void ctx_Begin(w32 *w) { (void)w; RET(0); }
static void ctx_End(w32 *w) { (void)w; RET(0); }
static void ctx_GetData(w32 *w) {
    /* A query's result. Nothing here is asynchronous, so every query is
     * finished and its answer is zero -- which for an occlusion query means
     * "nothing was drawn", and is the one answer that could mislead. It is
     * reported rather than left to be discovered. */
    w32_note_refused(w, "d3d11: GetData -- queries always report zero (no occlusion or timing here)");
    if (ARG(2) && ARG(3)) {
        void *p = W32PN(w, ARG(2), ARG(3));
        if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
        memset(p, 0, (size_t)ARG(3));
    }
    RET(S_OK_);
}

/* ------------------------------------------------------------ the draw path
 *
 * Where a vertex comes from, what happens to it, and where it lands.
 */
typedef struct {
    uint64_t elems;          /* the input layout's element array */
    uint32_t nelems;
    uint32_t stride;         /* one element descriptor's size, by bitness */
    uint64_t vb;             /* the vertex buffer's bytes */
    uint32_t vb_size, vb_stride, vb_offset;
    const dxbc_info *vs;
    float matrix[16];
    int has_matrix;
    d3d11_target target;
    d3d11_texture tex;
    int has_tex, wrap, blend;
} draw_state;

/* Find the input-layout element carrying a semantic, and read it from a
 * vertex. This is the join between what the program declared its vertices
 * look like and what the shader said it wanted: the layout gives the offset
 * and format, the signature gives the meaning. */
static int fetch_semantic(w32 *w, const draw_state *d, uint64_t vertex,
                          const char *semantic, uint32_t index, float out[4]) {
    for (uint32_t i = 0; i < d->nelems; i++) {
        uint64_t e = d->elems + (uint64_t)i * d->stride;
        uint64_t name_ptr = w32_read(w, e, (int)w32_ptrsize(w));
        if (!name_ptr) continue;
        const char *name = w32_str(w, name_ptr);
        uint32_t sem_index = (uint32_t)w32_read(w, e + w32_ptrsize(w), 4);
        if (strcasecmp(name, semantic) || sem_index != index) continue;
        uint32_t fmt = (uint32_t)w32_read(w, e + w32_ptrsize(w) + 4, 4);
        uint32_t off = (uint32_t)w32_read(w, e + w32_ptrsize(w) + 12, 4);
        /* D3D11_APPEND_ALIGNED_ELEMENT: the offset is "right after the
         * previous one", so it has to be worked out by walking forward. */
        if (off == 0xFFFFFFFFu) {
            off = 0;
            for (uint32_t k = 0; k < i; k++) {
                uint64_t pe = d->elems + (uint64_t)k * d->stride;
                off += (uint32_t)format_bytes((uint32_t)w32_read(w, pe + w32_ptrsize(w) + 4, 4));
            }
        }
        read_attr(w, vertex + off, fmt, out);
        return 1;
    }
    return 0;
}

/* v * M, with the matrix read as four rows of four floats.
 *
 * Which way round this goes is the one thing here that cannot be derived
 * from the data: it is decided by the shader's own `mul`, and the shader is
 * not run. Row-vector times row-major is the common D3D convention and what
 * a game's world-view-projection matrix is built for. If a game comes out
 * transposed -- everything on screen but mirrored through the diagonal --
 * this is the line, and the symptom is unmistakable rather than subtle. */
static void transform(const float m[16], const float in[4], float out[4]) {
    for (int c = 0; c < 4; c++)
        out[c] = in[0] * m[0 * 4 + c] + in[1] * m[1 * 4 + c]
               + in[2] * m[2 * 4 + c] + in[3] * m[3 * 4 + c];
}

/* One vertex, all the way from its bytes to a pixel position. */
static void build_vertex(w32 *w, const draw_state *d, uint32_t idx, d3d11_vertex *out) {
    memset(out, 0, sizeof *out);
    out->color = 0xFFFFFFFFu;
    uint64_t vertex = d->vb + d->vb_offset + (uint64_t)idx * d->vb_stride;

    float pos[4] = { 0, 0, 0, 1 }, uv[4] = { 0, 0, 0, 1 }, col[4] = { 1, 1, 1, 1 };
    if (!fetch_semantic(w, d, vertex, "POSITION", 0, pos))
        fetch_semantic(w, d, vertex, "SV_Position", 0, pos);
    fetch_semantic(w, d, vertex, "TEXCOORD", 0, uv);
    fetch_semantic(w, d, vertex, "COLOR", 0, col);

    float clip[4];
    if (d->has_matrix) transform(d->matrix, pos, clip);
    else { clip[0] = pos[0]; clip[1] = pos[1]; clip[2] = pos[2]; clip[3] = pos[3]; }

    /* Clip space to the viewport. The perspective divide is here and it is
     * the only division: after this everything is integer. */
    float iw = clip[3] != 0.0f ? 1.0f / clip[3] : 1.0f;
    float ndc_x = clip[0] * iw, ndc_y = clip[1] * iw;
    out->x = (float)d->target.clip_x + (ndc_x * 0.5f + 0.5f) * (float)d->target.clip_w;
    out->y = (float)d->target.clip_y + (0.5f - ndc_y * 0.5f) * (float)d->target.clip_h;
    out->z = clip[2] * iw;
    out->w = clip[3];
    out->u = uv[0];
    out->v = uv[1];
    out->color = pack_color(col);
}

/* ---- running the shaders ------------------------------------------------------
 *
 * What a draw needs beyond the fixed-function state: the two programs, their
 * signatures joined -- the pixel shader's inputs matched to the vertex
 * shader's outputs by semantic, which is how the hardware joins them -- and
 * the constant buffers, textures and samplers each stage can see. */
typedef struct {
    const dxbc_prog *vp, *pp;
    const dxbc_info *vinfo, *pinfo;
    dxbc_env venv, penv;
    d3d11_texture ptex[DXBC_SLOTS];
    int psmap[DXBC_REGS];          /* pixel input register -> vertex output register, or -1 */
    int pos_in;                    /* the pixel input carrying SV_Position, or -1 */
    int vs_pos_out;                /* the vertex output register with SV_Position */
    int ps_out;                    /* the pixel output register for SV_Target0 */
} shade_state;
static int sig_is_position(const dxbc_element *e) { return e->sysvalue == 1 || !strcasecmp(e->name, "SV_Position") || !strcasecmp(e->name, "SV_POSITION"); }
static void env_cbs(w32 *w, uint64_t self, const int fields[4], dxbc_env *env) {
    for (int i = 0; i < 4; i++) {
        uint64_t cb = w32_com_get(w, self, fields[i]);
        if (!cb || w32_com_tag(w, cb) != TAG_D11_BUFFER) continue;
        uint64_t data = w32_com_get(w, cb, BUF_DATA); uint32_t size = (uint32_t)w32_com_get(w, cb, BUF_SIZE);
        const void *p = data && size >= 16 ? W32PN(w, data, size) : 0;
        if (p) { env->cb[i] = p; env->cb_n[i] = size / 16; }
    }
}
static void shade_setup(w32 *w, uint64_t self, shade_state *sh, const dxbc_prog *vp, const dxbc_prog *pp, const dxbc_info *vi, const dxbc_info *pi) {
    sh->vp = vp; sh->pp = pp; sh->vinfo = vi; sh->pinfo = pi;
    env_cbs(w, self, VSCB_F, &sh->venv);
    env_cbs(w, self, PSCB_F, &sh->penv);
    for (int i = 0; i < 4; i++) {
        uint64_t stex = view_texture(w, w32_com_get(w, self, SRV_F[i]));
        if (stex) {
            d3d11_texture *t = &sh->ptex[i];
            t->w = (int)w32_com_get(w, stex, TEX_W); t->h = (int)w32_com_get(w, stex, TEX_H);
            t->pitch_px = (int)(w32_com_get(w, stex, TEX_PITCH) / 4);
            t->pixels = W32P(w, w32_com_get(w, stex, TEX_PIXELS));
            if (t->pixels && t->w > 0 && t->h > 0) sh->penv.tex[i] = t;
        }
        uint64_t smp = w32_com_get(w, self, SMP_F[i]);
        sh->penv.wrap[i] = smp ? (w32_com_get(w, smp, SMP_ADDRU) == 1) : 1;
        /* D3D11_FILTER: bit 2 set means the magnification filter is linear */
        sh->penv.linear[i] = smp ? ((w32_com_get(w, smp, SMP_FILTER) & 0x4) != 0) : 0;
    }
    sh->vs_pos_out = 0; sh->pos_in = -1; sh->ps_out = 0;
    if (vi) for (int k = 0; k < vi->output.n; k++) if (sig_is_position(&vi->output.e[k])) sh->vs_pos_out = (int)vi->output.e[k].reg;
    for (int r = 0; r < DXBC_REGS; r++) sh->psmap[r] = -1;
    if (pi && vi) for (int k = 0; k < pi->input.n; k++) {
        const dxbc_element *e = &pi->input.e[k];
        if (e->reg >= DXBC_REGS) continue;
        if (sig_is_position(e)) { sh->pos_in = (int)e->reg; continue; }
        const dxbc_element *src = dxbc_find(&vi->output, e->name, e->index);
        if (src && src->reg < D3D11_MAX_VARY) sh->psmap[e->reg] = (int)src->reg;
    }
    if (pi) for (int k = 0; k < pi->output.n; k++) if (!strcasecmp(pi->output.e[k].name, "SV_Target") && pi->output.e[k].index == 0) sh->ps_out = (int)pi->output.e[k].reg;
}
/* One vertex through the vertex shader to a screen position and its varyings. */
static int shade_vertex(w32 *w, const draw_state *d, const shade_state *sh, uint32_t idx, d3d11_svertex *out) {
    static float vin[DXBC_REGS][4], vout[DXBC_REGS][4];
    memset(vin, 0, sizeof vin);
    uint64_t vertex = d->vb + d->vb_offset + (uint64_t)idx * d->vb_stride;
    if (sh->vinfo) for (int k = 0; k < sh->vinfo->input.n; k++) {
        const dxbc_element *e = &sh->vinfo->input.e[k];
        if (e->reg >= DXBC_REGS) continue;
        if (!strcasecmp(e->name, "SV_VertexID")) { uint32_t u = idx; memcpy(&vin[e->reg][0], &u, 4); continue; }
        if (!strcasecmp(e->name, "SV_InstanceID")) continue;
        float val[4] = { 0, 0, 0, 1 };
        fetch_semantic(w, d, vertex, e->name, e->index, val);
        memcpy(vin[e->reg], val, sizeof val);
    }
    if (dxbc_exec(sh->vp, &sh->venv, vin, vout) < 0) return 0;
    const float *clip = vout[sh->vs_pos_out];
    float iw = clip[3] != 0.0f ? 1.0f / clip[3] : 1.0f;
    memset(out, 0, sizeof *out);
    out->x = (float)d->target.clip_x + (clip[0] * iw * 0.5f + 0.5f) * (float)d->target.clip_w;
    out->y = (float)d->target.clip_y + (0.5f - clip[1] * iw * 0.5f) * (float)d->target.clip_h;
    out->z = clip[2] * iw;
    out->w = clip[3] != 0.0f ? clip[3] : 1.0f;
    for (int k = 0; k < D3D11_MAX_VARY; k++) memcpy(out->var[k], vout[k], sizeof out->var[k]);
    return 1;
}
/* One pixel through the pixel shader. */
static uint32_t shade_pixel(void *ctx, const float var[D3D11_MAX_VARY][4], float px, float py, float z, int *discard) {
    const shade_state *sh = ctx;
    static _Thread_local float pin[DXBC_REGS][4], pout[DXBC_REGS][4];   /* pixels are shaded on several threads */
    memset(pin, 0, sizeof pin);
    for (int r = 0; r < DXBC_REGS; r++) if (sh->psmap[r] >= 0) memcpy(pin[r], var[sh->psmap[r]], 16);
    if (sh->pos_in >= 0) { pin[sh->pos_in][0] = px; pin[sh->pos_in][1] = py; pin[sh->pos_in][2] = z; pin[sh->pos_in][3] = 1.0f; }
    int r = dxbc_exec(sh->pp, &sh->penv, pin, pout);
    if (r) { *discard = 1; return 0; }
    const float *c = pout[sh->ps_out];
    uint32_t ch[4];
    for (int i = 0; i < 4; i++) { float v = c[i]; if (v != v || v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; ch[i] = (uint32_t)(v * 255.0f + 0.5f); }
    return (ch[3] << 24) | (ch[0] << 16) | (ch[1] << 8) | ch[2];
}

/* Assemble and draw. `base` is the first index; `indices` non-zero means the
 * indexed form, in which case `ib` is the buffer and `ifmt` its element
 * size. */
static void draw_common(w32 *w, uint64_t self, uint32_t count, uint32_t start,
                        int indexed, int base_vertex) {
    if (!count) return;
    uint64_t rtv = w32_com_get(w, self, CTX_RTV);
    uint64_t tex = view_texture(w, rtv);
    draw_state d;
    memset(&d, 0, sizeof d);
    if (!tex || !texture_target(w, tex, &d.target)) return;

    /* The viewport clips the draw. A zero one means the program never set
     * it, which on real hardware draws nothing -- but a program that
     * forgets is far more likely than a program that means it, so the whole
     * target is used and the report says so once. */
    int vx = (int)(int64_t)w32_com_get(w, self, CTX_VP_X);
    int vy = (int)(int64_t)w32_com_get(w, self, CTX_VP_Y);
    int vw = (int)(int64_t)w32_com_get(w, self, CTX_VP_W);
    int vh = (int)(int64_t)w32_com_get(w, self, CTX_VP_H);
    if (vw > 0 && vh > 0) {
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;
        if (vx + vw > d.target.w) vw = d.target.w - vx;
        if (vy + vh > d.target.h) vh = d.target.h - vy;
        if (vw > 0 && vh > 0) {
            d.target.clip_x = vx; d.target.clip_y = vy;
            d.target.clip_w = vw; d.target.clip_h = vh;
        }
    } else {
        w32_note_refused(w, "d3d11: a draw with no viewport set -- using the whole target");
    }

    uint64_t layout = w32_com_get(w, self, CTX_LAYOUT);
    uint64_t vb = w32_com_get(w, self, CTX_VB);
    if (!layout || !vb) return;
    d.elems = w32_com_get(w, layout, LAY_ELEMS);
    d.nelems = (uint32_t)w32_com_get(w, layout, LAY_COUNT);
    d.stride = (uint32_t)(w32_ptrsize(w) == 8 ? 32 : 28);
    d.vb = w32_com_get(w, vb, BUF_DATA);
    d.vb_size = (uint32_t)w32_com_get(w, vb, BUF_SIZE);
    d.vb_stride = (uint32_t)w32_com_get(w, self, CTX_VB_STRIDE);
    d.vb_offset = (uint32_t)w32_com_get(w, self, CTX_VB_OFFSET);
    if (!d.vb || !d.vb_stride) return;

    uint64_t vs = w32_com_get(w, self, CTX_VS), ps = w32_com_get(w, self, CTX_PS);
    int vs_i = vs ? (int)(int32_t)(uint32_t)w32_com_get(w, vs, SH_INFO) : -1;
    int ps_i = ps ? (int)(int32_t)(uint32_t)w32_com_get(w, ps, SH_INFO) : -1;
    if (vs) d.vs = shader_info(vs_i);
    /* Both stages have code that decoded: the shaders run. Otherwise the
     * fixed-function reading below, and the report says which shader and why. */
    const dxbc_prog *vprog = vs_i >= 0 && vs_i < MAX_SHADERS ? g_progs[vs_i] : 0;
    const dxbc_prog *pprog = ps_i >= 0 && ps_i < MAX_SHADERS ? g_progs[ps_i] : 0;
    int shaded = vprog && pprog && dxbc_prog_stage(vprog) == 1 && dxbc_prog_stage(pprog) == 0;
    if (!shaded) { if (vs) note_shader_not_run(w, vs_i); if (ps) note_shader_not_run(w, ps_i); }
    shade_state sh; memset(&sh, 0, sizeof sh);
    if (shaded) shade_setup(w, self, &sh, vprog, pprog, shader_info(vs_i), shader_info(ps_i));

    /* The transform, if the program bound a constant buffer big enough to
     * hold one. Sixteen floats at the start of the first vertex constant
     * buffer is where a world-view-projection matrix lives in every engine
     * that has one. */
    uint64_t cb = w32_com_get(w, self, CTX_VSCB0);
    if (cb && w32_com_tag(w, cb) == TAG_D11_BUFFER) {
        uint64_t data = w32_com_get(w, cb, BUF_DATA);
        uint32_t size = (uint32_t)w32_com_get(w, cb, BUF_SIZE);
        if (data && size >= 64) {
            for (int i = 0; i < 16; i++) d.matrix[i] = rdf(w, data + (unsigned)i * 4);
            d.has_matrix = 1;
        }
    }

    /* The texture and how to sample it. */
    uint64_t srv = w32_com_get(w, self, CTX_SRV0);
    uint64_t stex = view_texture(w, srv);
    if (stex) {
        d.tex.w = (int)w32_com_get(w, stex, TEX_W);
        d.tex.h = (int)w32_com_get(w, stex, TEX_H);
        d.tex.pitch_px = (int)(w32_com_get(w, stex, TEX_PITCH) / 4);
        d.tex.pixels = W32P(w, w32_com_get(w, stex, TEX_PIXELS));
        d.has_tex = d.tex.pixels && d.tex.w > 0 && d.tex.h > 0;
    }
    uint64_t smp = w32_com_get(w, self, CTX_SAMPLER0);
    /* D3D11_TEXTURE_ADDRESS_WRAP is 1; CLAMP is 3. */
    d.wrap = smp ? (w32_com_get(w, smp, SMP_ADDRU) == 1) : 1;

    /* Blending. SRC_ALPHA(5) over INV_SRC_ALPHA(6) is the usual pair and is
     * what "over" means; ONE(2) with ONE(2) is additive. Anything else, and
     * anything with blending disabled, writes the source. */
    uint64_t bl = w32_com_get(w, self, CTX_BLEND);
    d.blend = D3D11_BLEND_OVER_;
    if (bl) {
        if (!w32_com_get(w, bl, BL_ENABLE)) d.blend = D3D11_BLEND_NONE_;
        else {
            uint32_t src = (uint32_t)w32_com_get(w, bl, BL_SRC);
            uint32_t dst = (uint32_t)w32_com_get(w, bl, BL_DST);
            if (src == 2 && dst == 2) d.blend = D3D11_BLEND_ADD_;
            else if (src == 2 && dst == 1) d.blend = D3D11_BLEND_NONE_;
            else d.blend = D3D11_BLEND_OVER_;
        }
    }

    uint64_t ib = 0, ibdata = 0;
    uint32_t ifmt = 0, iboff = 0, ibsize = 0;
    if (indexed) {
        ib = w32_com_get(w, self, CTX_IB);
        if (!ib) return;
        ibdata = w32_com_get(w, ib, BUF_DATA);
        ibsize = (uint32_t)w32_com_get(w, ib, BUF_SIZE);
        ifmt = (uint32_t)w32_com_get(w, self, CTX_IB_FMT);
        iboff = (uint32_t)w32_com_get(w, self, CTX_IB_OFFSET);
        if (!ibdata) return;
    }

    uint32_t topology = (uint32_t)w32_com_get(w, self, CTX_TOPOLOGY);
    /* 4 is TRIANGLELIST, 5 TRIANGLESTRIP. A topology this cannot assemble
     * is reported rather than drawn as something else. */
    if (topology != 4 && topology != 5) {
        if (topology == 1 || topology == 2 || topology == 3)
            w32_note_refused(w, "d3d11: point and line topologies are not drawn");
        else if (topology)
            w32_note_refused(w, "d3d11: an unsupported primitive topology was not drawn");
        if (topology != 0) return;
        topology = 4;                             /* unset: a list is the safe reading */
    }

    uint32_t index_bytes = (ifmt == FMT_R32_UINT) ? 4 : 2;
    #define INDEX_AT(k) (indexed \
        ? (uint32_t)((iboff + (uint64_t)(k) * index_bytes + index_bytes <= ibsize) \
            ? w32_read(w, ibdata + iboff + (uint64_t)(k) * index_bytes, (int)index_bytes) : 0) \
            + (uint32_t)base_vertex \
        : (uint32_t)(k))

    /* Shaded triangles are gathered and handed to the rasterizer a draw at
     * a time, so it can spread them over the cores. */
    static d3d11_svertex *batch; static int batch_cap;
    int nb = 0;
    enum { BATCH_TRIS = 4096 };
    if (shaded && !batch) { batch_cap = 3 * BATCH_TRIS; batch = malloc((size_t)batch_cap * sizeof *batch); if (!batch) return; }

    uint32_t tris = topology == 4 ? count / 3 : (count >= 3 ? count - 2 : 0);
    for (uint32_t t = 0; t < tris; t++) {
        uint32_t a, b, c;
        if (topology == 4) { a = start + t * 3; b = a + 1; c = a + 2; }
        else {
            /* A strip alternates winding so that every triangle faces the
             * same way. Culling is off here, so the order only matters for
             * anything that later cares about facing -- and getting it right
             * costs one swap. */
            a = start + t; b = a + 1; c = a + 2;
            if (t & 1) { uint32_t tmp = b; b = c; c = tmp; }
        }
        uint32_t ia = INDEX_AT(a), ib2 = INDEX_AT(b), ic = INDEX_AT(c);
        /* A vertex outside the buffer is a bug in the program or in our
         * reading of its stride, and either way it must not read past the
         * allocation. */
        uint64_t last = (uint64_t)d.vb_offset + (uint64_t)(ia > ib2 ? (ia > ic ? ia : ic) : (ib2 > ic ? ib2 : ic)) * d.vb_stride;
        if (last + d.vb_stride > d.vb_size) continue;
        if (shaded) {
            d3d11_svertex *s0 = &batch[nb], *s1 = &batch[nb + 1], *s2 = &batch[nb + 2];
            if (!shade_vertex(w, &d, &sh, ia, s0) || !shade_vertex(w, &d, &sh, ib2, s1) || !shade_vertex(w, &d, &sh, ic, s2)) continue;
            nb += 3;
            if (nb == batch_cap) { w32_d3d11_triangles_shaded(&d.target, batch, nb / 3, shade_pixel, &sh, d.blend); nb = 0; }
            continue;
        }
        d3d11_vertex v0, v1, v2;
        build_vertex(w, &d, ia, &v0);
        build_vertex(w, &d, ib2, &v1);
        build_vertex(w, &d, ic, &v2);
        w32_d3d11_triangle(&d.target, &v0, &v1, &v2,
                           d.has_tex ? &d.tex : 0, d.wrap, d.blend);
    }
    #undef INDEX_AT
    if (nb) w32_d3d11_triangles_shaded(&d.target, batch, nb / 3, shade_pixel, &sh, d.blend);
    w32_com_set(w, self, CTX_DRAWS, w32_com_get(w, self, CTX_DRAWS) + 1);
}

static void ctx_Draw(w32 *w) {
    draw_common(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), 0, 0);
    RET(0);
}
static void ctx_DrawIndexed(w32 *w) {
    draw_common(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), 1, (int)(int32_t)(uint32_t)ARG(3));
    RET(0);
}
static void ctx_DrawInstanced(w32 *w) {
    /* Instancing draws the same vertices many times with per-instance data
     * this pipeline does not fetch, so drawing it once is the first instance
     * and no more. Reported, because a game that instances its particles
     * would otherwise show one of them and no error. */
    w32_note_refused(w, "d3d11: instanced draws render only the first instance");
    draw_common(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(3), 0, 0);
    RET(0);
}
static void ctx_DrawIndexedInstanced(w32 *w) {
    w32_note_refused(w, "d3d11: instanced draws render only the first instance");
    draw_common(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(3), 1, (int)(int32_t)(uint32_t)ARG(4));
    RET(0);
}

/* --------------------------------------------------------- IDXGISwapChain
 *
 * The back buffer, and the one call that puts it on screen. Present hands
 * the pixels to the same callback every other frame in this project goes
 * through -- the one the iOS view attaches and `winrun -frame` captures --
 * so a D3D11 game reaches the display by exactly the path a D3D9 one does.
 */
static void sc_GetBuffer(w32 *w) {
    /* (index, riid, out). Only buffer 0 exists: there is one back buffer,
     * and a program asking for a second in a flip chain gets told so. */
    uint64_t self = ARG(0), out = ARG(3);
    if (ARG(1) != 0) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)DXGI_ERROR_INVALID_CALL_); return; }
    uint64_t tex = w32_com_get(w, self, SC_TEX);
    /* GetBuffer hands out a reference the caller will release. */
    if (tex) w32_write(w, tex + w32_ptrsize(w), 4, (uint32_t)w32_read(w, tex + w32_ptrsize(w), 4) + 1);
    out_ptr(w, out, tex);
    RET(tex ? S_OK_ : (uint64_t)(uint32_t)E_FAIL_);
}
static void sc_Present(w32 *w) {
    uint64_t self = ARG(0);
    uint64_t tex = w32_com_get(w, self, SC_TEX);
    if (!tex) { RET((uint64_t)(uint32_t)DXGI_ERROR_INVALID_CALL_); return; }
    /* The same "the window went away, stop drawing" signal d3d9 uses, so
     * closing the screen ends a D3D11 guest's loop the same way. */
    if (w32_d3d9_lost()) { RET((uint64_t)(uint32_t)DXGI_ERROR_DEVICE_REMOVED_); return; }
    int width = (int)w32_com_get(w, tex, TEX_W), h = (int)w32_com_get(w, tex, TEX_H);
    int pitch = (int)w32_com_get(w, tex, TEX_PITCH);
    uint64_t px = w32_com_get(w, tex, TEX_PIXELS);
    w32_com_set(w, self, SC_FRAMES, w32_com_get(w, self, SC_FRAMES) + 1);
    w32_note_activity();          /* a frame is not idling */
    w32_frame_presented(w);
    void *ctx = 0;
    w32_present_fn fn = w32_get_present(&ctx);
    if (fn && px) fn(ctx, W32P(w, px), width, h, pitch);
    RET(S_OK_);
}
static void sc_GetDesc(w32 *w) {
    /* DXGI_SWAP_CHAIN_DESC: a DXGI_MODE_DESC (width, height, refresh
     * numerator and denominator, format, scanline order, scaling), then
     * SampleDesc, BufferUsage, BufferCount, OutputWindow, Windowed,
     * SwapEffect, Flags. */
    uint64_t self = ARG(0), d = ARG(1);
    if (!d) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    /* 60 bytes at 32 bits, 72 at 64: the HWND in the middle is pointer-sized
     * and everything after it moves. */
    void *p = W32PN(w, d, w->is32 ? 60 : 72);
    if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    memset(p, 0, w->is32 ? 60 : 72);
    w32_write(w, d, 4, w32_com_get(w, self, SC_W));
    w32_write(w, d + 4, 4, w32_com_get(w, self, SC_H));
    w32_write(w, d + 8, 4, 60);
    w32_write(w, d + 12, 4, 1);
    w32_write(w, d + 16, 4, FMT_B8G8R8A8_UNORM);
    w32_write(w, d + 28, 4, 1);                    /* SampleDesc.Count */
    RET(S_OK_);
}
static void sc_ResizeBuffers(w32 *w) {
    /* (count, width, height, format, flags). A window here does not change
     * size while a game runs -- the display mode is fixed before it starts
     * -- so this is accepted and the buffer kept, which is what a program
     * resizing to the size it already has expects. */
    uint64_t self = ARG(0);
    uint32_t nw = (uint32_t)ARG(2), nh = (uint32_t)ARG(3);
    if (nw && nh && (nw != w32_com_get(w, self, SC_W) || nh != w32_com_get(w, self, SC_H)))
        w32_note_refused(w, "d3d11: ResizeBuffers to a different size is not supported");
    RET(S_OK_);
}
static void sc_SetFullscreenState(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetFullscreenState(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, 0);
    if (ARG(2)) w32_write(w, ARG(2), (int)w32_ptrsize(w), 0);
    RET(S_OK_);
}
static void sc_GetLastPresentCount(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, w32_com_get(w, ARG(0), SC_FRAMES));
    RET(S_OK_);
}
/* IDXGISwapChain1, the DXGI 1.2 revision a Windows 8+ program creates with
 * CreateSwapChainForHwnd and then queries. Its methods follow the 1.0 ones
 * in the same vtable, so a program that asked for the newer interface calls
 * slot 18 onward and lands here rather than off the end of the table. */
static void sc_GetDesc1(w32 *w) {
    /* DXGI_SWAP_CHAIN_DESC1: Width, Height, Format, Stereo, SampleDesc,
     * BufferUsage, BufferCount, Scaling, SwapEffect, AlphaMode, Flags -- 48
     * bytes, no pointer, so one size for both bitnesses. */
    uint64_t self = ARG(0), d = ARG(1);
    void *p = d ? W32PN(w, d, 48) : 0;
    if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    memset(p, 0, 48);
    w32_write(w, d, 4, w32_com_get(w, self, SC_W));
    w32_write(w, d + 4, 4, w32_com_get(w, self, SC_H));
    w32_write(w, d + 8, 4, FMT_B8G8R8A8_UNORM);
    w32_write(w, d + 16, 4, 1);                    /* SampleDesc.Count */
    w32_write(w, d + 24, 4, 0x20);                 /* DXGI_USAGE_RENDER_TARGET_OUTPUT */
    w32_write(w, d + 28, 4, 1);                    /* BufferCount */
    RET(S_OK_);
}
static void sc_GetFullscreenDesc(w32 *w) {
    /* DXGI_SWAP_CHAIN_FULLSCREEN_DESC: RefreshRate {60, 1}, ScanlineOrdering,
     * Scaling, Windowed -- 20 bytes. Windowed, always: the screen is the window. */
    uint64_t d = ARG(1);
    void *p = d ? W32PN(w, d, 20) : 0;
    if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    memset(p, 0, 20);
    w32_write(w, d, 4, 60); w32_write(w, d + 4, 4, 1); w32_write(w, d + 16, 4, 1);
    RET(S_OK_);
}
static void sc_GetHwnd(w32 *w) {
    uint64_t h = w32_com_get(w, ARG(0), SC_HWND);
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), h);
    RET(h ? S_OK_ : (uint64_t)(uint32_t)DXGI_ERROR_INVALID_CALL_);
}
static void sc_GetCoreWindow(w32 *w) { out_ptr(w, ARG(2), 0); RET((uint64_t)(uint32_t)E_NOINTERFACE_); }
static void sc_IsTemporaryMonoSupported(w32 *w) { (void)w; RET(0); }
static void sc_GetRestrictToOutput(w32 *w) { out_ptr(w, ARG(1), 0); RET(S_OK_); }
static void sc_SetBackgroundColor(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetBackgroundColor(w32 *w) {
    void *p = ARG(1) ? W32PN(w, ARG(1), 16) : 0;
    if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    memset(p, 0, 16);
    RET(S_OK_);
}
static void sc_SetRotation(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetRotation(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 1); RET(S_OK_); }   /* DXGI_MODE_ROTATION_IDENTITY */
/* IDXGISwapChain2..4: the source size, frame latency and its waitable object,
 * the presentation transform, the back buffer index, colour spaces, HDR. */
static void sc_SetSourceSize(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetSourceSize(w32 *w) {
    uint64_t self = ARG(0);
    if (ARG(1)) w32_write(w, ARG(1), 4, w32_com_get(w, self, SC_W));
    if (ARG(2)) w32_write(w, ARG(2), 4, w32_com_get(w, self, SC_H));
    RET(S_OK_);
}
static void sc_SetMaximumFrameLatency(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetMaximumFrameLatency(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 1); RET(S_OK_); }
static void sc_GetFrameLatencyWaitableObject(w32 *w) {
    /* A program made with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
     * waits on this before each frame: an event that is always set, because
     * the GPU is never behind. */
    uint64_t self = ARG(0);
    uint64_t h = w32_com_get(w, self, SC_WAITABLE);
    if (!h) { h = w32_handle_new(w, H_EVENT, -1); w32_handle *hh = w32_handle_get(w, h); if (hh) hh->flags = 1u | 2u; w32_com_set(w, self, SC_WAITABLE, h); }
    RET(h);
}
static void sc_SetMatrixTransform(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetMatrixTransform(w32 *w) {
    /* DXGI_MATRIX_3X2_F: the identity */
    uint64_t m = ARG(1);
    if (!m || !w32_mem_ok(w, m, 24)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    for (int i = 0; i < 6; i++) w32_write(w, m + 4u * i, 4, (i == 0 || i == 3) ? 0x3F800000u : 0);
    RET(S_OK_);
}
static void sc_GetCurrentBackBufferIndex(w32 *w) { (void)w; RET(0); }
static void sc_CheckColorSpaceSupport(w32 *w) {
    /* (colorSpace, out): sRGB is supported and present-able; nothing else */
    if (ARG(2)) w32_write(w, ARG(2), 4, ARG(1) == 0 ? 0x1 | 0x2 : 0);
    RET(S_OK_);
}
static void sc_SetColorSpace1(w32 *w) { RET(ARG(1) == 0 ? S_OK_ : (uint64_t)(uint32_t)E_INVALIDARG_); }
static void sc_ResizeBuffers1(w32 *w) {
    /* (count, width, height, format, flags, nodeMasks, queues): the same
     * answer as ResizeBuffers -- the size is fixed */
    uint64_t self = ARG(0);
    uint32_t nw = (uint32_t)ARG(2), nh = (uint32_t)ARG(3);
    if (nw && nh && (nw != w32_com_get(w, self, SC_W) || nh != w32_com_get(w, self, SC_H)))
        w32_note_refused(w, "d3d11: ResizeBuffers1 to a different size is not supported");
    RET(S_OK_);
}
static void sc_SetHDRMetaData(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetFrameStatistics(w32 *w) {
    /* DXGI_FRAME_STATISTICS: three counts, then two 64-bit times -- 32 bytes
     * in both bitnesses. */
    if (ARG(1)) {
        void *p = W32PN(w, ARG(1), 32);
        if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
        memset(p, 0, 32);
    }
    RET(S_OK_);
}
static void sc_GetContainingOutput(w32 *w) { out_ptr(w, ARG(1), 0); RET((uint64_t)(uint32_t)DXGI_ERROR_NOT_FOUND_); }
static void sc_ResizeTarget(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetDevice(w32 *w) {
    uint64_t d = w32_com_get(w, ARG(0), SC_DEV);
    if (ARG(2)) w32_write(w, ARG(2), (int)w32_ptrsize(w), d);
    RET(d ? S_OK_ : (uint64_t)(uint32_t)E_NOINTERFACE_);
}
/* GetParent(riid, out). What a program is asking for is the object one up
 * the DXGI tree: the factory above an adapter or a swap chain, the adapter
 * above the DXGI side of a device. The factory itself has none.
 *
 * This used to answer E_NOINTERFACE for everything, and a GameMaker runner
 * takes that as "Direct3D is broken": it asks its adapter for the factory
 * before it will create a swap chain, formats the HRESULT into a message
 * box, and exits. The interface identifier is not checked because every
 * factory revision shares one prefix layout, and so does every adapter. */
static void obj_GetParent(w32 *w) {
    uint64_t self = ARG(0), out = ARG(2);
    int tag = w32_com_tag(w, self);
    uint64_t p = 0;
    if (tag == TAG_DXGI_DEVICE) p = w32_com_new(w, &cls_adapter, 4);
    else if (tag != TAG_DXGI_FACTORY) p = w32_com_new(w, &cls_factory, 4);
    out_ptr(w, out, p);
    RET(p ? S_OK_ : (uint64_t)(uint32_t)E_NOINTERFACE_);
}
static void obj_SetPrivateData(w32 *w) { (void)w; RET(S_OK_); }
static void obj_GetPrivateData(w32 *w) { RET((uint64_t)(uint32_t)DXGI_ERROR_NOT_FOUND_); }

/* ------------------------------------------------------------- resources */

static void res_GetType(w32 *w) {
    /* D3D11_RESOURCE_DIMENSION: 1 buffer, 3 texture2d. */
    if (ARG(1)) w32_write(w, ARG(1), 4, w32_com_tag(w, ARG(0)) == TAG_D11_BUFFER ? 1 : 3);
    RET(0);
}
static void res_SetEvictionPriority(w32 *w) { (void)w; RET(0); }
static void res_GetEvictionPriority(w32 *w) { (void)w; RET(0); }
static void child_GetDevice(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), 0); RET(0); }

static void tex_GetDesc(w32 *w) {
    uint64_t self = ARG(0), d = ARG(1);
    /* D3D11_TEXTURE2D_DESC is 44 bytes with no pointers in it, so it is the
     * same size in both bitnesses. GetDesc returns void: a description that
     * cannot be delivered is simply not delivered. */
    void *p = W32PN(w, d, 44);
    if (!p) { RET(0); return; }
    memset(p, 0, 44);
    w32_write(w, d, 4, w32_com_get(w, self, TEX_W));
    w32_write(w, d + 4, 4, w32_com_get(w, self, TEX_H));
    w32_write(w, d + 8, 4, 1);                     /* MipLevels */
    w32_write(w, d + 12, 4, 1);                    /* ArraySize */
    w32_write(w, d + 16, 4, w32_com_get(w, self, TEX_FMT));
    w32_write(w, d + 20, 4, 1);                    /* SampleDesc.Count */
    w32_write(w, d + 28, 4, w32_com_get(w, self, TEX_USAGE));
    w32_write(w, d + 32, 4, w32_com_get(w, self, TEX_BIND));
    RET(0);
}
static void buf_GetDesc(w32 *w) {
    uint64_t self = ARG(0), d = ARG(1);
    if (!d) { RET(0); return; }
    w32_write(w, d, 4, w32_com_get(w, self, BUF_SIZE));
    w32_write(w, d + 4, 4, w32_com_get(w, self, BUF_USAGE));
    w32_write(w, d + 8, 4, w32_com_get(w, self, BUF_BIND));
    w32_write(w, d + 12, 4, 0);
    w32_write(w, d + 16, 4, 0);
    w32_write(w, d + 20, 4, w32_com_get(w, self, BUF_STRIDE));
    RET(0);
}
static void view_GetResource(w32 *w) {
    uint64_t r = w32_com_get(w, ARG(0), VIEW_RES);
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), r);
    RET(0);
}
static void state_GetDesc(w32 *w) {
    /* A caller reading back a state it created. Zeroing the structure is
     * wrong in detail and right in shape; nothing here reads one back to
     * make a decision, and a state object is opaque by design. */
    (void)w;
    RET(0);
}

/* ------------------------------------------------------------------ DXGI */

static void fac_CreateSwapChain(w32 *w);
static void fac_CreateSwapChainForHwnd(w32 *w);
static void fac_CreateSwapChainForCoreWindow(w32 *w);
static void fac_CreateSwapChainForComposition(w32 *w);
static void fac_IsWindowedStereoEnabled(w32 *w);
static void fac_GetSharedResourceAdapterLuid(w32 *w);
static void fac_RegisterStatusWindow(w32 *w);
static void fac_RegisterStatusEvent(w32 *w);
static void fac_UnregisterStatus(w32 *w);
static void fac_GetCreationFlags(w32 *w);
static void fac_EnumAdapterByLuid(w32 *w);
static void fac_EnumWarpAdapter(w32 *w);
static void fac_CheckFeatureSupport(w32 *w);
static void fac_EnumAdapterByGpuPreference(w32 *w);
static void fac_RegisterAdaptersChangedEvent(w32 *w);
static void fac_UnregisterAdaptersChangedEvent(w32 *w);
static uint64_t make_swapchain(w32 *w, uint64_t device, int width, int height);

static void fac_EnumAdapters(w32 *w) {
    if (ARG(1) != 0) { out_ptr(w, ARG(2), 0); RET((uint64_t)(uint32_t)DXGI_ERROR_NOT_FOUND_); return; }
    uint64_t a = w32_com_new(w, &cls_adapter, 4);
    out_ptr(w, ARG(2), a);
    RET(a ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_MakeWindowAssociation(w32 *w) { (void)w; RET(S_OK_); }
static void fac_GetWindowAssociation(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), 0); RET(S_OK_); }
static void fac_IsCurrent(w32 *w) { (void)w; RET(1); }

static void adp_EnumOutputs(w32 *w) { out_ptr(w, ARG(2), 0); RET((uint64_t)(uint32_t)DXGI_ERROR_NOT_FOUND_); }
/* DXGI_ADAPTER_DESC: a 128-character description, then vendor, device,
 * subsystem and revision ids, then three SIZE_Ts of memory and a LUID. The
 * name is the honest one. DESC1 adds a Flags word; DESC2 two more enums.
 *
 * The size is measured, not computed. A structure ending in pointer-sized
 * members is not the same size in the two bitnesses, and clearing more
 * than it holds writes past the end of a caller's local -- which is a
 * smashed stack frame that faults somewhere else entirely, long after
 * this function has returned successfully. It cost an afternoon once.
 *
 * The memory figures are not zero: a program that divides by its video
 * memory, or refuses to start with none, is more common than one that
 * checks the number is true. A quarter of a gigabyte dedicated and one
 * shared is what a modest laptop reports. */
static void adapter_desc(w32 *w, int extra) {
    uint64_t d = ARG(1);
    int psz = (int)w32_ptrsize(w), size = (w->is32 ? 292 : 304) + extra;
    if (!d) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    void *p = W32PN(w, d, (uint64_t)size);
    if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    memset(p, 0, (size_t)size);
    static const char *name = "Winios software renderer";
    for (int i = 0; name[i]; i++) w32_write(w, d + (unsigned)i * 2, 2, (uint8_t)name[i]);
    w32_write(w, d + 272, psz, 256u << 20);                           /* DedicatedVideoMemory */
    w32_write(w, d + 272 + 2 * (unsigned)psz, psz, 1024u << 20);      /* SharedSystemMemory */
    RET(S_OK_);
}
static void adp_GetDesc(w32 *w)  { adapter_desc(w, 0); }
static void adp_GetDesc1(w32 *w) { adapter_desc(w, 4); }
static void adp_GetDesc2(w32 *w) { adapter_desc(w, 12); }
static void adp_GetDesc3(w32 *w) { adapter_desc(w, 12); }        /* DESC3 is DESC2 with a wider Flags */
/* IDXGIAdapter3: video memory accounting. QueryVideoMemoryInfo(node, segment,
 * info): the budget is the memory the description claims, none of it used
 * or reserved -- a program that watches its budget sees room. */
static void adp_QueryVideoMemoryInfo(w32 *w) {
    uint64_t p = ARG(3);
    if (!p || !w32_mem_ok(w, p, 32)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    w32_write(w, p, 8, ARG(2) == 0 ? 256u << 20 : 1024u << 20);   /* Budget: local, then non-local */
    w32_write(w, p + 8, 8, 0); w32_write(w, p + 16, 8, 0); w32_write(w, p + 24, 8, 0);
    RET(S_OK_);
}
static void adp_SetVideoMemoryReservation(w32 *w) { (void)w; RET(S_OK_); }
static void adp_RegisterEvent(w32 *w) { if (ARG(2) && w32_mem_ok(w, ARG(2), 4)) w32_write(w, ARG(2), 4, 1); RET(S_OK_); }
static void adp_UnregisterEvent(w32 *w) { (void)w; RET(0); }
static void adp_CheckInterfaceSupport(w32 *w) { RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_); }
static void dxgidev_GetAdapter(w32 *w) {
    uint64_t a = w32_com_new(w, &cls_adapter, 4);
    out_ptr(w, ARG(1), a);
    RET(a ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void dxgidev_SetGPUThreadPriority(w32 *w) { (void)w; RET(S_OK_); }
static void dxgidev_GetGPUThreadPriority(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 0); RET(S_OK_); }
/* IDXGIDevice1 and 2: frame latency, and offering resources back to the
 * system, which a renderer with no video memory to reclaim accepts and
 * forgets. */
static void dxgidev_SetMaximumFrameLatency(w32 *w) { (void)w; RET(S_OK_); }
static void dxgidev_GetMaximumFrameLatency(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 3); RET(S_OK_); }
static void dxgidev_OfferResources(w32 *w) { (void)w; RET(S_OK_); }
static void dxgidev_ReclaimResources(w32 *w) {
    /* (count, resources, discarded): nothing was lost while offered */
    uint32_t n = (uint32_t)ARG(1);
    if (ARG(3) && n <= 64 && w32_mem_ok(w, ARG(3), 4ull * n)) for (uint32_t i = 0; i < n; i++) w32_write(w, ARG(3) + 4ull * i, 4, 0);
    RET(S_OK_);
}
static void dxgidev_EnqueueSetEvent(w32 *w) {
    /* the GPU has nothing queued, so the event is due now */
    if (ARG(1)) w32_event_set(w, ARG(1));
    RET(S_OK_);
}
static void dxgidev_Trim(w32 *w) { (void)w; RET(0); }                    /* IDXGIDevice3: nothing cached to release */
static void dxgidev_ReclaimResources1(w32 *w) {
    /* (count, resources, results): DXGI_RECLAIM_RESOURCE_RESULT_OK for each */
    uint32_t n = (uint32_t)ARG(1);
    if (ARG(3) && n <= 64 && w32_mem_ok(w, ARG(3), 4ull * n)) for (uint32_t i = 0; i < n; i++) w32_write(w, ARG(3) + 4ull * i, 4, 0);
    RET(S_OK_);
}

/* ---------------------------------------------------------- the vtables */

/* QueryInterface, for real, on the device.
 *
 * The shared one in com.c says yes to everything and hands back the same
 * pointer, which is right when there is one object per interface. It is
 * wrong here: a program routinely asks a D3D11 device for its *DXGI* side,
 * and IDXGIDevice's methods are at different slot numbers than
 * ID3D11Device's. Returning the device would send GetAdapter to
 * CreateTexture1D.
 *
 * So the interface identifier is actually compared. The GUIDs below are the
 * ones a program asks for; anything else gets E_NOINTERFACE, which is a
 * documented answer and better than a wrong object.
 */
static int iid_is(w32 *w, uint64_t p, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4) {
    if (!p) return 0;
    return (uint32_t)w32_read(w, p, 4) == d1 && (uint32_t)w32_read(w, p + 4, 4) == d2
        && (uint32_t)w32_read(w, p + 8, 4) == d3 && (uint32_t)w32_read(w, p + 12, 4) == d4;
}
static void dev_QueryInterface(w32 *w) {
    uint64_t self = ARG(0), iid = ARG(1), out = ARG(2);
    /* IID_IDXGIDevice  {54ec77fa-1377-44e6-8c32-88fd5f44c84c}
     * IID_IDXGIDevice1 {77db970f-6276-48ba-ba28-070143b4392c}
     * IID_IDXGIDevice2 {05008617-fbfd-4051-a790-144884b4f6a9}
     * IID_IDXGIDevice3 {6007896c-3244-4afd-bf18-a6d3beda5023}
     * IID_IDXGIDevice4 {95b4f95f-d8da-4ca4-9ee6-3b76d5968a10} */
    if (iid_is(w, iid, 0x54EC77FAu, 0x44E61377u, 0xFD88328Cu, 0x4CC8445Fu) ||
        iid_is(w, iid, 0x77DB970Fu, 0x48BA6276u, 0x010728BAu, 0x2C39B443u) ||
        iid_is(w, iid, 0x05008617u, 0x4051FBFDu, 0x481490A7u, 0xA9F6B484u) ||
        iid_is(w, iid, 0x6007896Cu, 0x4AFD3244u, 0xD3A618BFu, 0x2350DABEu) ||
        iid_is(w, iid, 0x95B4F95Fu, 0x4CA4D8DAu, 0x763BE69Eu, 0x108A96D5u)) {
        uint64_t obj = w32_com_new(w, &cls_dxgidev, 4);
        out_ptr(w, out, obj);
        RET(obj ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
        return;
    }
    /* Anything else: the device itself, which is right for ID3D11Device,
     * its later revisions and IUnknown -- the interfaces that genuinely
     * share its layout. */
    if (out) w32_write(w, out, (int)w32_ptrsize(w), self);
    w32_com_AddRef(w);
    RET(S_OK_);
}

#define IUNK \
    { "QueryInterface", 3, 0, w32_com_QueryInterface, 0 }, \
    { "AddRef",         1, 0, w32_com_AddRef,         0 }, \
    { "Release",        1, 0, w32_com_Release,        0 }
/* ID3D11DeviceChild: the four every D3D11 object has after IUnknown. */
#define D11CHILD \
    { "GetDevice",              2, 0, child_GetDevice,   0 }, \
    { "GetPrivateData",         4, 0, dev_GetPrivateData, 0 }, \
    { "SetPrivateData",         4, 0, dev_SetPrivateData, 0 }, \
    { "SetPrivateDataInterface", 3, 0, dev_SetPrivateDataInterface, 0 }
/* IDXGIObject: the four after IUnknown on the DXGI side, in DXGI's order. */
#define DXGIOBJ \
    { "SetPrivateData",          4, 0, obj_SetPrivateData, 0 }, \
    { "SetPrivateDataInterface", 3, 0, obj_SetPrivateData, 0 }, \
    { "GetPrivateData",          4, 0, obj_GetPrivateData, 0 }, \
    { "GetParent",               3, 0, obj_GetParent,      0 }

#define NIL { 0, 0, 0, 0, 0 }

/* The slot numbers are the ABI: a guest calls `vtbl[12]`, not "CreateInputLayout".
 * Every one of these tables is padded to the real interface's method count so
 * that a call to something unimplemented lands on a stub that names the slot
 * rather than on the wrong function. */
static const w32_api device_methods[] = {
    { "QueryInterface", 3, 0, dev_QueryInterface, 0 },
    { "AddRef",         1, 0, w32_com_AddRef,     0 },
    { "Release",        1, 0, w32_com_Release,    0 },
    { "CreateBuffer",            4, 0, dev_CreateBuffer, 0 },
    NIL,                                                    /* CreateTexture1D */
    { "CreateTexture2D",         4, 0, dev_CreateTexture2D, 0 },
    NIL,                                                    /* CreateTexture3D */
    { "CreateShaderResourceView", 4, 0, dev_CreateShaderResourceView, 0 },
    NIL,                                                    /* CreateUnorderedAccessView */
    { "CreateRenderTargetView",  4, 0, dev_CreateRenderTargetView, 0 },
    { "CreateDepthStencilView",  4, 0, dev_CreateDepthStencilView, 0 },
    { "CreateInputLayout",       6, 0, dev_CreateInputLayout, 0 },
    { "CreateVertexShader",      5, 0, dev_CreateVertexShader, 0 },
    NIL, NIL,                                               /* geometry shaders */
    { "CreatePixelShader",       5, 0, dev_CreatePixelShader, 0 },
    NIL, NIL, NIL,                                          /* hull, domain, compute */
    NIL,                                                    /* CreateClassLinkage */
    { "CreateBlendState",        3, 0, dev_CreateBlendState, 0 },
    { "CreateDepthStencilState", 3, 0, dev_CreateDepthStencilState, 0 },
    { "CreateRasterizerState",   3, 0, dev_CreateRasterizerState, 0 },
    { "CreateSamplerState",      3, 0, dev_CreateSamplerState, 0 },
    { "CreateQuery",             3, 0, dev_CreateQuery, 0 },
    { "CreatePredicate",         3, 0, dev_CreateQuery, 0 },
    { "CreateCounter",           3, 0, dev_CreateQuery, 0 },
    NIL,                                                    /* CreateDeferredContext */
    { "OpenSharedResource",      4, 0, dev_OpenSharedResource, 0 },
    { "CheckFormatSupport",      3, 0, dev_CheckFormatSupport, 0 },
    { "CheckMultisampleQualityLevels", 4, 0, dev_CheckMultisampleQualityLevels, 0 },
    NIL, NIL,                                               /* CheckCounterInfo, CheckCounter */
    { "CheckFeatureSupport",     4, 0, dev_CheckFeatureSupport, 0 },
    { "GetPrivateData",          4, 0, dev_GetPrivateData, 0 },
    { "SetPrivateData",          4, 0, dev_SetPrivateData, 0 },
    { "SetPrivateDataInterface", 3, 0, dev_SetPrivateDataInterface, 0 },
    { "GetFeatureLevel",         1, 0, dev_GetFeatureLevel, 0 },
    { "GetCreationFlags",        1, 0, dev_GetCreationFlags, 0 },
    { "GetDeviceRemovedReason",  1, 0, dev_GetDeviceRemovedReason, 0 },
    { "GetImmediateContext",     2, 0, dev_GetImmediateContext, 0 },
    { "SetExceptionMode",        2, 0, dev_SetExceptionMode, 0 },
    { "GetExceptionMode",        1, 0, dev_GetExceptionMode, 0 },
};

static const w32_api context_methods[] = {
    IUNK,
    /* ID3D11DeviceContext's GetDevice returns the device that made it,
     * which is the one object here that actually knows. */
    { "GetDevice",              2, 0, ctx_GetDevice,   0 },
    { "GetPrivateData",         4, 0, dev_GetPrivateData, 0 },
    { "SetPrivateData",         4, 0, dev_SetPrivateData, 0 },
    { "SetPrivateDataInterface", 3, 0, dev_SetPrivateDataInterface, 0 },
    { "VSSetConstantBuffers",   4, 0, ctx_VSSetConstantBuffers, 0 },   /* 7 */
    { "PSSetShaderResources",   4, 0, ctx_PSSetShaderResources, 0 },
    { "PSSetShader",            4, 0, ctx_PSSetShader, 0 },
    { "PSSetSamplers",          4, 0, ctx_PSSetSamplers, 0 },
    { "VSSetShader",            4, 0, ctx_VSSetShader, 0 },
    { "DrawIndexed",            4, 0, ctx_DrawIndexed, 0 },
    { "Draw",                   3, 0, ctx_Draw, 0 },
    { "Map",                    6, 0, ctx_Map, 0 },
    { "Unmap",                  3, 0, ctx_Unmap, 0 },
    { "PSSetConstantBuffers",   4, 0, ctx_PSSetConstantBuffers, 0 },
    { "IASetInputLayout",       2, 0, ctx_IASetInputLayout, 0 },
    { "IASetVertexBuffers",     6, 0, ctx_IASetVertexBuffers, 0 },
    { "IASetIndexBuffer",       4, 0, ctx_IASetIndexBuffer, 0 },
    { "DrawIndexedInstanced",   6, 0, ctx_DrawIndexedInstanced, 0 },
    { "DrawInstanced",          5, 0, ctx_DrawInstanced, 0 },
    NIL, NIL,                                                /* GSSetConstantBuffers, GSSetShader */
    { "IASetPrimitiveTopology", 2, 0, ctx_IASetPrimitiveTopology, 0 }, /* 24 */
    NIL, NIL,                                                /* VSSetShaderResources, VSSetSamplers */
    { "Begin",                  2, 0, ctx_Begin, 0 },
    { "End",                    2, 0, ctx_End, 0 },
    { "GetData",                5, 0, ctx_GetData, 0 },
    NIL, NIL, NIL,                                           /* SetPredication, GS resources */
    { "OMSetRenderTargets",     4, 0, ctx_OMSetRenderTargets, 0 },     /* 33 */
    NIL,                                                     /* ...AndUnorderedAccessViews */
    { "OMSetBlendState",        4, 0, ctx_OMSetBlendState, 0 },
    { "OMSetDepthStencilState", 3, 0, ctx_OMSetDepthStencilState, 0 },
    NIL, NIL, NIL, NIL, NIL, NIL,                            /* SO, DrawAuto, indirect, dispatch */
    { "RSSetState",             2, 0, ctx_RSSetState, 0 },             /* 43 */
    { "RSSetViewports",         3, 0, ctx_RSSetViewports, 0 },
    { "RSSetScissorRects",      3, 0, ctx_RSSetScissorRects, 0 },
    NIL,                                                     /* CopySubresourceRegion */
    { "CopyResource",           3, 0, ctx_CopyResource, 0 },
    { "UpdateSubresource",      7, 0, ctx_UpdateSubresource, 0 },
    NIL,                                                     /* CopyStructureCount */
    { "ClearRenderTargetView",  3, 0, ctx_ClearRenderTargetView, 0 },  /* 50 */
    NIL, NIL,
    { "ClearDepthStencilView",  5, 0, ctx_ClearDepthStencilView, 0 },  /* 53 */
    NIL, NIL, NIL, NIL, NIL,                                 /* GenerateMips .. ExecuteCommandList */
    NIL, NIL, NIL, NIL,                                      /* hull */
    NIL, NIL, NIL, NIL,                                      /* domain */
    NIL, NIL, NIL, NIL, NIL,                                 /* compute */
    NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL,        /* 72..81 getters */
    NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL,        /* 82..91 */
    NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL,        /* 92..101 */
    NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL,                  /* 102..109 */
    { "ClearState",             1, 0, ctx_ClearState, 0 },             /* 110 */
    { "Flush",                  1, 0, ctx_Flush, 0 },
    { "GetType",                1, 0, ctx_GetType, 0 },
    { "GetContextFlags",        1, 0, ctx_GetContextFlags, 0 },
    NIL,                                                     /* FinishCommandList */
};

static const w32_api swapchain_methods[] = {
    IUNK, DXGIOBJ,
    { "GetDevice",            3, 0, sc_GetDevice, 0 },       /* 7 */
    { "Present",              3, 0, sc_Present, 0 },
    { "GetBuffer",            4, 0, sc_GetBuffer, 0 },
    { "SetFullscreenState",   3, 0, sc_SetFullscreenState, 0 },
    { "GetFullscreenState",   3, 0, sc_GetFullscreenState, 0 },
    { "GetDesc",              2, 0, sc_GetDesc, 0 },
    { "ResizeBuffers",        6, 0, sc_ResizeBuffers, 0 },
    { "ResizeTarget",         2, 0, sc_ResizeTarget, 0 },
    { "GetContainingOutput",  2, 0, sc_GetContainingOutput, 0 },
    { "GetFrameStatistics",   2, 0, sc_GetFrameStatistics, 0 },
    { "GetLastPresentCount",  2, 0, sc_GetLastPresentCount, 0 },
    { "GetDesc1",             2, 0, sc_GetDesc1, 0 },           /* 18: IDXGISwapChain1 */
    { "GetFullscreenDesc",    2, 0, sc_GetFullscreenDesc, 0 },
    { "GetHwnd",              2, 0, sc_GetHwnd, 0 },
    { "GetCoreWindow",        3, 0, sc_GetCoreWindow, 0 },
    { "Present1",             4, 0, sc_Present, 0 },
    { "IsTemporaryMonoSupported", 1, 0, sc_IsTemporaryMonoSupported, 0 },
    { "GetRestrictToOutput",  2, 0, sc_GetRestrictToOutput, 0 },
    { "SetBackgroundColor",   2, 0, sc_SetBackgroundColor, 0 },
    { "GetBackgroundColor",   2, 0, sc_GetBackgroundColor, 0 },
    { "SetRotation",          2, 0, sc_SetRotation, 0 },
    { "GetRotation",          2, 0, sc_GetRotation, 0 },
    { "SetSourceSize",        3, 0, sc_SetSourceSize, 0 },          /* 29: IDXGISwapChain2 */
    { "GetSourceSize",        3, 0, sc_GetSourceSize, 0 },
    { "SetMaximumFrameLatency", 2, 0, sc_SetMaximumFrameLatency, 0 },
    { "GetMaximumFrameLatency", 2, 0, sc_GetMaximumFrameLatency, 0 },
    { "GetFrameLatencyWaitableObject", 1, 0, sc_GetFrameLatencyWaitableObject, 0 },
    { "SetMatrixTransform",   2, 0, sc_SetMatrixTransform, 0 },
    { "GetMatrixTransform",   2, 0, sc_GetMatrixTransform, 0 },
    { "GetCurrentBackBufferIndex", 1, 0, sc_GetCurrentBackBufferIndex, 0 },   /* 36: IDXGISwapChain3 */
    { "CheckColorSpaceSupport", 3, 0, sc_CheckColorSpaceSupport, 0 },
    { "SetColorSpace1",       2, 0, sc_SetColorSpace1, 0 },
    { "ResizeBuffers1",       8, 0, sc_ResizeBuffers1, 0 },
    { "SetHDRMetaData",       4, 0, sc_SetHDRMetaData, 0 },          /* 40: IDXGISwapChain4 */
};

static const w32_api texture_methods[] = {
    IUNK, D11CHILD,
    { "GetType",               2, 0, res_GetType, 0 },       /* 7 */
    { "SetEvictionPriority",   2, 0, res_SetEvictionPriority, 0 },
    { "GetEvictionPriority",   1, 0, res_GetEvictionPriority, 0 },
    { "GetDesc",               2, 0, tex_GetDesc, 0 },       /* 10 */
};
static const w32_api buffer_methods[] = {
    IUNK, D11CHILD,
    { "GetType",               2, 0, res_GetType, 0 },
    { "SetEvictionPriority",   2, 0, res_SetEvictionPriority, 0 },
    { "GetEvictionPriority",   1, 0, res_GetEvictionPriority, 0 },
    { "GetDesc",               2, 0, buf_GetDesc, 0 },
};
static const w32_api view_methods[] = {
    IUNK, D11CHILD,
    { "GetResource",           2, 0, view_GetResource, 0 },  /* 7 */
    { "GetDesc",               2, 0, state_GetDesc, 0 },     /* 8 */
};
static const w32_api child_methods[] = { IUNK, D11CHILD };
static const w32_api state_methods[] = {
    IUNK, D11CHILD,
    { "GetDesc",               2, 0, state_GetDesc, 0 },     /* 7 */
};
/* An asynchronous object has one more: how big its result is. */
static void query_GetDataSize(w32 *w) { (void)w; RET(8); }
static const w32_api query_methods[] = {
    IUNK, D11CHILD,
    { "GetDataSize",           1, 0, query_GetDataSize, 0 }, /* 7 */
    { "GetDesc",               2, 0, state_GetDesc, 0 },     /* 8 */
};
static const w32_api factory_methods[] = {
    IUNK, DXGIOBJ,
    { "EnumAdapters",          3, 0, fac_EnumAdapters, 0 },  /* 7 */
    { "MakeWindowAssociation", 3, 0, fac_MakeWindowAssociation, 0 },
    { "GetWindowAssociation",  2, 0, fac_GetWindowAssociation, 0 },
    { "CreateSwapChain",       4, 0, fac_CreateSwapChain, 0 },
    NIL,                                                     /* CreateSoftwareAdapter */
    { "EnumAdapters1",         3, 0, fac_EnumAdapters, 0 },  /* IDXGIFactory1 */
    { "IsCurrent",             1, 0, fac_IsCurrent, 0 },
    { "IsWindowedStereoEnabled",       1, 0, fac_IsWindowedStereoEnabled, 0 },   /* 14: IDXGIFactory2 */
    { "CreateSwapChainForHwnd",        7, 0, fac_CreateSwapChainForHwnd, 0 },
    { "CreateSwapChainForCoreWindow",  6, 0, fac_CreateSwapChainForCoreWindow, 0 },
    { "GetSharedResourceAdapterLuid",  3, 0, fac_GetSharedResourceAdapterLuid, 0 },
    { "RegisterStereoStatusWindow",    4, 0, fac_RegisterStatusWindow, 0 },
    { "RegisterStereoStatusEvent",     3, 0, fac_RegisterStatusEvent, 0 },
    { "UnregisterStereoStatus",        2, 0, fac_UnregisterStatus, 0 },
    { "RegisterOcclusionStatusWindow", 4, 0, fac_RegisterStatusWindow, 0 },
    { "RegisterOcclusionStatusEvent",  3, 0, fac_RegisterStatusEvent, 0 },
    { "UnregisterOcclusionStatus",     2, 0, fac_UnregisterStatus, 0 },
    { "CreateSwapChainForComposition", 5, 0, fac_CreateSwapChainForComposition, 0 },
    { "GetCreationFlags",              1, 0, fac_GetCreationFlags, 0 },          /* 25: IDXGIFactory3 */
    { "EnumAdapterByLuid",             4, 0, fac_EnumAdapterByLuid, 0 },         /* 26: IDXGIFactory4 */
    { "EnumWarpAdapter",               3, 0, fac_EnumWarpAdapter, 0 },
    { "CheckFeatureSupport",           4, 0, fac_CheckFeatureSupport, 0 },       /* 28: IDXGIFactory5 */
    { "EnumAdapterByGpuPreference",    5, 0, fac_EnumAdapterByGpuPreference, 0 }, /* 29: IDXGIFactory6 */
    { "RegisterAdaptersChangedEvent",  3, 0, fac_RegisterAdaptersChangedEvent, 0 },   /* 30: IDXGIFactory7 */
    { "UnregisterAdaptersChangedEvent", 2, 0, fac_UnregisterAdaptersChangedEvent, 0 },
};
static const w32_api dxgidev_methods[] = {
    IUNK, DXGIOBJ,
    { "GetAdapter",            2, 0, dxgidev_GetAdapter, 0 },
    NIL, NIL,
    { "SetGPUThreadPriority",  2, 0, dxgidev_SetGPUThreadPriority, 0 },
    { "GetGPUThreadPriority",  2, 0, dxgidev_GetGPUThreadPriority, 0 },
    { "SetMaximumFrameLatency", 2, 0, dxgidev_SetMaximumFrameLatency, 0 },   /* 12: IDXGIDevice1 */
    { "GetMaximumFrameLatency", 2, 0, dxgidev_GetMaximumFrameLatency, 0 },
    { "OfferResources",        4, 0, dxgidev_OfferResources, 0 },            /* 14: IDXGIDevice2 */
    { "ReclaimResources",      4, 0, dxgidev_ReclaimResources, 0 },
    { "EnqueueSetEvent",       2, 0, dxgidev_EnqueueSetEvent, 0 },
    { "Trim",                  1, 0, dxgidev_Trim, 0 },                       /* 17: IDXGIDevice3 */
    { "OfferResources1",       5, 0, dxgidev_OfferResources, 0 },            /* 18: IDXGIDevice4 */
    { "ReclaimResources1",     4, 0, dxgidev_ReclaimResources1, 0 },
};
static const w32_api adapter_methods[] = {
    IUNK, DXGIOBJ,
    { "EnumOutputs",           3, 0, adp_EnumOutputs, 0 },
    { "GetDesc",               2, 0, adp_GetDesc, 0 },
    { "CheckInterfaceSupport", 3, 0, adp_CheckInterfaceSupport, 0 },
    { "GetDesc1",              2, 0, adp_GetDesc1, 0 },     /* 10: IDXGIAdapter1 */
    { "GetDesc2",              2, 0, adp_GetDesc2, 0 },     /* 11: IDXGIAdapter2 */
    { "RegisterHardwareContentProtectionTeardownStatusEvent", 3, 0, adp_RegisterEvent, 0 },   /* 12: IDXGIAdapter3 */
    { "UnregisterHardwareContentProtectionTeardownStatus",    2, 0, adp_UnregisterEvent, 0 },
    { "QueryVideoMemoryInfo",  4, 0, adp_QueryVideoMemoryInfo, 0 },
    { "SetVideoMemoryReservation", 4, 0, adp_SetVideoMemoryReservation, 0 },
    { "RegisterVideoMemoryBudgetChangeNotificationEvent", 3, 0, adp_RegisterEvent, 0 },
    { "UnregisterVideoMemoryBudgetChangeNotification",    2, 0, adp_UnregisterEvent, 0 },
    { "GetDesc3",              2, 0, adp_GetDesc3, 0 },     /* 18: IDXGIAdapter4 */
};

#define NM(a) ((int)(sizeof (a) / sizeof (a)[0]))

/* The slot counts are the ABI, and getting one wrong is not a compile error
 * or a crash -- it is a call landing on the neighbouring method, which is how
 * asking a device for its feature level ended up asking for its creation
 * flags. Counting them by eye is exactly the wrong tool, so the counts below
 * are the ones in d3d11.h and dxgi.h, and the compiler checks them.
 *
 * If one of these fires, the fix is to add the missing method to the table
 * as a NIL in the right place -- never to change the number here. */
_Static_assert(NM(device_methods)    == 43, "ID3D11Device has 43 methods");
_Static_assert(NM(context_methods)   == 115, "ID3D11DeviceContext has 115 methods");
_Static_assert(NM(swapchain_methods) == 41, "IDXGISwapChain4 has 41 methods");
_Static_assert(NM(texture_methods)   == 11, "ID3D11Texture2D has 11 methods");
_Static_assert(NM(buffer_methods)    == 11, "ID3D11Buffer has 11 methods");
_Static_assert(NM(view_methods)      == 9,  "ID3D11View-derived interfaces have 9");
_Static_assert(NM(child_methods)     == 7,  "ID3D11DeviceChild has 7 methods");
_Static_assert(NM(state_methods)     == 8,  "a D3D11 state object has 8 methods");
_Static_assert(NM(query_methods)     == 9,  "ID3D11Query has 9 methods");
_Static_assert(NM(factory_methods)   == 32, "IDXGIFactory7 has 32 methods");
_Static_assert(NM(dxgidev_methods)   == 20, "IDXGIDevice4 has 20 methods");
_Static_assert(NM(adapter_methods)   == 19, "IDXGIAdapter4 has 19 methods");
static w32_com_class cls_device    = { "ID3D11Device",         device_methods,    NM(device_methods),    TAG_D11_DEVICE, 0, {0} };
static w32_com_class cls_context   = { "ID3D11DeviceContext",  context_methods,   NM(context_methods),   TAG_D11_CONTEXT, 0, {0} };
static w32_com_class cls_swapchain = { "IDXGISwapChain4",      swapchain_methods, NM(swapchain_methods), TAG_D11_SWAPCHAIN, 0, {0} };
static w32_com_class cls_texture   = { "ID3D11Texture2D",      texture_methods,   NM(texture_methods),   TAG_D11_TEXTURE, 0, {0} };
static w32_com_class cls_buffer    = { "ID3D11Buffer",         buffer_methods,    NM(buffer_methods),    TAG_D11_BUFFER, 0, {0} };
static w32_com_class cls_rtv       = { "ID3D11RenderTargetView", view_methods,    NM(view_methods),      TAG_D11_RTV, 0, {0} };
static w32_com_class cls_srv       = { "ID3D11ShaderResourceView", view_methods,  NM(view_methods),      TAG_D11_SRV, 0, {0} };
static w32_com_class cls_dsv       = { "ID3D11DepthStencilView", view_methods,    NM(view_methods),      TAG_D11_DSV, 0, {0} };
static w32_com_class cls_layout    = { "ID3D11InputLayout",    child_methods,     NM(child_methods),     TAG_D11_LAYOUT, 0, {0} };
static w32_com_class cls_vshader   = { "ID3D11VertexShader",   child_methods,     NM(child_methods),     TAG_D11_VS, 0, {0} };
static w32_com_class cls_pshader   = { "ID3D11PixelShader",    child_methods,     NM(child_methods),     TAG_D11_PS, 0, {0} };
static w32_com_class cls_sampler   = { "ID3D11SamplerState",   state_methods,     NM(state_methods),     TAG_D11_SAMPLER, 0, {0} };
static w32_com_class cls_blend     = { "ID3D11BlendState",     state_methods,     NM(state_methods),     TAG_D11_BLEND, 0, {0} };
static w32_com_class cls_raster    = { "ID3D11RasterizerState", state_methods,    NM(state_methods),     TAG_D11_RASTER, 0, {0} };
static w32_com_class cls_depth     = { "ID3D11DepthStencilState", state_methods,  NM(state_methods),     TAG_D11_DEPTH, 0, {0} };
static w32_com_class cls_query     = { "ID3D11Query",          query_methods,     NM(query_methods),     TAG_D11_QUERY, 0, {0} };
static w32_com_class cls_factory   = { "IDXGIFactory7",        factory_methods,   NM(factory_methods),   TAG_DXGI_FACTORY, 0, {0} };
static w32_com_class cls_dxgidev   = { "IDXGIDevice4",         dxgidev_methods,   NM(dxgidev_methods),   TAG_DXGI_DEVICE, 0, {0} };
static w32_com_class cls_adapter   = { "IDXGIAdapter4",        adapter_methods,   NM(adapter_methods),   TAG_DXGI_ADAPTER, 0, {0} };


/* -------------------------------------------------------- creating it all */

static uint64_t make_swapchain(w32 *w, uint64_t device, int width, int height) {
    if (width <= 0 || height <= 0) w32_screen_size(&width, &height);
    uint64_t sc = w32_com_new(w, &cls_swapchain, SC_N);
    if (!sc) return 0;
    uint64_t tex = make_texture(w, width, height, FMT_B8G8R8A8_UNORM, 0x20 | 0x8, 0);
    if (!tex) return 0;
    w32_com_set(w, sc, SC_DEV, device);
    w32_com_set(w, sc, SC_TEX, tex);
    w32_com_set(w, sc, SC_W, (uint64_t)width);
    w32_com_set(w, sc, SC_H, (uint64_t)height);
    if (device) w32_com_set(w, device, DEV_SWAP, sc);
    return sc;
}

static uint64_t make_device(w32 *w, uint32_t flags) {
    uint64_t dev = w32_com_new(w, &cls_device, DEV_N);
    if (!dev) return 0;
    uint64_t ctx = w32_com_new(w, &cls_context, CTX_N);
    if (!ctx) return 0;
    w32_com_set(w, ctx, CTX_DEV, dev);
    w32_com_set(w, ctx, CTX_TOPOLOGY, 4);          /* TRIANGLELIST, until told otherwise */
    w32_com_set(w, dev, DEV_CTX, ctx);
    w32_com_set(w, dev, DEV_FLAGS, flags);
    return dev;
}

/* DXGI_SWAP_CHAIN_DESC's width and height, which may be zero meaning "the
 * size of the output window" -- and the output window here is the display. */
static void swapchain_size(w32 *w, uint64_t desc, int *width, int *height) {
    *width = 0; *height = 0;
    if (desc) {
        *width = (int)(uint32_t)w32_read(w, desc, 4);
        *height = (int)(uint32_t)w32_read(w, desc + 4, 4);
    }
    if (*width <= 0 || *height <= 0) w32_screen_size(width, height);
}

static void fac_CreateSwapChain(w32 *w) {
    /* (device, desc, out). DXGI_SWAP_CHAIN_DESC keeps its OutputWindow at
     * byte 48 in both bitnesses: a 28-byte mode, an 8-byte sample
     * description, usage and count, and the pointer lands 8-aligned. */
    int width, height;
    swapchain_size(w, ARG(2), &width, &height);
    uint64_t sc = make_swapchain(w, ARG(1), width, height);
    if (sc && ARG(2) && w32_mem_ok(w, ARG(2) + 48, w32_ptrsize(w))) w32_com_set(w, sc, SC_HWND, w32_read(w, ARG(2) + 48, (int)w32_ptrsize(w)));
    out_ptr(w, ARG(3), sc);
    RET(sc ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
/* IDXGIFactory2. CreateSwapChainForHwnd(device, hwnd, desc1, fullscreenDesc,
 * restrictToOutput, out) is how a Windows 8+ program makes its swap chain;
 * DXGI_SWAP_CHAIN_DESC1 starts with Width and Height like the old one, so
 * the same reader serves. The CoreWindow and composition forms have no
 * window to read a size from and get the display's. */
static void fac_CreateSwapChainForHwnd(w32 *w) {
    int width, height;
    swapchain_size(w, ARG(3), &width, &height);
    uint64_t sc = make_swapchain(w, ARG(1), width, height);
    if (sc) w32_com_set(w, sc, SC_HWND, ARG(2));
    out_ptr(w, ARG(6), sc);
    RET(sc ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_CreateSwapChainForCoreWindow(w32 *w) {
    int width, height;
    swapchain_size(w, ARG(3), &width, &height);
    uint64_t sc = make_swapchain(w, ARG(1), width, height);
    out_ptr(w, ARG(5), sc);
    RET(sc ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_CreateSwapChainForComposition(w32 *w) {
    int width, height;
    swapchain_size(w, ARG(2), &width, &height);
    uint64_t sc = make_swapchain(w, ARG(1), width, height);
    out_ptr(w, ARG(4), sc);
    RET(sc ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_IsWindowedStereoEnabled(w32 *w) { (void)w; RET(0); }
static void fac_GetSharedResourceAdapterLuid(w32 *w) {
    if (ARG(2) && w32_mem_ok(w, ARG(2), 8)) w32_write(w, ARG(2), 8, 0);
    RET(S_OK_);
}
/* Stereo and occlusion status: registered with a cookie nothing ever fires
 * -- there is one screen and it is never covered. The window form is
 * (hwnd, message, cookie*), the event form (event, cookie*). */
static void fac_RegisterStatusWindow(w32 *w) { if (ARG(3) && w32_mem_ok(w, ARG(3), 4)) w32_write(w, ARG(3), 4, 1); RET(S_OK_); }
static void fac_RegisterStatusEvent(w32 *w)  { if (ARG(2) && w32_mem_ok(w, ARG(2), 4)) w32_write(w, ARG(2), 4, 1); RET(S_OK_); }
static void fac_UnregisterStatus(w32 *w) { (void)w; RET(0); }
/* IDXGIFactory3..7: the creation flags, adapters by LUID, the WARP adapter,
 * feature support, adapters by GPU preference, adapter-change events. A
 * program that has asked QueryInterface for IDXGIFactory5 to check tearing
 * support calls slot 28, and lands here rather than off the table. */
static void fac_GetCreationFlags(w32 *w) { (void)w; RET(0); }
static void fac_EnumAdapterByLuid(w32 *w) {
    /* (luid, riid, out): the one adapter, whatever LUID was asked for */
    uint64_t a = w32_com_new(w, &cls_adapter, 4);
    out_ptr(w, ARG(3), a);
    RET(a ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_EnumWarpAdapter(w32 *w) {
    /* (riid, out): a software rasterizer is exactly what this is */
    uint64_t a = w32_com_new(w, &cls_adapter, 4);
    out_ptr(w, ARG(2), a);
    RET(a ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_CheckFeatureSupport(w32 *w) {
    /* (feature, data, size): DXGI_FEATURE_PRESENT_ALLOW_TEARING is the only
     * feature there is, and the answer is no -- there is no vsync to tear
     * through here */
    if (ARG(2) && ARG(3) >= 4 && w32_mem_ok(w, ARG(2), 4)) w32_write(w, ARG(2), 4, 0);
    RET(ARG(1) == 0 ? S_OK_ : (uint64_t)(uint32_t)DXGI_ERROR_INVALID_CALL_);
}
static void fac_EnumAdapterByGpuPreference(w32 *w) {
    /* (index, preference, riid, out) */
    if (ARG(1) != 0) { out_ptr(w, ARG(4), 0); RET((uint64_t)(uint32_t)DXGI_ERROR_NOT_FOUND_); return; }
    uint64_t a = w32_com_new(w, &cls_adapter, 4);
    out_ptr(w, ARG(4), a);
    RET(a ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void fac_RegisterAdaptersChangedEvent(w32 *w) { if (ARG(2) && w32_mem_ok(w, ARG(2), 4)) w32_write(w, ARG(2), 4, 1); RET(S_OK_); }
static void fac_UnregisterAdaptersChangedEvent(w32 *w) { (void)w; RET(S_OK_); }

/* D3D11CreateDevice(adapter, driverType, software, flags, featureLevels,
 *                   numFeatureLevels, sdkVersion, ppDevice, pFeatureLevel,
 *                   ppContext) */
static void d_D3D11CreateDevice(w32 *w) {
    uint64_t dev = make_device(w, (uint32_t)ARG(3));
    if (!dev) {
        out_ptr(w, ARG(7), 0);
        out_ptr(w, ARG(9), 0);
        RET((uint64_t)(uint32_t)E_OUTOFMEMORY_);
        return;
    }
    out_ptr(w, ARG(7), dev);
    if (ARG(8)) w32_write(w, ARG(8), 4, 0xB000u);          /* D3D_FEATURE_LEVEL_11_0 */
    out_ptr(w, ARG(9), w32_com_get(w, dev, DEV_CTX));
    RET(S_OK_);
}

/* D3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags,
 *                               featureLevels, numFeatureLevels, sdkVersion,
 *                               swapChainDesc, ppSwapChain, ppDevice,
 *                               pFeatureLevel, ppContext) */
static void d_D3D11CreateDeviceAndSwapChain(w32 *w) {
    uint64_t dev = make_device(w, (uint32_t)ARG(3));
    if (!dev) {
        out_ptr(w, ARG(8), 0); out_ptr(w, ARG(9), 0); out_ptr(w, ARG(11), 0);
        RET((uint64_t)(uint32_t)E_OUTOFMEMORY_);
        return;
    }
    int width, height;
    swapchain_size(w, ARG(7), &width, &height);
    uint64_t sc = make_swapchain(w, dev, width, height);
    out_ptr(w, ARG(8), sc);
    out_ptr(w, ARG(9), dev);
    if (ARG(10)) w32_write(w, ARG(10), 4, 0xB000u);
    out_ptr(w, ARG(11), w32_com_get(w, dev, DEV_CTX));
    RET(sc ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void d_D3D11On12CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d11!D3D11On12CreateDevice -- there is no Direct3D 12 to sit on");
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}

static void x_CreateDXGIFactory(w32 *w) {
    uint64_t f = w32_com_new(w, &cls_factory, 4);
    out_ptr(w, ARG(1), f);
    RET(f ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void x_CreateDXGIFactory2(w32 *w) {
    uint64_t f = w32_com_new(w, &cls_factory, 4);
    out_ptr(w, ARG(2), f);
    RET(f ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}

#define F(n, a) { #n, a, 0, d_##n, 0 }
const w32_api w32_d3d11[] = {
    F(D3D11CreateDevice, 10), F(D3D11CreateDeviceAndSwapChain, 12),
    F(D3D11On12CreateDevice, 11),
    { 0, 0, 0, 0, 0 },
};
#undef F

const w32_api w32_dxgi[] = {
    { "CreateDXGIFactory",  2, 0, x_CreateDXGIFactory, 0 },
    { "CreateDXGIFactory1", 2, 0, x_CreateDXGIFactory, 0 },
    { "CreateDXGIFactory2", 3, 0, x_CreateDXGIFactory2, 0 },
    { 0, 0, 0, 0, 0 },
};

/* Direct3D 10 and 12 are still not implemented, and still say so. 10 is a
 * dead API that a game only reaches for as a fallback, and 12 is a
 * different enough model that pretending would be worse than refusing. */
static void t_D3D10CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d10!D3D10CreateDevice -- Direct3D 10 is not implemented (11 is)");
    if (ARG(5)) w32_write(w, ARG(5), (int)w32_ptrsize(w), 0);
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}
static void t_D3D12CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d12!D3D12CreateDevice -- Direct3D 12 is not implemented");
    if (ARG(3)) w32_write(w, ARG(3), (int)w32_ptrsize(w), 0);
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}
const w32_api w32_d3d10[] = {
    { "D3D10CreateDevice", 6, 0, t_D3D10CreateDevice, 0 },
    { "D3D10CreateDeviceAndSwapChain", 8, 0, t_D3D10CreateDevice, 0 },
    { 0, 0, 0, 0, 0 },
};
const w32_api w32_d3d12[] = {
    { "D3D12CreateDevice", 4, 0, t_D3D12CreateDevice, 0 },
    { "D3D12GetDebugInterface", 2, 0, t_D3D12CreateDevice, 0 },
    { 0, 0, 0, 0, 0 },
};
