/* What a real program does with DLLs, minus the game.
 *
 *   - a static import of mid.dll, which itself imports sub.dll: the loader has
 *     to follow the chain, and attach sub before mid;
 *   - DllMain ordering, observable through sub.dll's log;
 *   - LoadLibrary + GetProcAddress at run time, by name and by ordinal;
 *   - a DLL that is never statically linked, found only by LoadLibrary, whose
 *     own import resolves to a module that is already loaded;
 *   - an export that forwards into another DLL;
 *   - GetModuleHandle for something already loaded, and a name that is not.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

__declspec(dllimport) int mid_calc(int n);
__declspec(dllimport) const char *sub_log(void);
__declspec(dllimport) void sub_note(const char *s);

typedef int (*addfn)(int, int);
typedef int (*triplefn)(int);
typedef const char *(*logfn)(void);

int main(void) {
    sub_note("exe");
    printf("order: %s\n", sub_log());
    printf("static import: mid_calc(7) = %d\n", mid_calc(7));

    HMODULE h = LoadLibraryA(SUBDLL);
    printf("LoadLibrary(%s): %s\n", SUBDLL, h ? "ok" : "FAILED");
    if (!h) return 1;

    addfn by_name = (addfn)(void *)GetProcAddress(h, "sub_add");
    printf("GetProcAddress by name: sub_add(20, 3) = %d\n", by_name ? by_name(20, 3) : -1);

    /* sub_add is ordinal 3 in dllsub.def */
    addfn by_ord = (addfn)(void *)GetProcAddress(h, (LPCSTR)3);
    printf("GetProcAddress by ordinal 3: %s\n", by_ord == by_name ? "same address" : "MISMATCH");

    logfn lg = (logfn)(void *)GetProcAddress(h, "sub_log");
    printf("one image, not two: %s\n", lg && lg() == sub_log() ? "yes" : "NO");

    printf("GetProcAddress for a name that is not exported: %s\n",
           GetProcAddress(h, "sub_nope") ? "FAILED (returned something)" : "NULL, as it should be");

    printf("GetModuleHandle(%s) == LoadLibrary: %s\n", SUBDLL,
           GetModuleHandleA(SUBDLL) == h ? "yes" : "NO");
    printf("GetModuleHandle(nosuch.dll): %s\n",
           GetModuleHandleA("nosuch.dll") ? "FAILED (returned something)" : "NULL, as it should be");

    /* late.dll is not in this program's import table at all */
    HMODULE l = LoadLibraryA(LATEDLL);
    printf("LoadLibrary(%s) for a DLL never linked in: %s\n", LATEDLL, l ? "ok" : "FAILED");
    if (!l) return 1;
    triplefn tri = (triplefn)(void *)GetProcAddress(l, "late_triple");
    printf("its import resolved to the loaded sub: late_triple(5) = %d\n", tri ? tri(5) : -1);
    printf("its DllMain ran, in order: %s\n", sub_log());

    addfn fwd = (addfn)(void *)GetProcAddress(l, "late_fwd_add");
    printf("forwarded export: %s\n",
           fwd == by_name ? "resolves to sub_add itself" : fwd ? "resolved elsewhere" : "FAILED");

    FreeLibrary(h);
    printf("still alive after FreeLibrary: sub_add(1, 1) = %d\n", by_name(1, 1));
    return 0;
}
