/* A guest that keeps drawing until the host tells it to stop.
 *
 * This is the shape of a game's frame loop rather than a one-shot test: clear,
 * draw, present, repeat. The host stops it by making Present return
 * D3DERR_DEVICELOST -- which is what a real driver returns when the display
 * mode changes or the machine sleeps, so a program that checks its Present
 * result exits on its own and nothing has to reach into a running guest.
 *
 * argv[1] caps the frame count so the same program is also a headless test;
 * with no argument it runs until the device is lost.
 *
 * Integer arithmetic throughout, so a PE32 and a PE32+ build produce
 * identical frames and the checksum can be recorded.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

#define W 320
#define H 180

static DWORD shade(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}

/* A disc on a lissajous path over a gradient, positioned from the frame
 * number so every frame differs and the motion is checkable. */
static void draw(void *bits, int pitch, int frame) {
    int cx = W / 2 + (int)((W / 3) * (frame % 120 < 60 ? frame % 120 - 30 : 90 - frame % 120)) / 30;
    int cy = H / 2 + (int)((H / 4) * (frame % 80 < 40 ? frame % 80 - 20 : 60 - frame % 80)) / 20;
    for (int y = 0; y < H; y++) {
        DWORD *row = (DWORD *)((char *)bits + (size_t)y * pitch);
        for (int x = 0; x < W; x++) {
            int r = 20 + (x * 90) / W + (y * 60) / H;
            int g = 30 + (y * 40) / H;
            int b = 90 + (140 * (H - y)) / H - (x * 40) / W;
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 < 34 * 34) { int k = (34 * 34 - d2) / 60; r += k; g += k / 2; b -= k / 3; }
            else if (d2 < 38 * 38) { r += 50; g += 50; b += 50; }
            if (y > H - 16 && (((x + frame) >> 3) + (y >> 3)) & 1) { r -= 30; g -= 30; b -= 30; }
            row[x] = shade(r, g, b);
        }
    }
}

int main(int argc, char **argv) {
    long limit = argc > 1 ? atol(argv[1]) : 0;          /* 0 = until the device is lost */

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

    IDirect3DSurface9 *bb = NULL;
    if (IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) != D3D_OK || !bb) {
        printf("GetBackBuffer failed\n"); return 1;
    }

    unsigned long long sum = 1469598103934665603ULL;
    long frame = 0;
    for (;; frame++) {
        if (limit && frame >= limit) break;
        IDirect3DDevice9_BeginScene(dev);
        D3DLOCKED_RECT lr;
        if (IDirect3DSurface9_LockRect(bb, &lr, NULL, 0) != D3D_OK) { printf("LockRect failed\n"); break; }
        draw(lr.pBits, (int)lr.Pitch, (int)frame);
        /* fold only the first row in: enough to prove the frames differ,
           cheap enough not to dominate the loop */
        const unsigned char *row = (const unsigned char *)lr.pBits;
        for (int i = 0; i < W * 4; i++) { sum ^= row[i]; sum *= 1099511628211ULL; }
        IDirect3DSurface9_UnlockRect(bb);
        IDirect3DDevice9_EndScene(dev);
        if (IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) != D3D_OK) break;   /* device lost: the host asked us to stop */
    }

    printf("%ld frames\n", frame);
    printf("checksum %016llx\n", sum);
    IDirect3DSurface9_Release(bb);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
