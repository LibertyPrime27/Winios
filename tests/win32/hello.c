/* The smallest real Windows program: no C runtime, three kernel32 imports.
 * Built with mingw-w64 (build.sh) into hello64.exe / hello32.exe; the same
 * binaries run on Windows and under winrun. */
#include <windows.h>

static const char msg[] = "hello from win32 on xcore\r\n";

void __attribute__((noreturn)) __stdcall entry(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(out, msg, sizeof msg - 1, &written, NULL);
    char buf[64]; DWORD n = 0;
    const char *cmd = GetCommandLineA();
    while (cmd[n] && n < 60) { buf[n] = cmd[n]; n++; }
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(out, buf, n, &written, NULL);
    ExitProcess(written == n ? 7 : 1);
}
