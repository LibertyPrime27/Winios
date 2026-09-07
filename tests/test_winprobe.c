/* The app's own C entry points, exercised the way the app calls them.
 *
 * tools/memprobe/Shared/winprobe.c is the whole surface between Swift and the
 * emulator: run a guest, capture what it printed, hand back the frame it drew
 * and the checksum of that frame. Every one of those is reached from a button
 * on the device and from nowhere in the shell suite, so until this existed the
 * app's C could break and every check would still pass.
 *
 * Two things are actually being asserted here.
 *
 * The first is that stdout capture works. winrun writes the guest's output to
 * fd 1, which on a phone goes nowhere, so winprobe redirects it to a temp file
 * for the duration of the run and puts it back afterwards. If that goes wrong
 * the app shows an empty report for a guest that ran perfectly well.
 *
 * The second is the frame checksum, and it is the more interesting one. The
 * rasterizers are integer by construction, so the same guest drawing the same
 * picture at the same size has to produce the same 32 bits everywhere -- on an
 * x86 runner, under qemu on aarch64, and on an iPhone. The diagnostics screen
 * on the device shows this number so that claim can be checked on the hardware
 * it is a claim about. That is only meaningful if the number the phone computes
 * comes from the same code and the same bytes as the number CI computes, which
 * is what this file pins down: winrun's `-frame` and win_probe_frame_crc() are
 * required here to agree, on the same guests, to the value the shell suite
 * records.
 */
#include "winprobe.h"
#include "w32.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

/* Direct3D 11, through the app's path, at the size the recording belongs to.
 *
 * 0x311139ad is the value tests/win32/run.sh checks for both bitnesses; it is
 * in one place there and one place here rather than a constant in a header,
 * because a checksum that is *defined* by the code that produces it has stopped
 * being a test of anything. If this number changes, the drawing changed, and
 * someone has to look at the picture and decide whether it changed for the
 * better.
 */
static void d3d11_frame(const char *dir, const char *name) {
    char path[512], out[8192];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint64_t ns = 0;
    int rc = win_probe_run_at(path, 320, 200, out, sizeof out, &ns);

    char what[160];
    snprintf(what, sizeof what, "%s ran through win_probe_run_at (exit %d)", name, rc);
    ok(rc == 0, what);

    /* The guest counts its own failures and says so; an empty capture means
     * the redirection broke, not that the guest was quiet. */
    snprintf(what, sizeof what, "%s output was captured", name);
    ok(out[0] != 0, what);
    snprintf(what, sizeof what, "%s reported no failures of its own", name);
    ok(strstr(out, "0 failures") != NULL, what);

    int w = 0, h = 0, pitch = 0;
    ok(win_probe_frame(&w, &h, &pitch) != NULL && w == 320 && h == 200,
       "the presented frame came back at the size that was asked for");

    uint32_t crc = win_probe_frame_crc();
    snprintf(what, sizeof what, "%s drew crc %08x (want 311139ad)", name, crc);
    ok(crc == 0x311139adu, what);
}

/* The screen size is borrowed, not taken. An app that ran a diagnostic at
 * 320x200 and left it there would start the next game at 320x200, which looks
 * like a graphics bug and is a bookkeeping one. */
static void screen_size_restored(const char *dir) {
    char path[512], out[4096];
    snprintf(path, sizeof path, "%s/d11test32.exe", dir);
    w32_set_screen_size(1280, 720);
    (void)win_probe_run_at(path, 320, 200, out, sizeof out, NULL);
    int cx = 0, cy = 0;
    w32_screen_size(&cx, &cy);
    ok(cx == 1280 && cy == 720, "the screen the app had set survived the run");
}

/* What the app shows for an .exe it has been handed but not run. This is the
 * first thing a person sees after importing something, so an exception in it
 * is the worst-placed failure in the product.
 *
 * enginedeps is the guest written to import what a commercial game engine
 * imports, so "nothing is missing" for it is the sentence the app most needs
 * to be able to say truthfully -- and the one that stopped being true the last
 * time a DLL table was incomplete. It reaches d3d11 through LoadLibrary rather
 * than a static import, which is what a game does, so d3d11 is deliberately
 * not expected to appear in this list. */
static void imports_report(const char *dir) {
    char path[512], out[8192];
    snprintf(path, sizeof path, "%s/enginedeps64.exe", dir);
    int rc = win_probe_imports(path, out, sizeof out);
    ok(rc == 0, "win_probe_imports read a 64-bit guest without running it");
    ok(strstr(out, "64-bit") != NULL, "the report says which bitness it is");
    ok(strstr(out, "0 missing") != NULL,
       "and nothing an engine imports is missing");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/win32";
    d3d11_frame(dir, "d11test64.exe");
    d3d11_frame(dir, "d11test32.exe");
    screen_size_restored(dir);
    imports_report(dir);
    printf("test_winprobe: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
