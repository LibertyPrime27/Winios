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
#include "w32.h"
#include <stdio.h>
#include <string.h>

/* The app stops a guest that is drawing frames by making Present return
 * D3DERR_DEVICELOST; the guest sees its own Present fail and leaves its loop.
 * Nothing reaches into the running guest, which is the point -- so this
 * checks the whole path: run d3dloop with no frame limit, ask it to stop
 * after a few frames, and require that it stops there and exits cleanly. */
static int g_seen;
static void count_and_stop(void *ctx, const void *px, int w, int h, int pitch) {
    (void)ctx; (void)px; (void)w; (void)h; (void)pitch;
    if (++g_seen == 5) w32_d3d9_device_lost(1);
}
static int test_device_lost(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/d3dloop32.exe", dir);
    char *av[2] = { (char *)"winrun", path };
    g_seen = 0;
    w32_set_present(count_and_stop, 0);
    int rc = winrun_main(2, av);            /* no frame limit: only the stop ends it */
    w32_set_present(0, 0);
    if (rc != 0 || g_seen != 5) {
        printf("FAIL device-lost stop: exit %d after %d frames, want 0 after 5\n", rc, g_seen);
        return 1;
    }
    printf("ok   d3dloop32.exe stopped on device-lost after %d frames\n", g_seen);
    return 0;
}

/* The registry is the one thing here that is supposed to outlive the process
 * that wrote it, and winrun_main() begins with a reset that throws the whole
 * in-memory store away -- so a value the second run can see came back off
 * disk and nowhere else. That is the property an installer depends on.
 * The read half deletes what it found, so this leaves nothing behind. */
static int test_registry(const char *dir) {
    char reg[600];
    snprintf(reg, sizeof reg, "%s/cdrive/registry.txt", dir);
    remove(reg);                              /* start from nothing, twice over */
    char path[512];
    snprintf(path, sizeof path, "%s/regtest32.exe", dir);
    int bad = 0;
    for (int a = 0; a < 2; a++) {
        char *av[3] = { (char *)"winrun", path, (char *)(a ? "read" : "write") };
        fflush(stdout);
        int rc = winrun_main(3, av);
        fflush(stdout);
        if (rc != 0) {
            printf("FAIL registry %s half exited %d, want 0\n", a ? "read" : "write", rc);
            bad++;
        }
    }
    remove(reg);
    if (!bad) printf("ok   regtest32.exe wrote the registry in one run and read it back in the next\n");
    return bad;
}

/* Faults, through the interpreter. Software exceptions run in the table above
 * with everything else because they work either way; turning a *fault* into a
 * guest exception needs the register state at the faulting instruction, which
 * a compiled block only has at its boundaries (the end of win32/seh.c says
 * what that will take). So the JIT comes off for these two and goes back on
 * after, which also checks that toggling it mid-process works -- the app does
 * exactly that between probes. */
static int test_faults(const char *dir) {
    int was = xc_jit_enabled();
    xc_jit_enable(0);
    int bad = 0;
    static const struct { const char *exe; const char *arg; int rc; } F[] = {
        { "faulttest32.exe", 0,      0   }, { "faulttest64.exe", 0,      0   },
        { "faulttest32.exe", "die",  129 }, { "faulttest64.exe", "die",  129 },
    };
    for (int i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, F[i].exe);
        char *av[3] = { (char *)"winrun", path, (char *)F[i].arg };
        fflush(stdout);
        int rc = winrun_main(F[i].arg ? 3 : 2, av);
        fflush(stdout);
        if (rc != F[i].rc) {
            printf("FAIL %s %s exited %d, want %d\n", F[i].exe, F[i].arg ? F[i].arg : "", rc, F[i].rc);
            bad++;
        }
    }
    xc_jit_enable(was);
    if (!bad) printf("ok   faulttest: a fault became a guest exception, and an unhandled one ended the run\n");
    return bad;
}

/* A window, a message pump and input, driven by the same script the shell
 * suite uses -- one recording, one source of truth for what the events are.
 * The guest exits 2 if nothing reached it, so a passing exit code here means
 * the events really crossed into the guest rather than the run merely
 * surviving. Run in both bitnesses because the MSG structure and the WndProc
 * call differ between them. */
static int test_input(const char *dir) {
    char script[600], path[512];
    snprintf(script, sizeof script, "%s/inputtest.script", dir);
    int bad = 0;
    for (int b = 0; b < 2; b++) {
        snprintf(path, sizeof path, "%s/inputtest%s.exe", dir, b ? "64" : "32");
        char *av[5] = { (char *)"winrun", (char *)"-input", script, path, (char *)"6" };
        fflush(stdout);
        int rc = winrun_main(5, av);
        fflush(stdout);
        if (rc != 0) { printf("FAIL inputtest%s exited %d, want 0\n", b ? "64" : "32", rc); bad++; }
    }
    if (!bad) printf("ok   inputtest: a window, a message pump, and keyboard and mouse events reaching the guest\n");
    return bad;
}

/* A dialog drawn from the guest's own resources, and the frame it produced.
 *
 * This one is here rather than only in the shell suite because it is the
 * device path: the app links this library, and what reaches the screen on a
 * phone is a frame handed to a present callback. So the callback is attached
 * here too, and what is checked is the checksum of the frame that arrived --
 * not the surface at exit, which by then is empty because the dialog has been
 * destroyed.
 *
 * The pixels are integer arithmetic throughout, so the checksum is the same
 * on an x86 runner, under qemu on aarch64, and on an iPad. A difference is a
 * bug rather than a rounding, which is the only reason a checksum is worth
 * asserting at all. */
static uint32_t g_frame_crc;
static int g_frame_count, g_frame_w, g_frame_h;

static uint32_t crc32_frame(const void *data, size_t n) {
    static uint32_t tab[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void dlg_present(void *ctx, const void *pixels, int width, int height, int pitch) {
    (void)ctx;
    if (width <= 0 || height <= 0) return;
    /* Row by row, because a frame's pitch need not be its width. */
    uint32_t c = 0;
    for (int y = 0; y < height; y++)
        c ^= crc32_frame((const uint8_t *)pixels + (size_t)y * pitch, (size_t)width * 4) + (uint32_t)y;
    g_frame_crc = c;
    g_frame_w = width; g_frame_h = height;
    g_frame_count++;
}

static int test_dialog(const char *dir) {
    char path[512];
    int bad = 0;
    for (int b = 0; b < 2; b++) {
        snprintf(path, sizeof path, "%s/dlgtest%s.exe", dir, b ? "64" : "32");
        g_frame_crc = 0; g_frame_count = 0; g_frame_w = g_frame_h = 0;
        w32_set_screen_size(640, 400);
        w32_set_present(dlg_present, 0);
        char *av[4] = { (char *)"winrun", (char *)"-screen", (char *)"640x400", path };
        fflush(stdout);
        int rc = winrun_main(4, av);
        fflush(stdout);
        w32_set_present(0, 0);
        if (rc != 0) { printf("FAIL dlgtest%s exited %d, want 0\n", b ? "64" : "32", rc); bad++; continue; }
        if (g_frame_count < 1) {
            printf("FAIL dlgtest%s drew a dialog but no frame reached the display\n", b ? "64" : "32");
            bad++; continue;
        }
        if (g_frame_w != 640 || g_frame_h != 400) {
            printf("FAIL dlgtest%s presented %dx%d, want 640x400\n", b ? "64" : "32", g_frame_w, g_frame_h);
            bad++; continue;
        }
        /* Both bitnesses draw the same dialog from the same template, so they
         * have to produce the same picture. Comparing them against each other
         * rather than against a recorded number keeps the check meaningful
         * when the font or a control's look is deliberately changed. */
        static uint32_t first;
        if (b == 0) first = g_frame_crc;
        else if (g_frame_crc != first) {
            printf("FAIL dlgtest: 32- and 64-bit drew different pixels (%08x vs %08x)\n",
                   first, g_frame_crc);
            bad++;
        }
    }
    if (!bad) printf("ok   dlgtest: a dialog from the guest's own resources, drawn and presented\n");
    return bad;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/win32";
    /* C:\ for pathtest, and a check that the setting survives winrun_reset --
     * the app sets it once at startup and every later run has to still see it. */
    char drive[600]; snprintf(drive, sizeof drive, "%s/cdrive", dir);
    w32_set_drive_c(drive);
    /* name, expected exit code */
    static const struct { const char *exe; int rc; } G[] = {
        { "hello64.exe", 7 }, { "hello32.exe", 7 },
        { "crt64.exe", 3 },   { "crt32.exe", 3 },
        { "nbody64.exe", 0 }, { "nbody32.exe", 0 },
        { "dlltest64.exe", 0 }, { "dlltest32.exe", 0 },
        { "chartest64.exe", 0 }, { "chartest32.exe", 0 },
        { "d3dtest64.exe", 0 },  { "d3dtest32.exe", 0 },
        { "d3dframe64.exe", 0 }, { "d3dframe32.exe", 0 },
        { "d3dloop64.exe", 0 },  { "d3dloop32.exe", 0 },
        { "pathtest64.exe", 0 }, { "pathtest32.exe", 0 },
        { "d3ddraw64.exe", 0 },  { "d3ddraw32.exe", 0 },
        { "filetest64.exe", 0 }, { "filetest32.exe", 0 },
        { "sehtest64.exe", 0 },  { "sehtest32.exe", 0 },
        { "audiotest64.exe", 0 }, { "audiotest32.exe", 0 },
        /* Threads, run back to back with everything else on purpose: the
         * thread table, the guest lock and the per-thread CPUs are globals
         * like any other, so this is where a winrun_reset() that forgot one
         * of them shows up. */
        { "threadtest64.exe", 0 }, { "threadtest32.exe", 0 },
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
            int argn = 2;
            if (!strncmp(G[i].exe, "hello", 5)) argn = 4;
            /* d3dloop draws until the device is lost; here it gets a frame count */
            else if (!strncmp(G[i].exe, "d3dloop", 7)) { av[2] = (char *)"12"; argn = 3; }
            fflush(stdout);
            int rc = winrun_main(argn, av);
            fflush(stdout);
            if (rc != G[i].rc) {
                printf("FAIL pass %d: %s exited %d, want %d\n", pass, G[i].exe, rc, G[i].rc);
                bad++;
            }
        }
    }
    bad += test_device_lost(dir);
    bad += test_registry(dir);
    bad += test_faults(dir);
    bad += test_input(dir);
    bad += test_dialog(dir);
    printf("test_winrun_lib: %d runs, %d failed\n", 2 * n, bad);
    return bad != 0;
}
