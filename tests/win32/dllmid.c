/* The middle of the chain: statically imports sub, and calls into it from its
 * own DllMain. If the loader gets the order wrong, sub's log will say so. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllimport) void sub_note(const char *s);
__declspec(dllimport) int  sub_add(int a, int b);

__declspec(dllexport) int mid_calc(int n) { return sub_add(n, n * 2); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) sub_note("mid");
    return TRUE;
}
