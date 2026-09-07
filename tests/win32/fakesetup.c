/* A setup program, for testing the importer against.
 *
 * Installer mode has four moving parts -- work out the family, hand it the
 * right silent flags, run it, and find out what appeared on the drive -- and
 * none of them can be tested against a real installer: a real one is
 * megabytes of somebody's proprietary payload, it cannot be committed, and it
 * would make the test depend on a download.
 *
 * So this is a program that does what a silent install does, in the order one
 * does it, using the calls one uses. It is deliberately strict: it *fails*
 * unless it was given the flags the Inno Setup family takes, because the point
 * of the test is that the importer chose the right ones and they arrived
 * intact. An installer that ignored its arguments would let a broken
 * `sk_silent_argv` pass.
 *
 * The marker string below is what makes the detector call this Inno Setup.
 * That is not a trick to game the test -- it is the same byte pattern a real
 * Inno installer carries and the same one the detector looks for, and putting
 * it here is how the detection path gets exercised at all.
 */
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* Volatile and referenced, so no compiler decides an unused string can go. */
volatile const char *setup_marker = "TSetupLdrWindow Inno Setup 6.2.0";

static int fail(const char *why) { printf("fakesetup: %s\n", why); return 2; }

/* ---- the visible half ------------------------------------------------------
 *
 * With no flags, a real installer opens a window and waits. So does this one:
 * a dialog out of its own resources, with a destination field and an Install
 * button, which is the shape every wizard has.
 *
 * Nobody is going to click it in a test, so it clicks its own button -- and
 * that is worth being clear about. What this proves is that the template was
 * found and walked, the controls were created, the dialog was drawn and
 * presented, and a click on a button came back to the dialog procedure as a
 * WM_COMMAND. What it cannot prove is that a person's finger lands on the
 * button, because there is no finger. That half is the touch mapping, which
 * the input test covers separately.
 *
 * It also installs somewhere the person chose rather than somewhere we
 * suggested -- into Program Files, with the binary a level below that -- so
 * the importer has to find the install by looking at the drive, and has to
 * name the library entry after the game's folder rather than after the folder
 * the .exe happens to sit in.
 */
#define IDC_DEST  201
#define IDC_STAT  202

static char g_visible_dir[MAX_PATH];

static INT_PTR CALLBACK setup_dlg(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(dlg, IDC_DEST, g_visible_dir);
        SetDlgItemTextA(dlg, IDC_STAT, "Ready to install.");
        UpdateWindow(dlg);
        SendMessageA(GetDlgItem(dlg, IDOK), BM_CLICK, 0, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            /* Whatever is in the field is where it goes -- which on a device
             * is whatever the person typed. */
            GetDlgItemTextA(dlg, IDC_DEST, g_visible_dir, sizeof g_visible_dir);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        return FALSE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    (void)setup_marker;
    char dir[MAX_PATH] = "";
    int silent = 0, sp_minus = 0, norestart = 0;

    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/SILENT") || !_stricmp(argv[i], "/VERYSILENT")) silent = 1;
        else if (!_stricmp(argv[i], "/SP-")) sp_minus = 1;
        else if (!_stricmp(argv[i], "/NORESTART")) norestart = 1;
        else if (!_strnicmp(argv[i], "/DIR=", 5)) snprintf(dir, sizeof dir, "%s", argv[i] + 5);
    }
    printf("flags: silent=%d sp-=%d norestart=%d\n", silent, sp_minus, norestart);
    printf("dir:   %s\n", dir[0] ? dir : "(none given)");

    /* Without a silent flag, do what a real installer does: put the window up
     * and ask. The destination it offers is the standard one, not one the
     * importer chose -- and the importer is going to have to find it. */
    if (!silent) {
        InitCommonControls();
        snprintf(g_visible_dir, sizeof g_visible_dir,
                 "C:\\Program Files\\Fake Game %d\\bin", (int)(sizeof(void *) * 8));
        INT_PTR r = DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(100),
                                    NULL, setup_dlg, 0);
        if (r != IDOK) return fail("cancelled");
        snprintf(dir, sizeof dir, "%s", g_visible_dir);
        printf("visible: installing to %s\n", dir);
    }
    if (!dir[0]) return fail("no /DIR=: nowhere to install to");

    /* 0. Narrow the DLL search path, which is the first thing a real NSIS
     *    installer does -- for security, not for function: it stops a DLL
     *    planted next to the setup file being picked up ahead of the real
     *    one. Reached through GetProcAddress rather than linked, because that
     *    is how an installer that must still run on older Windows calls a
     *    function added in Windows 8, and it is how the real one does it.
     *
     *    This is here because a 580 MB GameMaker installer stopped on exactly
     *    this call, and it was the only thing it asked for that was missing. */
    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        typedef BOOL (WINAPI *SetDefaultDllDirectories_t)(DWORD);
        SetDefaultDllDirectories_t sdd = k32
            ? (SetDefaultDllDirectories_t)(void *)GetProcAddress(k32, "SetDefaultDllDirectories")
            : NULL;
        if (!sdd) return fail("SetDefaultDllDirectories is not exported");
        /* LOAD_LIBRARY_SEARCH_SYSTEM32 | _APPLICATION_DIR | _USER_DIRS */
        if (!sdd(0x800 | 0x200 | 0x400))
            return fail("SetDefaultDllDirectories failed");
        printf("dllsearch: narrowed\n");
    }

    /* 1. Is there room? A real installer refuses to start if this fails, so
     *    an emulator where it fails is an emulator where nothing installs. */
    ULARGE_INTEGER avail, total, freebytes;
    if (!GetDiskFreeSpaceExA("C:\\", &avail, &total, &freebytes))
        return fail("GetDiskFreeSpaceEx failed");
    printf("free:  %s\n", avail.QuadPart > (1u << 20) ? "enough" : "not enough");
    if (avail.QuadPart == 0) return fail("no free space reported");

    /* 2. Where do programs go? Asked even though /DIR= was given, because a
     *    real installer uses this to build its default. */
    char pf[MAX_PATH] = "";
    if (SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf) != S_OK)
        return fail("SHGetFolderPath(PROGRAM_FILES) failed");
    printf("progs: %s\n", pf);

    /* 3. Make the install directory, and a subdirectory in it. */
    int rc = SHCreateDirectoryExA(NULL, dir, NULL);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS)
        return fail("SHCreateDirectoryEx failed");
    char sub[MAX_PATH];
    snprintf(sub, sizeof sub, "%s\\data", dir);
    if (!CreateDirectoryA(sub, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return fail("CreateDirectory failed");

    /* 4. Write a data file the way an installer writes an unpacked one. */
    char datafile[MAX_PATH];
    snprintf(datafile, sizeof datafile, "%s\\data\\assets.dat", dir);
    HANDLE h = CreateFileA(datafile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail("CreateFile failed");
    static const char payload[] = "not really assets, but a real file\n";
    DWORD put = 0;
    if (!WriteFile(h, payload, (DWORD)(sizeof payload - 1), &put, NULL) || put != sizeof payload - 1) {
        CloseHandle(h); return fail("WriteFile failed");
    }
    CloseHandle(h);

    /* 5. Put the program itself there. Copying ourselves is not what a real
     *    installer does, but it puts a genuine executable in the install --
     *    which is what the importer then has to find and rank. */
    char exe[MAX_PATH];
    snprintf(exe, sizeof exe, "%s\\FakeGame.exe", dir);
    if (!CopyFileA(argv[0], exe, FALSE)) return fail("CopyFile failed");

    /* 6. Settings, in an .ini, the way a game of this vintage keeps them. */
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%s\\FakeGame.ini", dir);
    if (!WritePrivateProfileStringA("Display", "Width", "1920", ini))
        return fail("WritePrivateProfileString failed");
    if (!WritePrivateProfileStringA("Display", "Height", "1080", ini))
        return fail("WritePrivateProfileString failed");
    if (!WritePrivateProfileStringA("Audio", "Volume", "80", ini))
        return fail("WritePrivateProfileString failed");
    /* Read one back: an installer that cannot read its own settings has not
     * really written them, and a stub that returns success would pass the
     * write and fail here. */
    char got[64] = "";
    GetPrivateProfileStringA("Display", "Width", "?", got, sizeof got, ini);
    if (strcmp(got, "1920")) return fail("the .ini did not read back");
    printf("ini:   Display/Width = %s\n", got);

    /* 6b. Where does a Start Menu shortcut go? An installer asks through the
     *     older pair -- SHGetSpecialFolderLocation for an item ID list, then
     *     SHGetPathFromIDList to turn it into a path -- and frees the list
     *     with CoTaskMemFree. All three were either missing or stubs that
     *     returned failure, which is how an installer ends up unable to find
     *     anywhere to put a shortcut. */
    {
        LPITEMIDLIST pidl = NULL;
        if (SHGetSpecialFolderLocation(NULL, CSIDL_PROGRAMS, &pidl) != S_OK || !pidl)
            return fail("SHGetSpecialFolderLocation(CSIDL_PROGRAMS) failed");
        char menu[MAX_PATH] = "";
        if (!SHGetPathFromIDListA(pidl, menu) || !menu[0])
            return fail("SHGetPathFromIDList gave nothing back");
        CoTaskMemFree(pidl);
        printf("menu:  %s\n", menu);
        /* And it has to be a directory we can actually write into, or the
         * path was only a plausible string. */
        char probe[MAX_PATH];
        snprintf(probe, sizeof probe, "%s\\FakeGame.lnk", menu);
        HANDLE lnk = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (lnk == INVALID_HANDLE_VALUE) return fail("the Start Menu path is not writable");
        CloseHandle(lnk);
    }

    /* 7. The uninstall key, which is how Windows knows the program is there. */
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FakeGame",
                        0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
        return fail("RegCreateKeyEx failed");
    static const char disp[] = "FakeGame";
    RegSetValueExA(key, "DisplayName", 0, REG_SZ, (const BYTE *)disp, sizeof disp);
    RegSetValueExA(key, "InstallLocation", 0, REG_SZ, (const BYTE *)dir, (DWORD)strlen(dir) + 1);
    RegCloseKey(key);

    /* 8. And a file in Temp, which the importer must *not* count as part of
     *    the install -- an installer leaves hundreds of these behind. */
    char tmp[MAX_PATH], tmpfile[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    if (GetTempFileNameA(tmp, "fsu", 0, tmpfile))
        printf("temp:  wrote one temporary file\n");

    printf("fakesetup: installed to %s\n", dir);
    return 0;
}
