/* The importer's own tests: the parts that do not need a guest.
 *
 * Installer detection, the zip reader, path sanitising and executable ranking
 * are all pure functions over files, so they can be checked here rather than
 * only through a real import -- which is worth doing because three of them are
 * the parts where being wrong is expensive: a path sanitiser that lets `..`
 * through writes outside the drive, a detector that guesses the wrong family
 * hands an installer flags it will ignore, and a ranker that picks the
 * uninstaller offers to run the uninstaller.
 *
 * The fixtures under tests/import/ are minimal PE headers with the real
 * marker bytes of each installer family placed inside them. They are not
 * installers and cannot run; identification only ever reads bytes, so that is
 * all a fixture needs to be. The one exception is fake_zipsfx.exe, which has a
 * genuine zip appended, because "can we unpack a self-extracting archive" is
 * not answerable against a fake.
 */
#include "setupkind.h"
#include "unzip.h"
#include "drivediff.h"
#include "import.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static void ok(int cond, const char *what, ...) {
    va_list ap; va_start(ap, what);
    char msg[512];
    vsnprintf(msg, sizeof msg, what, ap);
    va_end(ap);
    if (cond) printf("ok   %s\n", msg);
    else { printf("FAIL %s\n", msg); fails++; }
}

/* ---- installer detection -------------------------------------------------- */

static void test_detect(const char *dir) {
    struct { const char *file; sk_kind want; } C[] = {
        { "fake_inno.exe",     SK_INNO },
        { "fake_nsis.exe",     SK_NSIS },
        { "fake_is.exe",       SK_INSTALLSHIELD },
        { "fake_msi.exe",      SK_MSI_WRAPPER },
        { "fake_7zsfx.exe",    SK_SFX_7Z },
        { "fake_rarsfx.exe",   SK_SFX_RAR },
        { "fake_zipsfx.exe",   SK_SFX_ZIP },
        { "fake_wise.exe",     SK_WISE },
        { "fake_sf.exe",       SK_SETUPFACTORY },
        { "fake_plain.exe",    SK_PLAIN },
        { "fake_setupish.exe", SK_UNKNOWN },
        { "not_a_pe.bin",      SK_NOT_PE },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, C[i].file);
        sk_info k = sk_identify(path);
        ok(k.kind == C[i].want, "%-20s -> %s", C[i].file, k.name);
    }
    /* A file that is not there must not be reported as a program. */
    char missing[1024];
    snprintf(missing, sizeof missing, "%s/nothing_here.exe", dir);
    ok(sk_identify(missing).kind == SK_NOT_PE, "a file that does not exist is not a program");
}

/* The flags themselves. These are the difference between an install and a
 * dialog nobody can click, and two of them have shapes that are easy to get
 * wrong: Inno takes the directory as its own argument, NSIS glues it to /D=
 * and requires it last and unquoted. */
static void test_flags(const char *dir) {
    char path[1024];
    const char *a[SK_MAX_ARGS];
    char scratch[SK_MAX_ARGS * 1024];

    snprintf(path, sizeof path, "%s/fake_inno.exe", dir);
    sk_info inno = sk_identify(path);
    int n = sk_silent_argv(&inno, "C:\\Games\\Thing", a, scratch, sizeof scratch);
    ok(n == 4, "Inno: four arguments (got %d)", n);
    ok(n == 4 && !strcmp(a[0], "/SILENT"), "Inno: silent first");
    ok(n == 4 && !strcmp(a[3], "/DIR=C:\\Games\\Thing"), "Inno: the directory is substituted");

    snprintf(path, sizeof path, "%s/fake_nsis.exe", dir);
    sk_info nsis = sk_identify(path);
    n = sk_silent_argv(&nsis, "C:\\Games\\Thing", a, scratch, sizeof scratch);
    ok(n == 2 && !strcmp(a[0], "/S"), "NSIS: /S first");
    ok(n == 2 && !strcmp(a[1], "/D=C:\\Games\\Thing"), "NSIS: /D= glued, and last");
    ok(n == 2 && !strchr(a[1], '"'), "NSIS: /D= is never quoted -- quoting it misplaces the install");

    /* An MSI wrapper is identified and then declared unsupported, which is a
     * more useful answer than flags that will not help. */
    snprintf(path, sizeof path, "%s/fake_msi.exe", dir);
    ok(!sk_identify(path).silent_supported, "an MSI bootstrapper says up front that it cannot work");
    snprintf(path, sizeof path, "%s/fake_inno.exe", dir);
    ok(sk_identify(path).silent_supported, "Inno says it can");
}

/* ---- path sanitising ------------------------------------------------------- */

static void test_names(void) {
    char o[512];
    /* The whole point: nothing may escape the destination. */
    ok(uz_safe_name("../../etc/passwd", o, sizeof o) && !strcmp(o, "etc/passwd"),
       "a leading ../.. is dropped, not honoured");
    ok(uz_safe_name("a/../../../b", o, sizeof o) && !strcmp(o, "a/b"),
       "a .. in the middle is dropped");
    ok(uz_safe_name("C:\\Windows\\System32\\x.dll", o, sizeof o)
       && !strcmp(o, "Windows/System32/x.dll"), "a drive letter is stripped");
    ok(uz_safe_name("/abs/path/x", o, sizeof o) && !strcmp(o, "abs/path/x"),
       "an absolute path becomes relative");
    ok(uz_safe_name("a\\b\\c.txt", o, sizeof o) && !strcmp(o, "a/b/c.txt"),
       "backslashes become slashes");
    ok(!uz_safe_name("..", o, sizeof o), "a name that is only .. is rejected");
    ok(!uz_safe_name("", o, sizeof o), "an empty name is rejected");
    /* And a name that merely *contains* dots is not a traversal. */
    ok(uz_safe_name("...", o, sizeof o) && !strcmp(o, "..."), "... is a real name");
    ok(uz_safe_name("a..b/c", o, sizeof o) && !strcmp(o, "a..b/c"), "a..b is a real name");

    /* Library entry names: one component, nothing that could be a separator. */
    ok(wi_safe_component("Cool Game", o, sizeof o) && !strcmp(o, "Cool Game"), "an ordinary name survives");
    ok(wi_safe_component("a/b\\c:d", o, sizeof o) && !strchr(o, '/') && !strchr(o, '\\')
       && !strchr(o, ':'), "separators are removed from an entry name");
    ok(wi_safe_component("...", o, sizeof o) && o[0] != '.', "an entry name never begins with a dot");
    ok(wi_safe_component("", o, sizeof o) && o[0], "an entry name is never empty");
}

/* ---- the zip reader -------------------------------------------------------- */

static int count_files(const char *root) {
    dd_snap *s = dd_snapshot(root);
    int n = dd_count(s);
    dd_free(s);
    return n;
}

static void test_unzip(const char *dir) {
    char sfx[1024];
    snprintf(sfx, sizeof sfx, "%s/fake_zipsfx.exe", dir);
    ok(uz_is_zip(sfx), "a zip appended to a PE is recognised as a zip");
    char plain[1024];
    snprintf(plain, sizeof plain, "%s/fake_plain.exe", dir);
    ok(!uz_is_zip(plain), "a PE with no zip in it is not");

    char out[1024];
    snprintf(out, sizeof out, "%s/.unzip_test", dir);
    /* Left over from a previous run would make the counts meaningless. */
    char rm[1200]; snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    if (system(rm)) { /* nothing there to remove is fine */ }

    char err[256] = "";
    int skipped = 0;
    int n = uz_extract(sfx, out, 0, 0, &skipped, err, sizeof err);
    ok(n == 2, "the self-extractor's two entries came out (got %d, %s)", n, *err ? err : "no error");
    ok(skipped == 0, "nothing was skipped");
    ok(count_files(out) == 2, "and two files are on disk");

    /* The contents, not just the count: a reader that produces empty files of
     * the right names would pass everything above. */
    char one[1200];
    snprintf(one, sizeof one, "%s/game/readme.txt", out);
    FILE *f = fopen(one, "rb");
    char buf[128] = "";
    size_t got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    ok(got > 0 && strstr(buf, "inside a zip sfx"), "and the deflated contents are right");

    snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    if (system(rm)) { }
}

/* ---- which executable is the game ----------------------------------------- */

/* Built here rather than committed: the ranking depends on the *shape* of a
 * tree -- a _Data folder beside an executable, a Binaries/Win64 path -- and a
 * test that builds the shape says what it is testing, where a directory of
 * committed stub files does not. */
static void write_pe(const char *path, size_t size, int bits64) {
    char dirbuf[1024];
    snprintf(dirbuf, sizeof dirbuf, "%s", path);
    char *slash = strrchr(dirbuf, '/');
    if (slash) { *slash = 0; uz_mkdirs(dirbuf); }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned char h[0x200] = {0};
    h[0] = 'M'; h[1] = 'Z';
    h[0x3C] = 0x80;
    h[0x80] = 'P'; h[0x81] = 'E';
    /* The optional-header magic, little-endian: 0x10B is PE32 and 0x20B is
     * PE32+. Only the high byte differs, which is exactly the sort of thing
     * worth writing out rather than leaving to be noticed. */
    h[0x80 + 24] = 0x0B;
    h[0x80 + 25] = bits64 ? 0x02 : 0x01;
    fwrite(h, 1, sizeof h, f);
    for (size_t i = sizeof h; i < size; i++) fputc(0, f);
    fclose(f);
}
static void touch(const char *path) {
    char dirbuf[1024];
    snprintf(dirbuf, sizeof dirbuf, "%s", path);
    char *slash = strrchr(dirbuf, '/');
    if (slash) { *slash = 0; uz_mkdirs(dirbuf); }
    FILE *f = fopen(path, "wb");
    if (f) { fputs("x\n", f); fclose(f); }
}

static void test_ranking(const char *dir) {
    char root[1024];
    snprintf(root, sizeof root, "%s/.rank_test", dir);
    char rm[1200]; snprintf(rm, sizeof rm, "rm -rf '%s'", root);
    if (system(rm)) { }

    char p[1200];
    /* A Unity game as it actually ships, junk included. */
    snprintf(p, sizeof p, "%s/MyGame.exe", root);                       write_pe(p, 30u << 20, 1);
    snprintf(p, sizeof p, "%s/MyGame_Data/data.unity3d", root);         touch(p);
    snprintf(p, sizeof p, "%s/UnityCrashHandler64.exe", root);          write_pe(p, 1u << 20, 1);
    snprintf(p, sizeof p, "%s/unins000.exe", root);                     write_pe(p, 1u << 20, 0);
    snprintf(p, sizeof p, "%s/_CommonRedist/vcredist/vcredist.exe", root); write_pe(p, 8u << 20, 0);

    dd_exe e[DD_MAX_EXES];
    int n = dd_rank_exes(root, "MyGame", e, DD_MAX_EXES);
    ok(n == 4, "four executables found (got %d)", n);
    ok(n > 0 && !strcmp(e[0].rel, "MyGame.exe"),
       "the Unity binary wins, not the crash handler or the uninstaller (got %s)",
       n > 0 ? e[0].rel : "nothing");
    ok(n > 0 && e[0].is32 == 0, "and its bitness is read from the header");
    /* The redistributable must come last: it is large, which is the one
     * signal that would otherwise favour it. */
    ok(n == 4 && strstr(e[3].rel, "vcredist"), "the redistributable ranks last (got %s)",
       n == 4 ? e[3].rel : "nothing");

    if (system(rm)) { }

    /* Unreal: the real binary is three folders down and the thing at the top
     * is a launcher shim, so depth alone would pick the wrong one. */
    snprintf(p, sizeof p, "%s/Blastoff.exe", root);                                  write_pe(p, 400u << 10, 1);
    snprintf(p, sizeof p, "%s/Blastoff/Binaries/Win64/Blastoff-Win64-Shipping.exe", root); write_pe(p, 90u << 20, 1);
    n = dd_rank_exes(root, "Blastoff", e, DD_MAX_EXES);
    ok(n == 2 && strstr(e[0].rel, "Shipping"),
       "the Unreal shipping binary beats the launcher shim above it (got %s)",
       n > 0 ? e[0].rel : "nothing");
    if (system(rm)) { }
}

/* ---- what an installer left behind ---------------------------------------- */

typedef struct { int n; char first[512]; } added_count;
static int added_cb(void *c, const char *rel, uint64_t size) {
    (void)size;
    added_count *a = (added_count *)c;
    if (!a->n) snprintf(a->first, sizeof a->first, "%s", rel);
    a->n++;
    return 0;
}

static void test_diff(const char *dir) {
    char root[1024];
    snprintf(root, sizeof root, "%s/.diff_test", dir);
    char rm[1200]; snprintf(rm, sizeof rm, "rm -rf '%s'", root);
    if (system(rm)) { }
    uz_mkdirs(root);

    char p[1200];
    snprintf(p, sizeof p, "%s/before.txt", root); touch(p);
    dd_snap *a = dd_snapshot(root);
    ok(dd_count(a) == 1, "a snapshot of one file has one file");

    snprintf(p, sizeof p, "%s/Program Files/Thing/thing.exe", root); touch(p);
    snprintf(p, sizeof p, "%s/Program Files/Thing/data/x.dat", root); touch(p);
    dd_snap *b = dd_snapshot(root);

    added_count ac = { 0, "" };
    int n = dd_added(a, b, added_cb, &ac);
    ok(n == 2 && ac.n == 2, "two files appeared (got %d)", n);

    char common[1024] = "";
    ok(dd_added_root(a, b, common, sizeof common)
       && !strcmp(common, "Program Files/Thing"),
       "and their common directory is where the install went (got \"%s\")", common);

    /* Nothing changed: nothing added. A diff that reports files it has
     * already seen would make every install look like it worked. */
    dd_snap *c = dd_snapshot(root);
    ok(dd_added(b, c, 0, 0) == 0, "an unchanged tree reports nothing added");
    dd_free(a); dd_free(b); dd_free(c);
    if (system(rm)) { }
}

/* ---- will it fit? ---------------------------------------------------------- */

/* The estimate is the part that can be wrong; the comparison against free
 * space is two additions. So this checks the estimate against a tree whose
 * size the test knows exactly, and checks that an archive is measured
 * *uncompressed* -- which is the whole point, since a zip of mostly-zero
 * files can be a hundredth of what it unpacks to, and comparing the download
 * size against free space would wave a game through that cannot fit. */
static void test_space(const char *dir) {
    char root[1024];
    snprintf(root, sizeof root, "%s/.space_test", dir);
    char rm[1200]; snprintf(rm, sizeof rm, "rm -rf '%s'", root);
    if (system(rm)) { }

    char p[1200];
    snprintf(p, sizeof p, "%s/a.bin", root);          write_pe(p, 100000, 0);
    snprintf(p, sizeof p, "%s/sub/b.bin", root);      write_pe(p, 250000, 0);
    snprintf(p, sizeof p, "%s/sub/deep/c.bin", root); write_pe(p, 4096, 0);

    /* Every check below calls first and asserts second, on its own line.
     * Folding the call into the ok() -- `ok(f(&need) && need == n, "...", need)`
     * -- looks tidier and is wrong: C does not order the evaluation of a
     * call's arguments against the call, so the `need` printed in the message
     * can be the value from before f() filled it in. That is not a
     * hypothetical; the first version of this function did it and reported
     * "ok ... (got 230)" while asserting the number was 920. */
    uint64_t need = 0, have = 0;
    int known = wi_space_needed(root, dir, &need, &have);
    ok(known, "a folder's size can be worked out");
    ok(need == 100000 + 250000 + 4096,
       "and it is the sum of its files, at every depth (got %llu, want %llu)",
       (unsigned long long)need, (unsigned long long)(100000 + 250000 + 4096));
    ok(have > 0, "and the free space on the destination is reported");

    snprintf(p, sizeof p, "%s/a.bin", root);
    need = 0;
    known = wi_space_needed(p, dir, &need, 0);
    ok(known && need == 100000, "a single file is its own size (got %llu)",
       (unsigned long long)need);

    /* An archive is measured by what comes out, not by what is on disk, and
     * this is the whole point of measuring it at all: compressible.zip holds
     * 4 MB in about 4 KB. Checking the download's size against free space
     * would wave through a game a thousand times too big for the device. */
    struct stat st;
    char zip[1024];
    snprintf(zip, sizeof zip, "%s/compressible.zip", dir);
    uint64_t unpacked = uz_uncompressed_total(zip);
    uint64_t container = stat(zip, &st) == 0 ? (uint64_t)st.st_size : 0;
    ok(unpacked >= (4u << 20), "a zip's uncompressed total is what is inside it (%llu bytes)",
       (unsigned long long)unpacked);
    ok(container > 0 && unpacked > container * 100,
       "and it dwarfs the file holding it (%llu on disk, %llu unpacked)",
       (unsigned long long)container, (unsigned long long)unpacked);
    need = 0;
    known = wi_space_needed(zip, dir, &need, 0);
    ok(known && need == unpacked,
       "so that is what an import of it is said to need (got %llu, want %llu)",
       (unsigned long long)need, (unsigned long long)unpacked);

    /* And the same through a self-extracting .exe, where the payload is found
     * behind a PE loader. Here the stub is the bigger half, which is why the
     * ratio above is checked against a plain zip and not against this. */
    char sfx[1024];
    snprintf(sfx, sizeof sfx, "%s/fake_zipsfx.exe", dir);
    uint64_t sfx_unpacked = uz_uncompressed_total(sfx);
    ok(sfx_unpacked > 0, "a self-extractor's payload is measurable too (%llu bytes)",
       (unsigned long long)sfx_unpacked);
    need = 0;
    known = wi_space_needed(sfx, dir, &need, 0);
    ok(known && need == sfx_unpacked, "and it is measured the same way (got %llu)",
       (unsigned long long)need);

    /* Not a zip and not a PE: the size still comes back, because a plain file
     * is copied as it is. */
    snprintf(p, sizeof p, "%s/not_a_pe.bin", dir);
    uint64_t on_disk = stat(p, &st) == 0 ? (uint64_t)st.st_size : 0;
    need = 0;
    known = wi_space_needed(p, dir, &need, 0);
    ok(known && need == on_disk, "a plain file too (got %llu, want %llu)",
       (unsigned long long)need, (unsigned long long)on_disk);

    /* A source that is not there is "unknown", not "zero". The caller treats
     * unknown as permission to carry on, so answering "0 bytes needed" would
     * be a silent yes to something that was never measured. */
    snprintf(p, sizeof p, "%s/nothing_here_at_all", dir);
    need = 12345;
    known = wi_space_needed(p, dir, &need, 0);
    ok(!known, "an unreadable source reports that it is unknown");

    if (system(rm)) { }
}

/* ---- probing: which mode should be offered -------------------------------- */

static void test_probe(const char *dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s/fake_inno.exe", dir);
    wi_probe_result r = wi_probe(p);
    ok(r.src == WI_SRC_EXE && r.looks_like_installer,
       "an Inno setup is offered as an installer");

    snprintf(p, sizeof p, "%s/fake_plain.exe", dir);
    r = wi_probe(p);
    ok(r.src == WI_SRC_EXE && !r.looks_like_installer,
       "a plain program is offered as a game");

    /* A self-extracting zip is an archive we can open, so it is a game as far
     * as the user is concerned -- nothing has to be run. */
    snprintf(p, sizeof p, "%s/fake_zipsfx.exe", dir);
    r = wi_probe(p);
    ok(r.src == WI_SRC_ZIP && !r.looks_like_installer,
       "a zip self-extractor is an archive, not an installer");

    r = wi_probe(dir);
    ok(r.src == WI_SRC_FOLDER, "a directory is a folder");
    snprintf(p, sizeof p, "%s/not_a_pe.bin", dir);
    ok(wi_probe(p).src == WI_SRC_UNKNOWN, "a file that is nothing we handle says so");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/import";
    test_detect(dir);
    test_flags(dir);
    test_names();
    test_unzip(dir);
    test_ranking(dir);
    test_diff(dir);
    test_space(dir);
    test_probe(dir);
    printf("\ntest_import: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
