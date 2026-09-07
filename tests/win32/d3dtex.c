/* A sprite, the way a 2D game draws one.
 *
 * This is the case that mattered and did not work: CreateTexture, fill it
 * through LockRect, bind it with SetTexture, and draw a textured quad. Until
 * the texture object existed, CreateTexture was not in the device's vtable at
 * all, so the call landed on the unimplemented-slot stub and the run stopped
 * there -- before a game had drawn its first frame.
 *
 * Three things are exercised deliberately. The quad is D3DFVF_XYZRHW |
 * D3DFVF_DIFFUSE | D3DFVF_TEX1: a position already in screen space, a colour
 * to modulate by, and a texture coordinate, which is exactly what a sprite
 * engine sends. The second quad goes through the transform pipeline instead
 * -- D3DFVF_XYZ with a projection matrix -- because that is the other half of
 * the vertex path and it had never run. And the third is drawn with alpha
 * blending on, because a sprite with a transparent border is the normal case
 * and drawing it opaque is the visible failure.
 *
 * The frame checksum is what makes it a test: the same guest drawing the same
 * sprites has to produce the same pixels on a Linux runner, under qemu on
 * aarch64 and on the iPad, because every step of it is integer arithmetic.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define W 256
#define H 144
#define TS 8                       /* the texture is TS x TS */

static int fails;
static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

struct VtxT { float x, y, z, rhw; DWORD color; float u, v; };
struct Vtx3 { float x, y, z; DWORD color; float u, v; };

/* Row-major, as D3DMATRIX is. An orthographic projection that maps the unit
 * square onto the whole target, so the transformed quad lands somewhere
 * predictable without needing a camera. */
static void ortho(D3DMATRIX *m) {
    memset(m, 0, sizeof *m);
    m->_11 = 1.0f; m->_22 = 1.0f; m->_33 = 1.0f; m->_44 = 1.0f;
}

int main(void) {
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;

    IDirect3DDevice9 *dev = NULL;
    if (IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev) != D3D_OK || !dev) {
        printf("CreateDevice failed\n"); return 1;
    }
    ok(1, "CreateDevice");

    /* --- the texture -------------------------------------------------- */
    IDirect3DTexture9 *tex = NULL;
    HRESULT hr = IDirect3DDevice9_CreateTexture(dev, TS, TS, 1, 0, D3DFMT_A8R8G8B8,
                                                D3DPOOL_MANAGED, &tex, NULL);
    ok(hr == D3D_OK && tex != NULL, "CreateTexture");
    if (!tex) { printf("nothing more can be checked\n"); return 1; }

    D3DSURFACE_DESC desc;
    memset(&desc, 0, sizeof desc);
    ok(IDirect3DTexture9_GetLevelDesc(tex, 0, &desc) == D3D_OK
       && (int)desc.Width == TS && (int)desc.Height == TS,
       "GetLevelDesc reports the size it was created with");

    D3DLOCKED_RECT lr;
    memset(&lr, 0, sizeof lr);
    ok(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0) == D3D_OK && lr.pBits != NULL,
       "LockRect gives somewhere to write");
    if (lr.pBits) {
        /* A checkerboard, so a wrong texture coordinate is obvious rather
         * than merely wrong-coloured. Half of it transparent, so the blend
         * has something to blend. */
        for (int y = 0; y < TS; y++) {
            DWORD *row = (DWORD *)((BYTE *)lr.pBits + (size_t)y * lr.Pitch);
            for (int x = 0; x < TS; x++) {
                int on = ((x >> 1) + (y >> 1)) & 1;
                DWORD a = y < TS / 2 ? 0xFF000000u : 0x80000000u;
                row[x] = a | (on ? 0x00E0C040u : 0x004060E0u);
            }
        }
    }
    ok(IDirect3DTexture9_UnlockRect(tex, 0) == D3D_OK, "UnlockRect");

    IDirect3DSurface9 *lvl = NULL;
    ok(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &lvl) == D3D_OK && lvl != NULL,
       "GetSurfaceLevel");
    if (lvl) IDirect3DSurface9_Release(lvl);

    ok(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex) == D3D_OK,
       "SetTexture");
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    /* --- draw ---------------------------------------------------------- */
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 32), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    /* A screen-space textured quad, modulated by a per-vertex colour. Two
     * triangles as a strip, and the texture coordinates run 0..2 so the wrap
     * mode is exercised as well. */
    struct VtxT quad[4] = {
        {  16.0f,  16.0f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { 112.0f,  16.0f, 0.0f, 1.0f, 0xFFFF80FF, 2.0f, 0.0f },
        {  16.0f, 112.0f, 0.0f, 1.0f, 0xFF80FFFF, 0.0f, 2.0f },
        { 112.0f, 112.0f, 0.0f, 1.0f, 0xFFFFFF80, 2.0f, 2.0f },
    };
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    ok(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]) == D3D_OK,
       "a screen-space textured quad");

    /* The same texture through the transform pipeline. The matrices are
     * identity, so a vertex at -0.5..0.5 covers the middle half of the
     * target -- which is only true if the viewport map and the perspective
     * divide are both right. */
    D3DMATRIX m;
    ortho(&m);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &m);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &m);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &m);
    struct Vtx3 tri[3] = {
        {  0.10f, -0.10f, 0.5f, 0xFF60FF60, 0.0f, 0.0f },
        {  0.90f, -0.10f, 0.5f, 0xFF60FF60, 1.0f, 0.0f },
        {  0.10f, -0.90f, 0.5f, 0xFF60FF60, 0.0f, 1.0f },
    };
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    ok(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]) == D3D_OK,
       "a transformed textured triangle");

    /* And once more with blending on, over what is already there. */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    struct VtxT over[4] = {
        { 140.0f,  30.0f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { 230.0f,  30.0f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { 140.0f, 120.0f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { 230.0f, 120.0f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    ok(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, over, sizeof over[0]) == D3D_OK,
       "a blended textured quad");

    /* --- indexed, out of real buffers ---------------------------------- */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    ok(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof quad, 0,
           D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, D3DPOOL_DEFAULT, &vb, NULL) == D3D_OK && vb,
       "CreateVertexBuffer");
    ok(IDirect3DDevice9_CreateIndexBuffer(dev, 6 * sizeof(WORD), 0, D3DFMT_INDEX16,
                                          D3DPOOL_DEFAULT, &ib, NULL) == D3D_OK && ib,
       "CreateIndexBuffer");
    if (vb && ib) {
        struct VtxT far_[4] = {
            {  20.0f, 118.0f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
            { 100.0f, 118.0f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
            {  20.0f, 140.0f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
            { 100.0f, 140.0f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
        };
        void *p = NULL;
        if (IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0) == D3D_OK && p) {
            memcpy(p, far_, sizeof far_);
            IDirect3DVertexBuffer9_Unlock(vb);
        }
        WORD idx[6] = { 0, 1, 2, 2, 1, 3 };
        p = NULL;
        if (IDirect3DIndexBuffer9_Lock(ib, 0, 0, &p, 0) == D3D_OK && p) {
            memcpy(p, idx, sizeof idx);
            IDirect3DIndexBuffer9_Unlock(ib);
        }
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct VtxT));
        IDirect3DDevice9_SetIndices(dev, ib);
        ok(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2) == D3D_OK,
           "DrawIndexedPrimitive out of a vertex and an index buffer");
    }

    IDirect3DDevice9_EndScene(dev);
    ok(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) == D3D_OK, "Present");

    if (ib) IDirect3DIndexBuffer9_Release(ib);
    if (vb) IDirect3DVertexBuffer9_Release(vb);
    IDirect3DTexture9_Release(tex);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
