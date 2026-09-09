/* A program that starts programs, and the calls a modern C runtime makes on
 * the way in.
 *
 * The children are this same program with an argument, so one binary is the
 * parent and both children. What they print appears *after* the parent's
 * output: there is one process at a time here, and a child runs when its
 * parent has finished. That order is the property under test, along with the
 * parent seeing a signalled process handle, an exit code of 0, and an honest
 * refusal for a program that is not there.
 *
 * FlsGetValue2 and GetUserDefaultLocaleName are fetched by name because an
 * older import library may not have them -- which is also how the UCRT that
 * needs them gets them. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1) {
        char cwd[MAX_PATH] = "";
        GetCurrentDirectoryA(sizeof cwd, cwd);
        /* the last component only: the test runner's echo eats backslashes */
        const char *leaf = strrchr(cwd, '\\');
        printf("child %s: argc %d, cwd ...%s\n", argv[1], argc, leaf ? leaf + 1 : cwd);
        return 7;
    }
    printf("%d-bit parent\n", (int)(8 * sizeof(void *)));

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    typedef PVOID (WINAPI *fls2_t)(DWORD);
    typedef int (WINAPI *loc_t)(LPWSTR, int);
    fls2_t fls2 = (fls2_t)GetProcAddress(k32, "FlsGetValue2");
    loc_t locname = (loc_t)GetProcAddress(k32, "GetUserDefaultLocaleName");

    DWORD slot = FlsAlloc(NULL);
    FlsSetValue(slot, (void *)0x1234);
    printf("FlsGetValue2: %s\n", !fls2 ? "MISSING" : fls2(slot) == (void *)0x1234 ? "reads back what was set" : "WRONG");

    WCHAR loc[16] = {0}; char locn[16] = {0};
    int n = locname ? locname(loc, 16) : -1;
    for (int i = 0; i < 15 && loc[i]; i++) locn[i] = (char)loc[i];
    printf("GetUserDefaultLocaleName: %d chars, %s\n", n, locn);
    printf("AreFileApisANSI: %d\n", (int)AreFileApisANSI());
    printf("DisableThreadLibraryCalls: %d\n", (int)DisableThreadLibraryCalls(GetModuleHandleA(NULL)));

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" first", self);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    printf("CreateProcess: %s\n", ok ? "ok" : "FAILED");
    DWORD w = WaitForSingleObject(pi.hProcess, 1000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("wait: %s, exit code %lu\n", w == WAIT_OBJECT_0 ? "signalled" : "NOT SIGNALLED", (unsigned long)code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    ok = CreateProcessA("C:\\nowhere\\missing.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    printf("CreateProcess(missing): %s, error %lu\n", ok ? "SUCCEEDED (wrong)" : "refused", (unsigned long)GetLastError());

    SHELLEXECUTEINFOA sei; memset(&sei, 0, sizeof sei); sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpVerb = "open"; sei.lpFile = self;
    sei.lpParameters = "second"; sei.nShow = SW_SHOWNORMAL;
    ok = ShellExecuteExA(&sei);
    printf("ShellExecuteEx: %s, hInstApp %d, process handle %s\n", ok ? "ok" : "FAILED",
           (int)(INT_PTR)sei.hInstApp, sei.hProcess ? "given" : "MISSING");
    return 0;
}
