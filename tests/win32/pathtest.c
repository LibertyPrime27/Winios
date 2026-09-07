/* Where does a guest path land on the host?
 *
 * A Windows program opens "C:\..." for things it installed and a bare relative
 * name for things beside its executable, and both have to work or a game
 * cannot find its own data. C:\ is wherever the host says it is (the app
 * points it at its storage; winrun takes -C or WINRUN_DRIVE_C), and a relative
 * path resolves against the executable's own directory, because that is the
 * working directory a Windows program is started in.
 */
#include <windows.h>
#include <stdio.h>

static void try_open(const char *what, const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("%s: not found\n", what); return; }
    char b[64] = {0}; DWORD n = 0;
    ReadFile(h, b, 40, &n, NULL);
    while (n && (b[n-1] == '\n' || b[n-1] == '\r')) n--;
    printf("%s: %.*s\n", what, (int)n, b);
    CloseHandle(h);
}

int main(void) {
    try_open("absolute C:\\ path", "C:\\note.txt");
    try_open("relative to the executable", "beside.txt");
    try_open("a path that is not there", "C:\\nothing\\here.txt");
    return 0;
}
