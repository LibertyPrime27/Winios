/* Geometry, drawn.
 *
 * Two triangles from a vertex buffer and one from DrawPrimitiveUP, in
 * D3DFVF_XYZRHW | D3DFVF_DIFFUSE -- positions already in screen space, so
 * this exercises vertex fetch, primitive assembly and rasterization without
 * needing the transform pipeline that does not exist yet.
 *
 * The frame checksum is what makes it a test: the same guest, drawing the
 * same triangles, has to produce the same pixels on a Linux runner, under
 * qemu-aarch64 and on the iPad.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define W 256
#define H 144

struct Vtx { float x, y, z, rhw; DWORD color; };

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

    /* a quad as two triangles, each corner a different colour */
    static const struct Vtx quad[6] = {
        {  20.0f,  20.0f, 0.0f, 1.0f, 0xFFFF0000 },
        { 150.0f,  30.0f, 0.0f, 1.0f, 0xFF00FF00 },
        {  40.0f, 120.0f, 0.0f, 1.0f, 0xFF0000FF },
        { 150.0f,  30.0f, 0.0f, 1.0f, 0xFF00FF00 },
        { 170.0f, 130.0f, 0.0f, 1.0f, 0xFFFFFF00 },
        {  40.0f, 120.0f, 0.0f, 1.0f, 0xFF0000FF },
    };
    IDirect3DVertexBuffer9 *vb = NULL;
    HRESULT hr = IDirect3DDevice9_CreateVertexBuffer(dev, sizeof quad, 0,
                     D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb, NULL);
    printf("CreateVertexBuffer: %s\n", hr == D3D_OK && vb ? "ok" : "FAILED");
    if (!vb) return 1;

    void *dst = NULL;
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, sizeof quad, &dst, 0);
    printf("Lock: %s\n", hr == D3D_OK && dst ? "ok" : "FAILED");
    if (!dst) return 1;
    memcpy(dst, quad, sizeof quad);
    IDirect3DVertexBuffer9_Unlock(vb);

    D3DVERTEXBUFFER_DESC vd;
    if (IDirect3DVertexBuffer9_GetDesc(vb, &vd) == D3D_OK)
        printf("buffer: %u bytes, fvf %#x\n", (unsigned)vd.Size, (unsigned)vd.FVF);

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 12, 30), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vtx));
    hr = IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    printf("DrawPrimitive (2 triangles from a buffer): %s\n", hr == D3D_OK ? "ok" : "FAILED");

    /* and one straight from guest memory, no buffer object involved */
    static const struct Vtx tri[3] = {
        { 190.0f,  20.0f, 0.0f, 1.0f, 0xFFFFFFFF },
        { 240.0f,  70.0f, 0.0f, 1.0f, 0xFFFF00FF },
        { 190.0f, 120.0f, 0.0f, 1.0f, 0xFF00FFFF },
    };
    hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(struct Vtx));
    printf("DrawPrimitiveUP (1 triangle): %s\n", hr == D3D_OK ? "ok" : "FAILED");
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    /* read the result back and checksum it */
    IDirect3DSurface9 *bb = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    D3DLOCKED_RECT lr;
    if (bb && IDirect3DSurface9_LockRect(bb, &lr, NULL, D3DLOCK_READONLY) == D3D_OK) {
        unsigned long long h = 1469598103934665603ULL;
        int lit = 0;
        for (int y = 0; y < H; y++) {
            const unsigned char *row = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
            const DWORD *px = (const DWORD *)row;
            for (int x = 0; x < W; x++) if ((px[x] & 0xFFFFFF) != 0x0A0C1E) lit++;
            for (int i = 0; i < W * 4; i++) { h ^= row[i]; h *= 1099511628211ULL; }
        }
        printf("pixels covered by geometry: %d\n", lit);
        printf("centre of the quad: %08lx\n", (unsigned long)((const DWORD *)((const char *)lr.pBits + (size_t)70 * lr.Pitch))[90]);
        printf("checksum %016llx\n", h);
        IDirect3DSurface9_UnlockRect(bb);
        IDirect3DSurface9_Release(bb);
    }

    IDirect3DVertexBuffer9_Release(vb);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
