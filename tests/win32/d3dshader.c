/* Direct3D 9 shaders, the way a 2010s 2D engine uses them.
 *
 * Until d3d9_shader.c existed, CreateVertexShader and CreatePixelShader
 * handed back objects nothing ran: a game that transforms its vertices in
 * vs_2_0 by a matrix in c0..c3, and tints and cuts out its sprites in
 * ps_2_0, drew with the fixed-function reading instead, which put every
 * quad somewhere else and every colour wrong.
 *
 * The bytecode is assembled here from the token format itself, the way
 * test_sm3.c does on the host, so the test does not depend on a shader
 * compiler being present at build time. Three quads: one through both
 * shaders (a vertex declaration, a constant matrix, a texture sampled and
 * tinted, and texkill cutting three quarters away); one through the pixel
 * shader alone with the fixed-function transform in front of it; one through
 * the vertex shader alone with the fixed-function texture-times-colour after
 * it. The frame checksum is what makes it a test.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define W 256
#define H 144

static int fails;
static void ok(int cond, const char *what) { printf("%s %s\n", cond ? "ok  " : "FAIL", what); if (!cond) fails++; }

/* ---- the token format ---- */
static DWORD code[256]; static int nw, op_at;
static void w(DWORD v) { code[nw++] = v; }
static void begin(int vs, int major, int minor) { nw = 0; w((vs ? 0xFFFE0000u : 0xFFFF0000u) | (DWORD)major << 8 | (DWORD)minor); }
static void finish(void) { w(0x0000FFFF); }
static void op(DWORD opcode) { op_at = nw; w(opcode); }
static void end(void) { code[op_at] |= (DWORD)(nw - op_at - 1) << 24; }
static DWORD regtok(int type, int reg) { return 0x80000000u | (DWORD)(type & 7) << 28 | (DWORD)((type >> 3) & 3) << 11 | (DWORD)reg; }
static void dst(int type, int reg, int mask) { w(regtok(type, reg) | (DWORD)mask << 16); }
static DWORD swz(int x, int y, int z, int q) { return (DWORD)(x | y << 2 | z << 4 | q << 6); }
static void src(int type, int reg, DWORD sw) { w(regtok(type, reg) | sw << 16); }
static void dcl(int usage, int index, int type, int reg) { op(31); w(0x80000000u | (DWORD)usage | (DWORD)index << 16); dst(type, reg, 0xF); end(); }
static void dcl_sampler(int reg) { op(31); w(0x80000000u | 2u << 27); dst(10, reg, 0xF); end(); }
#define XYZW swz(0,1,2,3)
enum { R = 0, V = 1, C = 2, T = 3, OPOS = 4, OD = 5, OT = 6, OC = 8, SMP = 10 };
enum { MOV = 1, ADD = 2, MUL = 5, M4x4 = 20, TEXKILL = 65, TEXLD = 66 };

struct Vtx { float x, y, z; DWORD color; float u, v; };
struct VtxT { float x, y, z, rhw; DWORD color; float u, v; };

int main(void) {
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
    D3DPRESENT_PARAMETERS pp; memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE;
    IDirect3DDevice9 *dev = NULL;
    if (IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev) != D3D_OK || !dev) {
        printf("CreateDevice failed\n"); return 1;
    }
    ok(1, "CreateDevice");

    /* a 4x4 checkerboard texture: red and blue */
    IDirect3DTexture9 *tex = NULL;
    ok(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL) == D3D_OK && tex, "CreateTexture 4x4");
    if (!tex) { printf("%d failures\n", fails); return 1; }
    D3DLOCKED_RECT lr;
    if (IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0) == D3D_OK) {
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = ((x + y) & 1) ? 0xFFFF0000u : 0xFF0000FFu;
        IDirect3DTexture9_UnlockRect(tex, 0);
    }
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);

    /* vs_2_0: oPos = v0 * (c0..c3), oD0 = v1, oT0 = v2 */
    begin(1, 2, 0);
    dcl(D3DDECLUSAGE_POSITION, 0, V, 0); dcl(D3DDECLUSAGE_COLOR, 0, V, 1); dcl(D3DDECLUSAGE_TEXCOORD, 0, V, 2);
    op(M4x4); dst(OPOS, 0, 0xF); src(V, 0, XYZW); src(C, 0, XYZW); end();
    op(MOV); dst(OD, 0, 0xF); src(V, 1, XYZW); end();
    op(MOV); dst(OT, 0, 0xF); src(V, 2, XYZW); end();
    finish();
    IDirect3DVertexShader9 *vs = NULL;
    ok(IDirect3DDevice9_CreateVertexShader(dev, code, &vs) == D3D_OK && vs, "CreateVertexShader(vs_2_0)");

    /* ps_2_0: r0 = tex(t0) * v0 * c0; kill where t0 - c1 < 0 (the top-left three quarters); oC0 = r0 */
    begin(0, 2, 0);
    dcl(D3DDECLUSAGE_COLOR, 0, V, 0); dcl(D3DDECLUSAGE_TEXCOORD, 0, T, 0); dcl_sampler(0);
    op(TEXLD); dst(R, 0, 0xF); src(T, 0, XYZW); src(SMP, 0, XYZW); end();
    op(MUL); dst(R, 0, 0xF); src(R, 0, XYZW); src(V, 0, XYZW); end();
    op(MUL); dst(R, 0, 0xF); src(R, 0, XYZW); src(C, 0, XYZW); end();
    op(ADD); dst(R, 1, 0xF); src(T, 0, XYZW); src(C, 1, XYZW); end();
    op(TEXKILL); dst(R, 1, 0xF); end();
    op(MOV); dst(OC, 0, 0xF); src(R, 0, XYZW); end();
    finish();
    IDirect3DPixelShader9 *ps = NULL;
    ok(IDirect3DDevice9_CreatePixelShader(dev, code, &ps) == D3D_OK && ps, "CreatePixelShader(ps_2_0)");

    /* the constants: an orthographic matrix from pixels to clip space, a tint, the kill threshold */
    float m[16] = { 2.0f / W, 0, 0, -1,   0, -2.0f / H, 0, 1,   0, 0, 1, 0,   0, 0, 0, 1 };
    ok(IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, m, 4) == D3D_OK, "SetVertexShaderConstantF(c0..c3)");
    float back[16]; memset(back, 0, sizeof back);
    ok(IDirect3DDevice9_GetVertexShaderConstantF(dev, 0, back, 4) == D3D_OK && memcmp(back, m, sizeof m) == 0, "GetVertexShaderConstantF reads them back");
    float tint[4] = { 1.0f, 1.0f, 0.5f, 1.0f }, kill[4] = { -0.5f, -0.5f, 0.0f, 0.0f };
    ok(IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1) == D3D_OK, "SetPixelShaderConstantF(c0)");
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, kill, 1);
    BOOL b = TRUE; ok(IDirect3DDevice9_SetVertexShaderConstantB(dev, 3, &b, 1) == D3D_OK, "SetVertexShaderConstantB");
    BOOL bb = FALSE; ok(IDirect3DDevice9_GetVertexShaderConstantB(dev, 3, &bb, 1) == D3D_OK && bb, "  and it reads back");

    D3DVERTEXELEMENT9 elems[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    ok(IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl) == D3D_OK && decl, "CreateVertexDeclaration");

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(32, 32, 32), 1.0f, 0);
    ok(IDirect3DDevice9_BeginScene(dev) == D3D_OK, "BeginScene");

    /* quad 1: both shaders. 96x96 at (16, 24), white vertices, two texture repeats */
    struct Vtx q1[4] = {
        {  16,  24, 0, 0xFFFFFFFFu, 0, 0 }, { 112,  24, 0, 0xFFFFFFFFu, 2, 0 },
        {  16, 120, 0, 0xFFFFFFFFu, 0, 2 }, { 112, 120, 0, 0xFFFFFFFFu, 2, 2 },
    };
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    ok(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q1, sizeof q1[0]) == D3D_OK, "DrawPrimitiveUP through vs_2_0 and ps_2_0");

    /* quad 2: pixel shader only, transformed vertices, green tint by vertex colour */
    struct VtxT q2[4] = {
        { 128,  24, 0, 1, 0xFF00FF00u, 0, 0 }, { 176,  24, 0, 1, 0xFF00FF00u, 1, 0 },
        { 128,  72, 0, 1, 0xFF00FF00u, 0, 1 }, { 176,  72, 0, 1, 0xFF00FF00u, 1, 1 },
    };
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    ok(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q2, sizeof q2[0]) == D3D_OK, "DrawPrimitiveUP through ps_2_0 alone");

    /* quad 3: vertex shader only, fixed-function texture times a half-bright colour */
    struct Vtx q3[4] = {
        { 128,  80, 0, 0xFF808080u, 0, 0 }, { 240,  80, 0, 0xFF808080u, 1, 0 },
        { 128, 136, 0, 0xFF808080u, 0, 1 }, { 240, 136, 0, 0xFF808080u, 1, 1 },
    };
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    ok(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q3, sizeof q3[0]) == D3D_OK, "DrawPrimitiveUP through vs_2_0 alone");

    ok(IDirect3DDevice9_EndScene(dev) == D3D_OK, "EndScene");
    ok(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) == D3D_OK, "Present");

    /* look at the back buffer: quad 1's kept quarter is tinted red/blue checks, the killed part is background */
    IDirect3DSurface9 *bb2 = NULL;
    if (IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb2) == D3D_OK && bb2) {
        D3DLOCKED_RECT r;
        if (IDirect3DSurface9_LockRect(bb2, &r, NULL, D3DLOCK_READONLY) == D3D_OK) {
            DWORD killed = ((DWORD *)((BYTE *)r.pBits + 30 * r.Pitch))[20] & 0xFFFFFF;
            DWORD kept = ((DWORD *)((BYTE *)r.pBits + 110 * r.Pitch))[100] & 0xFFFFFF;
            DWORD q2px = ((DWORD *)((BYTE *)r.pBits + 30 * r.Pitch))[134] & 0xFFFFFF;
            ok(killed == 0x202020, "texkill left the background where t0 < 0.5");
            ok(kept == 0xFF0000 || kept == 0x000080, "the kept quarter is the texture, tinted (red, or blue at half)");
            ok(q2px == 0x000000 || q2px == 0x008000, "the pixel-shader-only quad is the texture times green");
            printf("  pixels: killed %06lx kept %06lx quad2 %06lx\n", (unsigned long)killed, (unsigned long)kept, (unsigned long)q2px);
            IDirect3DSurface9_UnlockRect(bb2);
        }
        IDirect3DSurface9_Release(bb2);
    }

    IDirect3DVertexDeclaration9_Release(decl);
    IDirect3DPixelShader9_Release(ps);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DTexture9_Release(tex);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
