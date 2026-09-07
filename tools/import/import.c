/* See import.h. */
/* strdup and lstat are POSIX rather than C, so a strict -std=c11 build hides
 * them and the calls below silently become implicit declarations returning
 * int -- which on a 64-bit target is a pointer with its top half missing.
 * The default CMake build asks for gnu11 and never saw it; the aarch64
 * cross-check with -std=c11 did. */
#define _GNU_SOURCE
#include "import.h"

#include "drivediff.h"
#include "unzip.h"

/* Declared here rather than by including w32.h, so this file -- and so the
 * zip reader, the installer detection and the drive diff beside it -- does
 * not drag in the emulator's headers. Installer mode is the only part that
 * touches the guest at all, and it does that through the runner the caller
 * passes in. The one exception is making the drive skeleton, which has to
 * happen before the setup program looks for Program Files. */
void w32_drive_init(void);

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Everything appended to the report goes through this, so a full buffer
 * truncates cleanly instead of overrunning or silently doing nothing. */
static void addf(wi_result *r, const char *fmt, ...) {
    size_t used = strlen(r->detail);
    if (used + 1 >= sizeof r->detail) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(r->detail + used, sizeof r->detail - used, fmt, ap);
    va_end(ap);
}

int wi_safe_component(const char *raw, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = raw; *p && o + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        /* Separators and the characters Windows itself forbids in a name,
         * plus control codes: what is left is safe on both filesystems. */
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) { if (o && out[o-1] != '_') out[o++] = '_'; continue; }
        out[o++] = (char)c;
    }
    while (o && (out[o-1] == ' ' || out[o-1] == '.' || out[o-1] == '_')) o--;
    out[o] = 0;
    /* A leading dot would hide the entry from the very listing that is
     * supposed to show it. */
    size_t lead = 0;
    while (out[lead] == '.' || out[lead] == ' ') lead++;
    if (lead) memmove(out, out + lead, strlen(out + lead) + 1);
    if (!out[0]) snprintf(out, n, "imported");
    return 1;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
/* The name without its extension, which is what a program should be called
 * in the library: "Cool Game.zip" is not a good entry name. */
static void stem_of(const char *p, char *out, size_t n) {
    const char *b = base_of(p);
    const char *dot = strrchr(b, '.');
    size_t l = dot && dot != b ? (size_t)(dot - b) : strlen(b);
    if (l >= n) l = n - 1;
    memcpy(out, b, l);
    out[l] = 0;
}

/* --- copying a tree --------------------------------------------------------
 *
 * Its own recursion rather than drivediff's walk: this one has to create the
 * matching directory on the way down, which a callback that only sees paths
 * cannot do in the right order.
 */
static int copy_file_raw(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb");
    if (!a) return -1;
    FILE *b = fopen(dst, "wb");
    if (!b) { fclose(a); return -1; }
    char buf[128 * 1024];
    size_t n; int bad = 0;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { bad = 1; break; }
    if (ferror(a)) bad = 1;
    fclose(a);
    if (fclose(b)) bad = 1;
    if (bad) { remove(dst); return -1; }
    struct stat st;
    if (stat(src, &st) == 0) (void)chmod(dst, st.st_mode & 07777);
    return 0;
}

typedef struct { wi_progress cb; void *ctx; int files; int cancelled; } copy_state;

static int copy_tree(const char *src, const char *dst, copy_state *cs) {
    if (uz_mkdirs(dst)) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[4096], t[4096];
        if ((size_t)snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= sizeof s) continue;
        if ((size_t)snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= sizeof t) continue;
        struct stat st;
        if (lstat(s, &st)) continue;
        if (S_ISLNK(st.st_mode)) continue;      /* never follow one out of the source */
        if (S_ISDIR(st.st_mode)) { if (copy_tree(s, t, cs) < 0) rc = -1; if (cs->cancelled) break; continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (copy_file_raw(s, t) == 0) {
            cs->files++;
            if (cs->cb && cs->cb(cs->ctx, "copying", (uint64_t)cs->files, 0)) { cs->cancelled = 1; break; }
        } else rc = -1;
    }
    closedir(d);
    return rc;
}

/* --- probing ---------------------------------------------------------------- */

wi_probe_result wi_probe(const char *path) {
    wi_probe_result p;
    memset(&p, 0, sizeof p);
    p.is32 = -1;
    stem_of(path, p.suggested_name, sizeof p.suggested_name);

    if (is_dir(path)) {
        p.src = WI_SRC_FOLDER;
        dd_exe e[DD_MAX_EXES];
        int n = dd_rank_exes(path, p.suggested_name, e, DD_MAX_EXES);
        if (n > 0) p.is32 = e[0].is32;
        /* A folder that is *only* an installer -- what an installer download
         * unzips to -- should be offered as an install, not as a game. */
        if (n > 0) {
            sk_info k = sk_identify(path);          /* a directory: not a PE */
            (void)k;
            char first[4096];
            snprintf(first, sizeof first, "%s/%s", path, e[0].rel);
            p.setup = sk_identify(first);
            p.looks_like_installer = p.setup.kind != SK_PLAIN && p.setup.kind != SK_NOT_PE;
        }
        return p;
    }

    /* An archive before an executable: a self-extracting .exe is both, and
     * unpacking it is always better than running it. */
    if (uz_is_zip(path)) {
        p.src = WI_SRC_ZIP;
        p.setup = sk_identify(path);
        /* A zip SFX is an archive we can open, so it is not an installer as
         * far as the user is concerned. */
        if (p.setup.kind == SK_SFX_ZIP) p.looks_like_installer = 0;
        else p.looks_like_installer = p.setup.kind != SK_PLAIN && p.setup.kind != SK_NOT_PE;
        return p;
    }

    p.setup = sk_identify(path);
    p.src = p.setup.kind == SK_NOT_PE ? WI_SRC_UNKNOWN : WI_SRC_EXE;
    p.is32 = dd_pe_is32(path);
    p.looks_like_installer = p.setup.kind != SK_PLAIN && p.setup.kind != SK_NOT_PE;
    return p;
}

/* --- naming the destination ------------------------------------------------- */

/* A name nothing on the drive is using yet. Overwriting an existing entry
 * would destroy a program the user still has, and refusing outright means
 * they cannot import two versions of the same game. */
static void unique_dest(const char *drive_c, const char *want, char *name, size_t nn,
                        char *full, size_t fn) {
    for (int i = 0; i < 1000; i++) {
        if (i == 0) snprintf(name, nn, "%s", want);
        else snprintf(name, nn, "%s (%d)", want, i + 1);
        snprintf(full, fn, "%s/%s", drive_c, name);
        struct stat st;
        if (stat(full, &st) != 0) return;
    }
}

/* Fill in the executable fields once the files are in place. Shared by both
 * modes: "which of these do I run" has one answer however the files arrived. */
static void pick_exe(wi_result *r, const char *drive_c, const char *entry_dir) {
    dd_exe e[DD_MAX_EXES];
    int n = dd_rank_exes(entry_dir, r->name, e, DD_MAX_EXES);
    r->exes = n;
    if (n <= 0) { addf(r, "\nNo executable was found, so there is nothing to run.\n"); return; }

    snprintf(r->exe_rel, sizeof r->exe_rel, "%s", e[0].rel);
    snprintf(r->exe_host, sizeof r->exe_host, "%s/%s", entry_dir, e[0].rel);
    r->is32 = e[0].is32;
    /* A game finds its DLLs beside its executable, which is not necessarily
     * the top of the folder -- Unreal puts the binary three levels down. */
    snprintf(r->dll_dir, sizeof r->dll_dir, "%s", r->exe_host);
    char *slash = strrchr(r->dll_dir, '/');
    if (slash) *slash = 0;
    (void)drive_c;

    addf(r, "\nWill run: %s (%s)\n  %s\n", e[0].rel,
         e[0].is32 == 1 ? "32-bit" : e[0].is32 == 0 ? "64-bit" : "unreadable header",
         e[0].why);
    if (n > 1) {
        addf(r, "\n%d other executable%s here, in case that is the wrong one:\n",
             n - 1, n == 2 ? "" : "s");
        for (int i = 1; i < n && i < 8; i++)
            addf(r, "  %-44s  %s\n", e[i].rel, e[i].why);
        if (n > 8) addf(r, "  ... and %d more\n", n - 8);
    }
}

/* --- game mode -------------------------------------------------------------- */

int wi_import_game(const char *src, const char *drive_c,
                   wi_progress cb, void *ctx, wi_result *out) {
    memset(out, 0, sizeof *out);
    out->is32 = -1;
    if (!src || !drive_c || !*drive_c) { addf(out, "no source or no C: drive\n"); return -1; }

    char want[256], name[256], dest[2048];
    { char stem[256]; stem_of(src, stem, sizeof stem); wi_safe_component(stem, want, sizeof want); }
    unique_dest(drive_c, want, name, sizeof name, dest, sizeof dest);
    snprintf(out->name, sizeof out->name, "%s", name);

    wi_probe_result p = wi_probe(src);

    if (p.src == WI_SRC_ZIP) {
        addf(out, "%s is an archive; extracting it.\n", base_of(src));
        /* Into a staging directory first. An archive almost always holds a
         * single top-level folder -- that is what a download unpacks to -- and
         * extracting straight into the entry would nest it one level deeper
         * than the game expects, so every relative path it opens would be
         * wrong. Which shape it is cannot be known until it is open, hence a
         * staging name that is then either promoted or renamed. */
        char stage[2100];
        snprintf(stage, sizeof stage, "%s.part", dest);
        if (is_dir(stage)) { addf(out, "%s.part is in the way; remove it and try again.\n", name); return -1; }
        if (uz_mkdirs(stage)) { addf(out, "could not create %s\n", name); return -1; }

        int skipped = 0; char err[256] = "";
        int n = uz_extract(src, stage, (uz_progress)cb, ctx, &skipped, err, sizeof err);
        if (n < 0) { addf(out, "extraction failed: %s\n", err); return -1; }
        out->files = n;
        addf(out, "%d file%s extracted", n, n == 1 ? "" : "s");
        if (skipped) addf(out, ", %d skipped (unsupported compression, or an unsafe path)", skipped);
        addf(out, ".\n");

        /* One folder and nothing else: that folder *is* the program, and its
         * name is a better name for the library entry than the archive's --
         * "Blastoff" rather than "blastoff-win-v1.2-itch". */
        char only[256] = "";
        { DIR *d = opendir(stage); struct dirent *e; int dirs = 0, files = 0;
          while (d && (e = readdir(d))) {
              if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
              char q[2400]; snprintf(q, sizeof q, "%s/%s", stage, e->d_name);
              if (is_dir(q)) { dirs++; snprintf(only, sizeof only, "%.*s", (int)sizeof only - 1, e->d_name); }
              else files++;
          }
          if (d) closedir(d);
          if (!(dirs == 1 && files == 0)) only[0] = 0; }

        if (only[0]) {
            char want2[256];
            wi_safe_component(only, want2, sizeof want2);
            unique_dest(drive_c, want2, name, sizeof name, dest, sizeof dest);
            snprintf(out->name, sizeof out->name, "%s", name);
            char inner[2400];
            snprintf(inner, sizeof inner, "%s/%s", stage, only);
            if (rename(inner, dest)) { addf(out, "could not move %s into place\n", only); return -1; }
            (void)rmdir(stage);
            addf(out, "Everything was inside \"%s\", so that is the program and its name.\n", only);
        } else {
            if (rename(stage, dest)) { addf(out, "could not move the files into place\n"); return -1; }
        }
    } else if (p.src == WI_SRC_FOLDER) {
        addf(out, "Copying %s onto C:.\n", base_of(src));
        copy_state cs = { cb, ctx, 0, 0 };
        if (copy_tree(src, dest, &cs) < 0 && cs.files == 0) {
            addf(out, "nothing could be copied out of %s\n", base_of(src));
            return -1;
        }
        out->files = cs.files;
        addf(out, "%d file%s copied%s.\n", cs.files, cs.files == 1 ? "" : "s",
             cs.cancelled ? " before you cancelled" : "");
    } else if (p.src == WI_SRC_EXE) {
        if (uz_mkdirs(dest)) { addf(out, "could not create %s\n", name); return -1; }
        char t[3072]; snprintf(t, sizeof t, "%s/%s", dest, base_of(src));
        if (copy_file_raw(src, t)) { addf(out, "could not copy %s\n", base_of(src)); return -1; }
        out->files = 1;
        addf(out, "Copied %s onto C:. A single executable with no folder around it "
                  "is unusual for a game -- if it needs DLLs or data files, import "
                  "the whole folder instead.\n", base_of(src));
        if (p.looks_like_installer)
            addf(out, "\nNote: this looks like %s, not a game. Importing it as an "
                      "installer would run it and keep what it produces.\n", p.setup.name);
    } else {
        addf(out, "%s is neither a folder, an archive, nor a Windows executable.\n", base_of(src));
        return -1;
    }

    pick_exe(out, drive_c, dest);
    out->ok = out->exes > 0;
    return out->ok ? 0 : -1;
}

/* --- installer mode --------------------------------------------------------- */

/* What is on the drive but is not the install.
 *
 * Temp, because an installer unpacks itself into it and leaves most of it
 * behind -- counting that as the product would bury the twenty files that
 * matter under two thousand that do not. And the registry, because it is a
 * file at the root of the drive (advapi32.c keeps it as text) and *every*
 * installer touches it, so it would appear in every report as though it were
 * something that had been installed. */
static int is_not_the_install(const char *rel) {
    return !strncasecmp(rel, "Temp/", 5)
        || !strncasecmp(rel, "Windows/Temp/", 13)
        || !strncasecmp(rel, "Users/winios/AppData/Local/Temp/", 32)
        || !strcasecmp(rel, "registry.txt");
}

typedef struct {
    wi_result *r;
    int n;
    int exes;
    char deepest_exe_dir[1024];
    char lines[3072];
} added_ctx;

static int added_step(void *c, const char *rel, uint64_t size) {
    added_ctx *a = (added_ctx *)c;
    if (is_not_the_install(rel)) return 0;
    a->n++;
    const char *dot = strrchr(rel, '.');
    if (dot && !strcasecmp(dot, ".exe")) {
        a->exes++;
        /* Where the executables landed is a better answer than the common
         * prefix of everything: an installer that also drops a file in
         * Windows\System32 has a common prefix of nothing at all. */
        char dir[1024]; snprintf(dir, sizeof dir, "%s", rel);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0; else dir[0] = 0;
        if (!a->deepest_exe_dir[0] || strlen(dir) < strlen(a->deepest_exe_dir))
            snprintf(a->deepest_exe_dir, sizeof a->deepest_exe_dir, "%s", dir);
    }
    if (a->n <= 40) {
        size_t used = strlen(a->lines);
        if (used + 80 < sizeof a->lines)
            snprintf(a->lines + used, sizeof a->lines - used, "  %-56s %llu\n",
                     rel, (unsigned long long)size);
    }
    return 0;
}

int wi_import_installer(const char *setup, const char *drive_c,
                        wi_runner run, int keep_going, int timeout_s,
                        wi_progress cb, void *ctx, wi_result *out) {
    memset(out, 0, sizeof *out);
    out->is32 = -1;
    if (!setup || !drive_c || !*drive_c || !run) { addf(out, "nothing to run\n"); return -1; }

    wi_probe_result p = wi_probe(setup);
    addf(out, "%s: %s\n%s\n", base_of(setup), p.setup.name, p.setup.note);

    /* A zip payload never has to be executed, whatever the wrapper claims to
     * be -- and not executing it removes every way it can fail. */
    if (p.src == WI_SRC_ZIP) {
        addf(out, "\nThe payload is a zip, so it is being unpacked rather than run.\n");
        return wi_import_game(setup, drive_c, cb, ctx, out) == 0
             ? (out->ok = 1, 0)
             : -1;
    }
    if (p.setup.kind == SK_PLAIN) {
        addf(out, "\nThis has no installer markers. Import it as a game instead -- "
                  "running a program in the hope that it installs itself will "
                  "usually just run it.\n");
        return -1;
    }

    /* Where it should install to, and what it will be called afterwards. */
    char want[256], name[256], dest[2048];
    { char stem[256]; stem_of(setup, stem, sizeof stem);
      /* "setup", "install", "SomeGame_v1.2_setup" -- strip the part that
       * names the installer rather than the program, so the library entry is
       * called after the game. */
      static const char *tails[] = { "_setup", "-setup", " setup", "setup",
                                     "_install", "-install", " installer", "installer" };
      for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) {
          size_t sl = strlen(stem), tl = strlen(tails[i]);
          if (sl > tl && !strcasecmp(stem + sl - tl, tails[i])) { stem[sl - tl] = 0; break; }
      }
      while (*stem && (stem[strlen(stem) - 1] == '_' || stem[strlen(stem) - 1] == '-'
                       || stem[strlen(stem) - 1] == ' ')) stem[strlen(stem) - 1] = 0;
      if (!stem[0]) snprintf(stem, sizeof stem, "installed");
      wi_safe_component(stem, want, sizeof want); }
    unique_dest(drive_c, want, name, sizeof name, dest, sizeof dest);
    snprintf(out->name, sizeof out->name, "%s", name);

    /* The Windows path the installer is told to use. It has to be a Windows
     * path because it is going to the guest, not to us. */
    char win_dir[512];
    snprintf(win_dir, sizeof win_dir, "C:\\%s", name);

    w32_drive_init();

    if (!p.setup.silent_supported)
        addf(out, "\nRunning it anyway, because the alternative is not trying -- but "
                  "this family is not expected to finish here.\n");

    /* Before. */
    if (cb) cb(ctx, "looking at the drive", 0, 0);
    dd_snap *before = dd_snapshot(drive_c);

    /* Build the command line: the setup program, then its family's silent
     * flags with the destination substituted in. */
    const char *sargs[SK_MAX_ARGS];
    char scratch[SK_MAX_ARGS * 1024];
    int ns = sk_silent_argv(&p.setup, win_dir, sargs, scratch, sizeof scratch);

    char *argv[SK_MAX_ARGS + 10];
    char tbuf[32];
    int argc = 0;
    argv[argc++] = (char *)"winrun";
    if (keep_going) argv[argc++] = (char *)"-k";
    argv[argc++] = (char *)"-t";
    snprintf(tbuf, sizeof tbuf, "%d", timeout_s > 0 ? timeout_s : 300);
    argv[argc++] = tbuf;
    argv[argc++] = (char *)"-C";
    argv[argc++] = (char *)drive_c;
    argv[argc++] = (char *)setup;
    for (int i = 0; i < ns; i++) argv[argc++] = (char *)sargs[i];
    argv[argc] = 0;

    addf(out, "\nRunning:");
    for (int i = 5; i < argc; i++) addf(out, " %s", i == 5 ? base_of(argv[i]) : argv[i]);
    addf(out, "\n");

    if (cb) cb(ctx, "installing", 0, 0);
    int rc = run(argc, argv);
    addf(out, "The installer exited %d.\n", rc);

    /* After. */
    if (cb) cb(ctx, "looking at what appeared", 0, 0);
    dd_snap *now = dd_snapshot(drive_c);
    added_ctx a; memset(&a, 0, sizeof a); a.r = out;
    dd_added(before, now, added_step, &a);
    dd_free(before); dd_free(now);

    out->files = a.n;
    if (a.n == 0) {
        addf(out, "\nNothing new appeared on the drive, so nothing was installed.\n"
                  "The run report above lists what it called that is not implemented "
                  "here -- that list is the reason, and it is the work.\n");
        return -1;
    }

    addf(out, "\n%d file%s appeared (temporary files and the registry ignored)%s\n", a.n, a.n == 1 ? "" : "s",
         a.n > 40 ? ", first 40:" : ":");
    addf(out, "%s", a.lines);
    if (a.n > 40) addf(out, "  ... and %d more\n", a.n - 40);

    if (a.exes == 0) {
        addf(out, "\nNone of them is an executable, so the install did not finish. "
                  "Files were unpacked and then something stopped -- the report above "
                  "says what.\n");
        return -1;
    }

    /* Where to look for the program. The directory it was told to use, if it
     * honoured it; otherwise wherever the executables actually landed. */
    char entry[2048];
    struct stat st;
    if (stat(dest, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(entry, sizeof entry, "%s", dest);
    } else if (a.deepest_exe_dir[0]) {
        snprintf(entry, sizeof entry, "%s/%s", drive_c, a.deepest_exe_dir);
        /* The entry name has to match where the files are, or the library
         * would point at a folder that does not exist. */
        const char *b = base_of(a.deepest_exe_dir);
        snprintf(out->name, sizeof out->name, "%.*s",
                 (int)sizeof out->name - 1, b && *b ? b : name);
        addf(out, "\nIt ignored the directory it was given and installed to %s instead.\n",
             a.deepest_exe_dir);
    } else {
        snprintf(entry, sizeof entry, "%s", drive_c);
    }

    pick_exe(out, drive_c, entry);
    out->ok = out->exes > 0;
    return out->ok ? 0 : -1;
}
