/* wimport -- bring a Windows program in, from the command line.
 *
 * The same code the app's importer calls, with a shell in front of it. It
 * exists so that both modes can be exercised end to end on a Linux machine,
 * where there is a real filesystem to look at afterwards and a debugger to
 * attach: an importer whose only interface is a phone is an importer that
 * cannot be tested.
 */
#include "import.h"
#include "drivediff.h"
#include "unzip.h"
#include "winrun.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int spinner(void *ctx, const char *stage, uint64_t done, uint64_t total) {
    (void)ctx;
    if (total) fprintf(stderr, "\r%s %llu/%llu   ", stage,
                       (unsigned long long)done, (unsigned long long)total);
    else if (done) fprintf(stderr, "\r%s %llu   ", stage, (unsigned long long)done);
    else fprintf(stderr, "\r%s...   ", stage);
    fflush(stderr);
    return 0;
}
static void spinner_done(void) { fprintf(stderr, "\r%60s\r", ""); }

/* The progress callbacks have different shapes on purpose -- an extraction
 * knows how many entries there are and a copy does not -- so the zip one is
 * adapted rather than shared. */
static int zip_spin(void *ctx, const char *name, uint64_t done, uint64_t total) {
    (void)name;   /* the count is the useful part; a filename per entry scrolls */
    return spinner(ctx, "extracting", done, total);
}

static int usage(void) {
    fprintf(stderr,
        "usage: wimport <command> [args]\n"
        "  probe   <path>                     what is this: a game, an archive, or a setup?\n"
        "  game    <src> <drive_c>            import a folder, zip or .exe as a program\n"
        "  install <setup> <drive_c> [-visible] [-k] [-t s]\n"
        "                                     run a setup program and keep what it makes;\n"
        "                                     -visible draws its screens instead of going silent\n"
        "  unzip   <archive> <dir>            just extract (works on a self-extracting .exe)\n"
        "  exes    <dir> [name]               rank the executables in a folder\n"
        "  space   <src> <drive_c>            how much room an import would need\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *cmd = argv[1];

    if (!strcmp(cmd, "probe")) {
        wi_probe_result p = wi_probe(argv[2]);
        static const char *sk[] = { "executable", "folder", "archive", "not recognised" };
        printf("%s\n  looks like: %s\n", argv[2], sk[p.src]);
        printf("  bitness:    %s\n", p.is32 == 1 ? "32-bit" : p.is32 == 0 ? "64-bit" : "unknown");
        printf("  family:     %s\n", p.setup.name);
        printf("  %s\n", p.setup.note);
        printf("  import as:  %s\n", p.looks_like_installer ? "an installer" : "a game");
        if (p.looks_like_installer) {
            const char *a[SK_MAX_ARGS]; char sc[SK_MAX_ARGS * 1024];
            int n = sk_silent_argv(&p.setup, "C:\\Games\\Thing", a, sc, sizeof sc);
            printf("  silent:     ");
            for (int i = 0; i < n; i++) printf("%s ", a[i]);
            printf("%s\n", n ? "" : "(no flags)");
        }
        return 0;
    }

    if (!strcmp(cmd, "exes")) {
        dd_exe e[DD_MAX_EXES];
        int n = dd_rank_exes(argv[2], argc > 3 ? argv[3] : 0, e, DD_MAX_EXES);
        printf("%d executable(s), best first:\n", n);
        for (int i = 0; i < n; i++)
            printf("  %2d. %-46s %5d  %s  %s\n", i + 1, e[i].rel, e[i].score,
                   e[i].is32 == 1 ? "32" : e[i].is32 == 0 ? "64" : "??", e[i].why);
        return n ? 0 : 1;
    }

    if (!strcmp(cmd, "space")) {
        if (argc < 4) return usage();
        uint64_t need = 0, have = 0;
        int known = wi_space_needed(argv[2], argv[3], &need, &have);
        printf("%s\n  needs: %s%.2f GB\n  free:  %.2f GB on %s\n", argv[2],
               known ? "" : "(unknown) ", (double)need / 1073741824.0,
               (double)have / 1073741824.0, argv[3]);
        return 0;
    }

    if (!strcmp(cmd, "unzip")) {
        if (argc < 4) return usage();
        char err[256] = ""; int skipped = 0;
        int n = uz_extract(argv[2], argv[3], zip_spin, 0, &skipped, err, sizeof err);
        spinner_done();
        if (n < 0) { fprintf(stderr, "wimport: %s\n", err); return 1; }
        printf("%d file(s) into %s", n, argv[3]);
        if (skipped) printf(", %d skipped", skipped);
        printf("\n");
        return 0;
    }

    if (!strcmp(cmd, "game")) {
        if (argc < 4) return usage();
        wi_result r;
        int rc = wi_import_game(argv[2], argv[3], spinner, 0, &r);
        spinner_done();
        fputs(r.detail, stdout);
        if (rc == 0) printf("\nlibrary entry: %s\n  run: %s\n  dlls: %s\n", r.name, r.exe_host, r.dll_dir);
        return rc == 0 ? 0 : 1;
    }

    if (!strcmp(cmd, "install")) {
        if (argc < 4) return usage();
        int keep = 0, timeout = 300, visible = WI_SILENT;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "-k")) keep = 1;
            /* Run it the way a person would see it, rather than in its own
             * silent mode. Off the device this mostly proves the run reaches
             * the installer's first screen; on the device it is how a person
             * answers one. */
            else if (!strcmp(argv[i], "-visible")) visible = WI_VISIBLE;
            else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atoi(argv[++i]);
            else return usage();
        }
        wi_result r;
        int rc = wi_import_installer(argv[2], argv[3], winrun_main, visible, keep, timeout, spinner, 0, &r);
        spinner_done();
        fputs(r.detail, stdout);
        if (rc == 0) printf("\nlibrary entry: %s\n  run: %s\n  dlls: %s\n", r.name, r.exe_host, r.dll_dir);
        return rc == 0 ? 0 : 1;
    }

    return usage();
}
