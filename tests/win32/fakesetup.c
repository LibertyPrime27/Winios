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
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* Volatile and referenced, so no compiler decides an unused string can go. */
volatile const char *setup_marker = "TSetupLdrWindow Inno Setup 6.2.0";

static int fail(const char *why) { printf("fakesetup: %s\n", why); return 2; }

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

    /* Without a silent flag a real installer opens a window, which is the one
     * thing that cannot happen here -- so say so and stop, exactly as the
     * importer's report would then have to explain. */
    if (!silent) return fail("no silent flag: a real installer would open a window here");
    if (!dir[0]) return fail("no /DIR=: nowhere to install to");

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
