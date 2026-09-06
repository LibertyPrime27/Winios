/* Direct3D 9 from a Windows program, the way a game starts a frame.
 *
 * Create the interface, ask what the adapter is, make a device with a back
 * buffer, clear it, present it, then lock the back buffer and read the pixels
 * back. Everything here goes through COM vtables in guest memory, so it is
 * also the test that the vtable slot numbers are right: a wrong slot is an
 * indirect call to the wrong function, and the output would say so.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

int main(void) {
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    printf("Direct3DCreate9: %s\n", d3d ? "ok" : "FAILED");
    if (!d3d) return 1;
    printf("adapters: %u\n", IDirect3D9_GetAdapterCount(d3d));

    D3DADAPTER_IDENTIFIER9 id;
    if (IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id) == D3D_OK)
        printf("adapter: %s (vendor %#x)\n", id.Description, (unsigned)id.VendorId);

    D3DDISPLAYMODE dm;
    if (IDirect3D9_GetAdapterDisplayMode(d3d, 0, &dm) == D3D_OK)
        printf("display mode: %ux%u fmt %u\n", dm.Width, dm.Height, dm.Format);

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth  = 64;
    pp.BackBufferHeight = 32;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.Windowed         = TRUE;

    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice: %s\n", hr == D3D_OK && dev ? "ok" : "FAILED");
    if (hr != D3D_OK || !dev) return 1;

    printf("TestCooperativeLevel: %s\n", IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK ? "ok" : "FAILED");

    /* the slot numbers have to be right for these to land anywhere sane */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);

    hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x30, 0x60, 0x90), 1.0f, 0);
    printf("Clear: %s\n", hr == D3D_OK ? "ok" : "FAILED");
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_EndScene(dev);
    hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    printf("Present: %s\n", hr == D3D_OK ? "ok" : "FAILED");

    IDirect3DSurface9 *bb = NULL;
    hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    printf("GetBackBuffer: %s\n", hr == D3D_OK && bb ? "ok" : "FAILED");
    if (!bb) return 1;

    D3DSURFACE_DESC sd;
    if (IDirect3DSurface9_GetDesc(bb, &sd) == D3D_OK)
        printf("back buffer: %ux%u fmt %u\n", sd.Width, sd.Height, sd.Format);

    D3DLOCKED_RECT lr;
    hr = IDirect3DSurface9_LockRect(bb, &lr, NULL, D3DLOCK_READONLY);
    printf("LockRect: %s pitch %d\n", hr == D3D_OK ? "ok" : "FAILED", (int)lr.Pitch);
    if (hr == D3D_OK) {
        const DWORD *px = (const DWORD *)lr.pBits;
        DWORD first = px[0], last = *(const DWORD *)((const char *)lr.pBits + (size_t)lr.Pitch * (sd.Height - 1)
                                                     + (sd.Width - 1) * 4);
        printf("pixel(0,0)   = %08lx\n", (unsigned long)first);
        printf("pixel(63,31) = %08lx\n", (unsigned long)last);
        printf("cleared uniformly: %s\n", first == last && first == 0xFF306090 ? "yes" : "NO");
        IDirect3DSurface9_UnlockRect(bb);
    }

    IDirect3DSurface9_Release(bb);
    printf("device refs after Release: %lu\n", (unsigned long)IDirect3DDevice9_Release(dev));
    printf("d3d refs after Release: %lu\n", (unsigned long)IDirect3D9_Release(d3d));
    return 0;
}
