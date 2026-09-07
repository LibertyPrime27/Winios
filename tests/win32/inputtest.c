/* A window, a message pump, and a keyboard and mouse.
 *
 * Shaped like a game's startup and frame loop, because that is the thing
 * being tested: register a class, create a window, then each frame drain the
 * message queue, poll the key and pointer state, and present. Input arrives
 * from outside (`winrun -input <script>`, or a finger on the device), so the
 * frame number is what makes it deterministic -- the script says "at frame 3,
 * press W" and the recording says what frame 3 saw.
 *
 * Both ways of reading input are checked, because games use both. The message
 * queue is what a WndProc sees; GetAsyncKeyState and GetCursorPos are what a
 * frame loop asks. An event must show up in both.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_wm_create, g_wm_size, g_wm_activate;
static int g_proc_calls;
static int g_input_seen;        /* any key or mouse event at all */
static char g_log[64][96];
static int g_nlog;

static void logf_(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (g_nlog < 64) vsnprintf(g_log[g_nlog++], 96, fmt, ap);
    va_end(ap);
}

static const char *msgname(UINT m) {
    switch (m) {
    case WM_CREATE: return "WM_CREATE";
    case WM_SIZE: return "WM_SIZE";
    case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_CHAR: return "WM_CHAR";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    case WM_SYSKEYUP: return "WM_SYSKEYUP";
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP";
    case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
    case WM_RBUTTONUP: return "WM_RBUTTONUP";
    case WM_MBUTTONDOWN: return "WM_MBUTTONDOWN";
    case WM_MBUTTONUP: return "WM_MBUTTONUP";
    case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    case WM_CLOSE: return "WM_CLOSE";
    case WM_DESTROY: return "WM_DESTROY";
    case WM_QUIT: return "WM_QUIT";
    case WM_PAINT: return "WM_PAINT";
    default: return NULL;
    }
}

/* The WndProc is guest code the runtime calls back into, which is the part of
 * DispatchMessage worth testing at all. */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    g_proc_calls++;
    switch (m) {
    case WM_CREATE:   g_wm_create++;   return 0;
    case WM_SIZE:     g_wm_size++;     logf_("  wndproc: WM_SIZE to %dx%d", (int)LOWORD(lp), (int)HIWORD(lp)); return 0;
    case WM_ACTIVATE: g_wm_activate++; return 0;
    case WM_KEYDOWN:  logf_("  wndproc: WM_KEYDOWN vk %d", (int)wp); return 0;
    case WM_CHAR:     logf_("  wndproc: WM_CHAR '%c'", (int)wp); return 0;
    case WM_LBUTTONDOWN: logf_("  wndproc: WM_LBUTTONDOWN at %d,%d", (int)(short)LOWORD(lp), (int)(short)HIWORD(lp)); return 0;
    default: return DefWindowProcA(h, m, wp, lp);
    }
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 6;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "winiostest";
    ATOM a = RegisterClassA(&wc);
    printf("RegisterClass: %s\n", a ? "ok" : "FAILED");

    HWND hwnd = CreateWindowExA(0, "winiostest", "input test", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
    printf("CreateWindowEx: %s\n", hwnd ? "ok" : "FAILED");
    if (!hwnd) return 1;
    printf("WM_CREATE reached the WndProc before CreateWindowEx returned: %s\n",
           g_wm_create ? "yes" : "NO");
    ShowWindow(hwnd, SW_SHOW);
    printf("IsWindowVisible: %d\n", (int)IsWindowVisible(hwnd));

    RECT rc;
    GetClientRect(hwnd, &rc);
    printf("client rect: %ldx%ld\n", (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
    printf("screen: %dx%d, mouse present %d, buttons %d\n",
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_MOUSEPRESENT), GetSystemMetrics(SM_CMOUSEBUTTONS));

    /* a device, so Present exists and a frame is a moment the script can
     * aim at -- the same shape as a game's loop */
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 FAILED\n"); return 1; }
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice9 *dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
        printf("CreateDevice FAILED\n"); return 1;
    }

    /* Drain what creating the window queued, so the frames below start clean.
     * Naming them is the test: the order a program is entitled to. */
    printf("\nqueued by window creation:\n");
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        const char *n = msgname(msg.message);
        printf("  %s\n", n ? n : "(other)");
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    printf("WndProc saw WM_SIZE %d, WM_ACTIVATE %d\n", g_wm_size, g_wm_activate);
    for (int i = 0; i < g_nlog; i++) puts(g_log[i]);
    g_nlog = 0;

    int quit = 0;
    for (int f = 1; f <= frames && !quit; f++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF102030, 1.0f, 0);
        if (IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) != D3D_OK) break;

        printf("\nframe %d:\n", f);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            const char *n = msgname(msg.message);
            if (msg.message == WM_QUIT) { printf("  WM_QUIT, code %d\n", (int)msg.wParam); quit = 1; break; }
            if (msg.message >= WM_KEYDOWN && msg.message <= WM_MOUSELAST) g_input_seen++;
            if (msg.message == WM_MOUSEMOVE || msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONUP ||
                msg.message == WM_RBUTTONDOWN || msg.message == WM_RBUTTONUP)
                printf("  %s at %d,%d keys %#x\n", n ? n : "(other)",
                       (int)(short)LOWORD(msg.lParam), (int)(short)HIWORD(msg.lParam), (unsigned)msg.wParam);
            else if (msg.message == WM_MOUSEWHEEL)
                printf("  %s delta %d\n", n ? n : "(other)", (int)(short)HIWORD(msg.wParam));
            else if (msg.message == WM_CHAR)
                printf("  %s '%c'\n", n ? n : "(other)", (int)msg.wParam);
            else
                printf("  %s vk %d\n", n ? n : "(other)", (int)msg.wParam);
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        for (int i = 0; i < g_nlog; i++) puts(g_log[i]);
        g_nlog = 0;

        /* what a frame loop actually asks */
        POINT pt;
        GetCursorPos(&pt);
        printf("  polled: W %d A %d S %d D %d shift %d, cursor %ld,%ld, buttons L%d R%d\n",
               (GetAsyncKeyState('W') & 0x8000) != 0, (GetAsyncKeyState('A') & 0x8000) != 0,
               (GetAsyncKeyState('S') & 0x8000) != 0, (GetAsyncKeyState('D') & 0x8000) != 0,
               (GetKeyState(VK_SHIFT) & 0x8000) != 0,
               (long)pt.x, (long)pt.y,
               (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
               (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    }

    printf("\nWndProc was called %s\n", g_proc_calls > 0 ? "yes" : "NO - DispatchMessage never reached the guest");
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
    printf("IsWindow after DestroyWindow: %d\n", (int)IsWindow(hwnd));
    /* The exit code carries the one fact a caller that is not comparing
     * output still needs: input reached the guest. Without it a harness that
     * only checks the exit code would pass on a run that received nothing. */
    if (!g_input_seen) { printf("FAILED - no key or mouse event arrived\n"); return 2; }
    return 0;
}
