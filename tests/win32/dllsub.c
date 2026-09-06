/* The leaf of the DLL chain: no imports of its own, and a log the rest of the
 * test writes into so the exe can prove what ran, and in what order. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char g_log[128];
static int  g_n;

__declspec(dllexport) void sub_note(const char *s) {
    while (*s && g_n < (int)sizeof g_log - 2) g_log[g_n++] = *s++;
    g_log[g_n++] = ' ';
    g_log[g_n] = 0;
}
__declspec(dllexport) const char *sub_log(void) { return g_log; }
__declspec(dllexport) int sub_add(int a, int b) { return a + b; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) sub_note("sub");
    return TRUE;
}
