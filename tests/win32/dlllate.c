/* Never linked into the executable: this one exists only to be found by
 * LoadLibrary at run time. It imports sub, which by then is already loaded,
 * so it also proves an import can resolve to a module that is already in. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllimport) void sub_note(const char *s);
__declspec(dllimport) int  sub_add(int a, int b);

__declspec(dllexport) int late_triple(int n) { return sub_add(n, sub_add(n, n)); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) sub_note("late");
    return TRUE;
}
