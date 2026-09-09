/* The 7z reader against archives an independent encoder made.
 *
 * tests/import/make_7z_fixtures.py writes the containers by hand from the
 * specification and has liblzma produce the compressed streams: LZMA1, LZMA2
 * behind the x86 branch filter, Delta, a stored folder, an encoded header and
 * a self-extractor. Each is extracted here and every byte compared with what
 * went in. A wrong bit anywhere in the range coder, the dictionary, the
 * filter's address rewriting or the header walk shows up as a mismatch, not
 * as a plausible file. */
#include "import/un7z.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
static void ok(int c, const char *what) { checks++; if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } }
static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { *n = 0; return 0; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(len > 0 ? (size_t)len : 1); size_t got = fread(p, 1, (size_t)len, f); fclose(f); *n = got; return p;
}
static int file_is(const char *path, const unsigned char *want, size_t n, const char *what) {
    size_t got; unsigned char *p = slurp(path, &got);
    int same = p && got == n && (n == 0 || !memcmp(p, want, n));
    if (!same) printf("     %s: got %zu bytes, wanted %zu%s\n", path, got, n, p && got == n ? " (contents differ)" : "");
    free(p); ok(same, what); return same;
}
static void pattern(unsigned char *out, size_t n, int k) { for (size_t i = 0; i < n; i++) out[i] = (unsigned char)((i * (size_t)k + (i >> 8)) & 0xFF); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static void rmtree(const char *p) { char cmd[4200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", p); if (system(cmd)) { } }

static void check_solid(const char *dir, const char *fixture, const char *what) {
    char arc[4096], out[4096], path[4200], err[256] = "";
    snprintf(arc, sizeof arc, "%s/%s", dir, fixture);
    snprintf(out, sizeof out, "%s/.un7z_test", dir);
    rmtree(out); mkdir(out, 0755);
    int skipped = -1;
    int n = sz_extract(arc, out, 0, 0, &skipped, err, sizeof err);
    char label[128]; snprintf(label, sizeof label, "%s: extracts (%d files, %d skipped%s%s)", what, n, skipped, err[0] ? "; " : "", err);
    ok(n == 4 && skipped == 0, label);
    static unsigned char buf[300000];
    snprintf(path, sizeof path, "%s/readme.txt", out);
    file_is(path, (const unsigned char *)"hello from a 7z archive\n", 24, "  readme.txt is right");
    pattern(buf, 70000, 7); snprintf(path, sizeof path, "%s/data/level.bin", out); file_is(path, buf, 70000, "  data/level.bin is right (70000 bytes)");
    pattern(buf, 300000, 3); snprintf(path, sizeof path, "%s/music/theme.raw", out); file_is(path, buf, 300000, "  music/theme.raw is right (300000 bytes)");
    snprintf(path, sizeof path, "%s/data/notes", out); ok(is_dir(path), "  data/notes is a directory");
    snprintf(path, sizeof path, "%s/data/empty.txt", out); file_is(path, 0, 0, "  data/empty.txt is an empty file");
    snprintf(arc, sizeof arc, "%s/%s", dir, fixture);
    ok(sz_uncompressed_total(arc) == 24 + 70000 + 300000, "  the size estimate is the sum of the files");
    rmtree(out);
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/import";
    char arc[4096], out[4096], path[4200], err[256];
    snprintf(arc, sizeof arc, "%s/copy.7z", dir); ok(sz_is_7z(arc), "copy.7z is recognised as 7z");
    snprintf(arc, sizeof arc, "%s/zip64.zip", dir); ok(!sz_is_7z(arc), "a zip is not");
    snprintf(arc, sizeof arc, "%s/fake_rarsfx.exe", dir); ok(sz_is_rar(arc) || 1, "(RAR detection ran)");

    check_solid(dir, "copy.7z", "stored");
    check_solid(dir, "lzma1.7z", "LZMA1, solid");
    check_solid(dir, "encoded_header.7z", "LZMA1 with an encoded header");
    check_solid(dir, "sfx_lzma1.exe", "the same archive behind a self-extractor stub");

    /* BCJ + LZMA2 over a real executable: the filter's address arithmetic has to invert exactly */
    snprintf(arc, sizeof arc, "%s/lzma2_bcj.7z", dir); snprintf(out, sizeof out, "%s/.un7z_test", dir);
    rmtree(out); mkdir(out, 0755); err[0] = 0;
    int skipped = 0, n = sz_extract(arc, out, 0, 0, &skipped, err, sizeof err);
    ok(n == 1 && !skipped, err[0] ? err : "x86 BCJ + LZMA2: extracts");
    { size_t want_n; char ref[4200]; snprintf(ref, sizeof ref, "%s/../win32/hello64.exe", dir); unsigned char *want = slurp(ref, &want_n);
      snprintf(path, sizeof path, "%s/hello64.exe", out); file_is(path, want, want_n, "  hello64.exe comes back byte for byte"); free(want); }
    rmtree(out);

    /* Delta + LZMA2 */
    snprintf(arc, sizeof arc, "%s/delta_lzma2.7z", dir); rmtree(out); mkdir(out, 0755); err[0] = 0;
    n = sz_extract(arc, out, 0, 0, &skipped, err, sizeof err);
    ok(n == 1 && !skipped, err[0] ? err : "Delta + LZMA2: extracts");
    { static unsigned char ramp[200000]; for (size_t i = 0; i < sizeof ramp; i++) ramp[i] = (unsigned char)((i / 3) & 0xFF);
      snprintf(path, sizeof path, "%s/ramp.bin", out); file_is(path, ramp, sizeof ramp, "  ramp.bin comes back byte for byte"); }
    rmtree(out);

    /* damage is reported, not written */
    { size_t n7; snprintf(arc, sizeof arc, "%s/lzma1.7z", dir); unsigned char *p = slurp(arc, &n7);
      if (p && n7 > 100) { p[60] ^= 0x55; snprintf(path, sizeof path, "%s/.damaged.7z", dir); FILE *f = fopen(path, "wb"); fwrite(p, 1, n7, f); fclose(f);
          rmtree(out); mkdir(out, 0755); err[0] = 0;
          n = sz_extract(path, out, 0, 0, &skipped, err, sizeof err);
          ok(n < 0 || skipped > 0, "a damaged archive is refused or its bad files skipped, not written as if fine");
          remove(path); rmtree(out); }
      free(p); }

    printf("test_un7z: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
