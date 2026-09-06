/* winrun_main() called repeatedly in one process.
 *
 * The device build runs a .exe on a button tap, and a second tap must not see
 * anything the first one left behind: guest heap addresses cached in msvcrt,
 * the 4 GB arena, the bump allocator, and above all the block cache -- two
 * PE32 images both map their code at 0x400000, so a stale compiled block
 * there would run the previous program. Running every guest twice, in a
 * different order the second time, is the cheapest way to catch that.
 */
#include "winrun.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/win32";
    /* name, expected exit code */
    static const struct { const char *exe; int rc; } G[] = {
        { "hello64.exe", 7 }, { "hello32.exe", 7 },
        { "crt64.exe", 3 },   { "crt32.exe", 3 },
        { "nbody64.exe", 0 }, { "nbody32.exe", 0 },
    };
    const int n = (int)(sizeof G / sizeof G[0]);
    int bad = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < n; k++) {
            /* second pass runs them backwards, so every guest follows a
             * different predecessor than it did the first time */
            const int i = pass ? n - 1 - k : k;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, G[i].exe);
            char *av[4] = { (char *)"winrun", path, (char *)"a", (char *)"b" };
            int argn = strncmp(G[i].exe, "hello", 5) == 0 ? 4 : 2;
            fflush(stdout);
            int rc = winrun_main(argn, av);
            fflush(stdout);
            if (rc != G[i].rc) {
                printf("FAIL pass %d: %s exited %d, want %d\n", pass, G[i].exe, rc, G[i].rc);
                bad++;
            }
        }
    }
    printf("test_winrun_lib: %d runs, %d failed\n", 2 * n, bad);
    return bad != 0;
}
