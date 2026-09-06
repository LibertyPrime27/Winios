/* A Windows program that produces a frame and presents it.
 *
 * The Direct3D 9 device, its back buffer and Present are real; the pixels are
 * drawn by the guest's own code through a locked surface, because
 * DrawPrimitive is not implemented yet. That is how a software-rendered intro
 * or a video player puts a frame on screen, and it exercises the whole path
 * the GPU one will use: guest x86 computes the pixels (on the dynarec), they
 * land in guest memory the host can see, and Present hands them over.
 *
 * Integer arithmetic only, so a PE32 and a PE32+ build produce identical
 * bytes and the checksum can be recorded.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define W 320
#define H 180

static DWORD shade(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
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

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 10, 24), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    IDirect3DSurface9 *bb = NULL;
    if (IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) != D3D_OK || !bb) {
        printf("GetBackBuffer failed\n"); return 1;
    }
    D3DLOCKED_RECT lr;
    if (IDirect3DSurface9_LockRect(bb, &lr, NULL, 0) != D3D_OK) { printf("LockRect failed\n"); return 1; }

    for (int y = 0; y < H; y++) {
        DWORD *row = (DWORD *)((char *)lr.pBits + (size_t)y * lr.Pitch);
        for (int x = 0; x < W; x++) {
            /* a sky that gets warmer towards the bottom right */
            int r = 20 + (x * 90) / W + (y * 60) / H;
            int g = 30 + (y * 40) / H;
            int b = 90 + (140 * (H - y)) / H - (x * 40) / W;

            /* a disc, drawn with integer distance so both builds agree */
            int dx = x - W / 2, dy = y - H / 2 - 10;
            int d2 = dx * dx + dy * dy;
            if (d2 < 46 * 46) { int k = (46 * 46 - d2) / 96; r += k; g += k / 2; b -= k / 3; }
            else if (d2 < 50 * 50) { r += 40; g += 40; b += 40; }

            /* a checkerboard strip along the bottom, so scaling artefacts show */
            if (y > H - 20 && ((x >> 3) + (y >> 3)) & 1) { r -= 30; g -= 30; b -= 30; }

            row[x] = shade(r, g, b);
        }
    }

    /* FNV-1a over the pixels: one number that says the frame is byte-exact */
    unsigned long long h = 1469598103934665603ULL;
    for (int y = 0; y < H; y++) {
        const unsigned char *row = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        for (int i = 0; i < W * 4; i++) { h ^= row[i]; h *= 1099511628211ULL; }
    }

    IDirect3DSurface9_UnlockRect(bb);
    IDirect3DSurface9_Release(bb);
    IDirect3DDevice9_EndScene(dev);
    HRESULT hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    printf("frame %dx%d presented: %s\n", W, H, hr == D3D_OK ? "ok" : "FAILED");
    printf("checksum %016llx\n", h);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
