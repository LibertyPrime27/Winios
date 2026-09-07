/* Not a test of the emulator -- a test of the *report*.
 *
 * This calls the set of things a game touches in its first few seconds: make a
 * window and pump its messages, spin up a thread, take a lock, read the
 * registry, walk a directory, memory-map a file, time a frame, play a sound,
 * and query the display. Almost none of that is implemented, which is the
 * point: `winrun -imports` on this should name every one of them, and the list
 * it prints is the shape of the work between here and running a real game.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

static DWORD WINAPI worker(LPVOID p) { (void)p; return 0; }
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    /* a window and a message pump */
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "winios";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "winios", "winios", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE);
    TranslateMessage(&msg);
    DispatchMessageA(&msg);

    /* threads and synchronisation */
    DWORD tid = 0;
    HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    WaitForSingleObject(th, 1000);
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetEvent(ev);
    InterlockedIncrement(&tid);

    /* timing */
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    Sleep(0);

    /* files and the registry */
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("*.esm", &fd);
    if (find != INVALID_HANDLE_VALUE) FindNextFileA(find, &fd), FindClose(find);
    HANDLE fh = CreateFileA("data.bsa", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    HKEY key;
    RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Bethesda Softworks", 0, KEY_READ, &key);

    /* display and sound */
    DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    GetSystemMetrics(SM_CXSCREEN);
    timeGetTime();

    printf("gamelike: reached the end\n");
    return 0;
}
