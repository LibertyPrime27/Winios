/* The registry, across two processes.
 *
 * A game asks HKEY_LOCAL_MACHINE where it was installed and believes the
 * answer, so the only interesting property is that a value written by one
 * program is still there when a different program starts up and looks. One
 * process cannot prove that, so this guest has two halves: run it with
 * "write", then run it again with "read", and the second half never creates
 * anything -- if the store did not survive to disk, the open fails.
 *
 * The "read" half also cleans up after itself, so the suite can run twice.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define KEY  "Software\\Winios\\Test"
#define SUBK "Software\\Winios\\Test\\Sub"

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Enumeration order is not defined by Windows and is not defined here either,
 * so sort before printing -- otherwise the expectation records an accident. */
static void list_values(HKEY k) {
    char rows[16][160]; int n = 0; LONG end = 0;
    for (DWORD i = 0; ; i++) {
        char name[64]; DWORD nlen = sizeof name, type = 0; BYTE data[64]; DWORD dlen = sizeof data;
        LONG r = RegEnumValueA(k, i, name, &nlen, NULL, &type, data, &dlen);
        if (r != ERROR_SUCCESS) { end = r; break; }
        if (n < 16) { snprintf(rows[n], 160, "  value %s type %lu, %lu bytes",
                               name, (unsigned long)type, (unsigned long)dlen); n++; }
    }
    char *p[16]; for (int i = 0; i < n; i++) p[i] = rows[i];
    qsort(p, n, sizeof p[0], cmp);
    for (int i = 0; i < n; i++) puts(p[i]);
    printf("  value enum ended with %ld (259 = no more items)\n", end);
}

static void list_keys(HKEY k) {
    char rows[16][160]; int n = 0; LONG end = 0;
    for (DWORD i = 0; ; i++) {
        char name[64]; DWORD nlen = sizeof name;
        LONG r = RegEnumKeyExA(k, i, name, &nlen, NULL, NULL, NULL, NULL);
        if (r != ERROR_SUCCESS) { end = r; break; }
        if (n < 16) { snprintf(rows[n], 160, "  subkey %s", name); n++; }
    }
    char *p[16]; for (int i = 0; i < n; i++) p[i] = rows[i];
    qsort(p, n, sizeof p[0], cmp);
    for (int i = 0; i < n; i++) puts(p[i]);
    printf("  subkey enum ended with %ld\n", end);
}

static int phase_write(void) {
    HKEY k; DWORD disp = 0;
    LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
    printf("RegCreateKeyEx: %ld, disposition %lu (1 = created new)\n", r, (unsigned long)disp);
    if (r != ERROR_SUCCESS) return 1;

    const char *path = "C:\\Games\\Fallout";
    DWORD version = 4;
    BYTE blob[4] = { 0xde, 0xad, 0xbe, 0xef };
    printf("set InstallPath: %ld\n", RegSetValueExA(k, "InstallPath", 0, REG_SZ,
                                                   (const BYTE *)path, (DWORD)strlen(path) + 1));
    printf("set Version: %ld\n", RegSetValueExA(k, "Version", 0, REG_DWORD,
                                                (const BYTE *)&version, sizeof version));
    printf("set Scratch: %ld\n", RegSetValueExA(k, "Scratch", 0, REG_BINARY, blob, sizeof blob));

    /* an empty subkey has to exist even with nothing in it */
    HKEY sub;
    printf("create subkey: %ld\n", RegCreateKeyExA(HKEY_LOCAL_MACHINE, SUBK, 0, NULL, 0,
                                                   KEY_ALL_ACCESS, NULL, &sub, NULL));
    RegCloseKey(sub);

    /* what every caller does first: ask how big the value is with no buffer */
    DWORD need = 0, type = 0;
    r = RegQueryValueExA(k, "InstallPath", NULL, &type, NULL, &need);
    printf("size-only query: %ld, type %lu, %lu bytes\n", r, (unsigned long)type, (unsigned long)need);
    /* and a buffer that is too small must say so rather than truncate */
    char small[4]; DWORD room = sizeof small;
    r = RegQueryValueExA(k, "InstallPath", NULL, NULL, (BYTE *)small, &room);
    printf("query into 4 bytes: %ld (234 = more data), wants %lu\n", r, (unsigned long)room);

    DWORD nsub = 0, nval = 0;
    r = RegQueryInfoKeyA(k, NULL, NULL, NULL, &nsub, NULL, NULL, &nval, NULL, NULL, NULL, NULL);
    printf("RegQueryInfoKey: %ld, %lu subkeys, %lu values\n", r, (unsigned long)nsub, (unsigned long)nval);

    list_values(k);
    list_keys(k);

    /* delete one value; the other two are what the next process looks for */
    printf("delete Scratch: %ld\n", RegDeleteValueA(k, "Scratch"));
    printf("delete Scratch again: %ld (2 = not found)\n", RegDeleteValueA(k, "Scratch"));
    RegCloseKey(k);
    return 0;
}

static int phase_read(void) {
    /* open, never create: this fails outright if nothing was persisted */
    HKEY k;
    LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, KEY, 0, KEY_READ, &k);
    printf("RegOpenKeyEx on a key written by the previous run: %ld\n", r);
    if (r != ERROR_SUCCESS) return 1;

    char buf[128]; DWORD len = sizeof buf, type = 0;
    r = RegQueryValueExA(k, "InstallPath", NULL, &type, (BYTE *)buf, &len);
    printf("InstallPath: %ld, type %lu, \"%s\"\n", r, (unsigned long)type, r ? "" : buf);

    DWORD version = 0; len = sizeof version;
    r = RegQueryValueExA(k, "Version", NULL, &type, (BYTE *)&version, &len);
    printf("Version: %ld, type %lu, %lu\n", r, (unsigned long)type, (unsigned long)version);

    r = RegQueryValueExA(k, "Scratch", NULL, NULL, NULL, &len);
    printf("Scratch (deleted last run): %ld (2 = not found)\n", r);

    list_values(k);
    list_keys(k);
    RegCloseKey(k);

    /* leave the store as we found it */
    printf("delete the tree: %ld\n", RegDeleteKeyA(HKEY_LOCAL_MACHINE, "Software\\Winios"));
    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, KEY, 0, KEY_READ, &k);
    printf("reopen after delete: %ld (2 = not found)\n", r);
    return r == ERROR_FILE_NOT_FOUND ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "write";
    if (!strcmp(what, "read")) return phase_read();
    return phase_write();
}
