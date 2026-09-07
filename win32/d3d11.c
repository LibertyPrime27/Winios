/* d3d11.dll -- the one that is not implemented, said out loud.
 *
 * A modern GameMaker, Unity or Unreal game on Windows draws everything
 * through Direct3D 11. There is no Direct3D 11 here: d3d9.c exists and goes
 * through a software rasterizer, and D3D10/11/12 do not exist at all. That
 * is the largest single thing standing between this project and a game that
 * runs, and it is a rendering API rather than a function -- a device, a
 * context, a swap chain, buffers, textures, states, and HLSL shader
 * bytecode translated into something a GPU here can run. The last of those
 * is the hard part and it is not a stub.
 *
 * So why is this file here at all?
 *
 * Because there are two ways for a game to fail on D3D11CreateDevice, and
 * only one of them is any use. Without this file the import does not
 * resolve, the run stops at the call with a stub address in the report, and
 * everything the game would have done *after* creating a device -- every
 * other function it needs, which is the list we are working from -- is never
 * reached and never reported. With it, the call fails the way a real machine
 * with no suitable adapter fails, the game takes its own error path, says so
 * in its own words, and the run report names the rest of what it wanted.
 *
 * That is the difference between "it stopped somewhere" and a work queue.
 * DXGI_ERROR_UNSUPPORTED is exactly what Windows returns when no device on
 * the machine can do what was asked, and it is the truth here.
 *
 * Nothing in this file should ever be described as an implementation of
 * Direct3D 11. When there is one, it will not look like this.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <stdio.h>

enum {
    DXGI_ERROR_UNSUPPORTED_       = (int)0x887A0004,
    DXGI_ERROR_NOT_FOUND_         = (int)0x887A0002,
    D3D11_ERROR_FILE_NOT_FOUND_   = (int)0x887C0002,
};

static void nulls(w32 *w, int first, int count) {
    for (int i = first; i < first + count; i++)
        if (ARG((unsigned)i)) w32_write(w, ARG((unsigned)i), (int)w32_ptrsize(w), 0);
}

/* D3D11CreateDevice(adapter, driverType, software, flags, featureLevels,
 *                   numFeatureLevels, sdkVersion, ppDevice, pFeatureLevel,
 *                   ppContext) */
static void d_D3D11CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d11!D3D11CreateDevice -- Direct3D 11 is not implemented "
                        "(this is the largest missing piece, not an oversight)");
    nulls(w, 7, 1);
    if (ARG(8)) w32_write(w, ARG(8), 4, 0);
    nulls(w, 9, 1);
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}
static void d_D3D11CreateDeviceAndSwapChain(w32 *w) {
    w32_note_refused(w, "d3d11!D3D11CreateDeviceAndSwapChain -- Direct3D 11 is not implemented");
    nulls(w, 8, 4);
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}
static void d_D3D11On12CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d11!D3D11On12CreateDevice -- Direct3D 11 is not implemented");
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}

#define F(n, a) { #n, a, 0, d_##n, 0 }
const w32_api w32_d3d11[] = {
    F(D3D11CreateDevice, 10), F(D3D11CreateDeviceAndSwapChain, 12),
    F(D3D11On12CreateDevice, 11),
    { 0, 0, 0, 0, 0 },
};
#undef F

/* dxgi, d3d10 and d3d12 fail the same way and for the same reason. They are
 * here so that a game which asks for one of them gets past the asking. */
static void x_CreateDXGIFactory(w32 *w)   { nulls(w, 1, 1); RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_); }
static void x_CreateDXGIFactory1(w32 *w)  { nulls(w, 1, 1); RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_); }
static void x_CreateDXGIFactory2(w32 *w)  { nulls(w, 2, 1); RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_); }

#define X(n, a) { #n, a, 0, x_##n, 0 }
const w32_api w32_dxgi[] = {
    X(CreateDXGIFactory, 2), X(CreateDXGIFactory1, 2), X(CreateDXGIFactory2, 3),
    { 0, 0, 0, 0, 0 },
};
#undef X

static void t_D3D10CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d10!D3D10CreateDevice -- Direct3D 10 is not implemented");
    nulls(w, 5, 1);
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}
static void t_D3D12CreateDevice(w32 *w) {
    w32_note_refused(w, "d3d12!D3D12CreateDevice -- Direct3D 12 is not implemented");
    nulls(w, 3, 1);
    RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_);
}
static void t_D3D12GetDebugInterface(w32 *w) { nulls(w, 1, 1); RET((uint64_t)(uint32_t)DXGI_ERROR_UNSUPPORTED_); }

const w32_api w32_d3d10[] = {
    { "D3D10CreateDevice", 6, 0, t_D3D10CreateDevice, 0 },
    { "D3D10CreateDeviceAndSwapChain", 8, 0, t_D3D10CreateDevice, 0 },
    { 0, 0, 0, 0, 0 },
};
const w32_api w32_d3d12[] = {
    { "D3D12CreateDevice", 4, 0, t_D3D12CreateDevice, 0 },
    { "D3D12GetDebugInterface", 2, 0, t_D3D12GetDebugInterface, 0 },
    { 0, 0, 0, 0, 0 },
};
