/* See drivediff.h.
 *
 * strdup and lstat are POSIX rather than C, so a strict -std=c11 build hides
 * them and the calls below silently become implicit declarations returning
 * int -- which on a 64-bit target is a pointer with its top half missing.
 * The default CMake build asks for gnu11 and never saw it; the aarch64
 * cross-check with -std=c11 did. */
#define _GNU_SOURCE
#include "drivediff.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* --- snapshots -------------------------------------------------------------
 *
 * A flat array of entries with the paths in one arena, sorted by path. That
 * makes the diff a merge rather than a lookup per file, which matters because
 * a game's install can be tens of thousands of files and the diff runs twice
 * for every import.
 */
typedef struct { uint32_t off; uint64_t size; int64_t mtime; } dd_ent;

struct dd_snap {
    dd_ent *e;
    int n, cap;
    char *arena;
    size_t alen, acap;
};

static int snap_grow(dd_snap *s, size_t extra) {
    if (s->n + 1 > s->cap) {
        int c = s->cap ? s->cap * 2 : 1024;
        dd_ent *p = (dd_ent *)realloc(s->e, (size_t)c * sizeof *p);
        if (!p) return -1;
        s->e = p; s->cap = c;
    }
    if (s->alen + extra > s->acap) {
        size_t c = s->acap ? s->acap * 2 : (64u << 10);
        while (c < s->alen + extra) c *= 2;
        char *p = (char *)realloc(s->arena, c);
        if (!p) return -1;
        s->arena = p; s->acap = c;
    }
    return 0;
}

/* Iterative, with an explicit stack of directories to visit: a game install
 * can nest deeply enough that recursion is a needless risk, and the walk is
 * shared by the snapshot and the executable ranking. */
typedef int (*walk_fn)(void *ctx, const char *rel, const char *full,
                       uint64_t size, int64_t mtime, int is_dir);

static int walk(const char *root, walk_fn fn, void *ctx) {
    char **stack = 0; int sn = 0, scap = 0;
    int rc = 0;
    /* the relative prefix is pushed alongside, so a callback never has to
     * work out where in the tree it is */
    char **rels = 0;

    #define PUSH(f, r) do { \
        if (sn + 1 > scap) { int c = scap ? scap * 2 : 64; \
            char **a = (char **)realloc(stack, (size_t)c * sizeof *a); \
            char **b = (char **)realloc(rels,  (size_t)c * sizeof *b); \
            if (!a || !b) { free(a ? a : stack); free(b ? b : rels); return -1; } \
            stack = a; rels = b; scap = c; } \
        stack[sn] = strdup(f); rels[sn] = strdup(r); \
        if (!stack[sn] || !rels[sn]) { rc = -1; } \
        sn++; } while (0)

    PUSH(root, "");
    while (sn > 0 && rc == 0) {
        char *dirf = stack[--sn], *dirr = rels[sn];
        DIR *d = opendir(dirf);
        if (!d) { free(dirf); free(dirr); continue; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char full[4096], rel[4096];
            if ((size_t)snprintf(full, sizeof full, "%s/%s", dirf, de->d_name) >= sizeof full) continue;
            if ((size_t)snprintf(rel, sizeof rel, "%s%s%s", dirr, *dirr ? "/" : "", de->d_name) >= sizeof rel) continue;
            struct stat st;
            if (lstat(full, &st)) continue;
            if (S_ISLNK(st.st_mode)) continue;     /* never follow one out of the drive */
            int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (fn(ctx, rel, full, is_dir ? 0 : (uint64_t)st.st_size,
                   (int64_t)st.st_mtime, is_dir)) { rc = 1; break; }
            if (is_dir) PUSH(full, rel);
        }
        closedir(d);
        free(dirf); free(dirr);
    }
    #undef PUSH
    for (int i = 0; i < sn; i++) { free(stack[i]); free(rels[i]); }
    free(stack); free(rels);
    return rc < 0 ? -1 : 0;
}

static int snap_add(void *ctx, const char *rel, const char *full,
                    uint64_t size, int64_t mtime, int is_dir) {
    (void)full;
    if (is_dir) return 0;
    dd_snap *s = (dd_snap *)ctx;
    size_t len = strlen(rel) + 1;
    if (snap_grow(s, len)) return 1;
    memcpy(s->arena + s->alen, rel, len);
    s->e[s->n].off = (uint32_t)s->alen;
    s->e[s->n].size = size;
    s->e[s->n].mtime = mtime;
    s->n++;
    s->alen += len;
    return 0;
}

static const char *snap_path(const dd_snap *s, int i) { return s->arena + s->e[i].off; }

/* qsort's comparator gets the entries, but ordering them needs the arena, and
 * qsort_r is spelled differently on Linux and Darwin. One file-scope pointer
 * for the duration of a sort is the portable answer; nothing here sorts two
 * snapshots at once. */
static const dd_snap *g_sorting;
static int by_path(const void *a, const void *b) {
    const dd_ent *x = (const dd_ent *)a, *y = (const dd_ent *)b;
    return strcmp(g_sorting->arena + x->off, g_sorting->arena + y->off);
}

dd_snap *dd_snapshot(const char *root) {
    struct stat st;
    if (stat(root, &st) || !S_ISDIR(st.st_mode)) return 0;
    dd_snap *s = (dd_snap *)calloc(1, sizeof *s);
    if (!s) return 0;
    if (walk(root, snap_add, s) < 0) { dd_free(s); return 0; }
    if (s->n > 1) { g_sorting = s; qsort(s->e, (size_t)s->n, sizeof *s->e, by_path); g_sorting = 0; }
    return s;
}

void dd_free(dd_snap *s) { if (!s) return; free(s->e); free(s->arena); free(s); }
int dd_count(const dd_snap *s) { return s ? s->n : 0; }

int dd_added(const dd_snap *before, const dd_snap *now, dd_added_fn cb, void *ctx) {
    if (!now) return 0;
    int i = 0, j = 0, n = 0;
    const int bn = before ? before->n : 0;
    while (j < now->n) {
        const char *np = snap_path(now, j);
        int cmp = 1;
        while (i < bn && (cmp = strcmp(snap_path(before, i), np)) < 0) i++;
        int is_new = 1;
        if (i < bn && cmp == 0) {
            is_new = before->e[i].size != now->e[j].size
                  || before->e[i].mtime != now->e[j].mtime;
            i++;
        }
        if (is_new) {
            n++;
            if (cb && cb(ctx, np, now->e[j].size)) return n;
        }
        j++;
    }
    return n;
}

/* --- where the install landed ---------------------------------------------- */

typedef struct { char pre[4096]; int first; } prefix_ctx;

static int prefix_step(void *c, const char *rel, uint64_t size) {
    (void)size;
    prefix_ctx *p = (prefix_ctx *)c;
    if (p->first) { snprintf(p->pre, sizeof p->pre, "%s", rel); p->first = 0; return 0; }
    /* Shrink to the common prefix, then back off to the last separator so the
     * answer is a directory and not half a filename. */
    size_t i = 0;
    while (p->pre[i] && rel[i] && p->pre[i] == rel[i]) i++;
    p->pre[i] = 0;
    char *slash = strrchr(p->pre, '/');
    if (slash) *slash = 0; else p->pre[0] = 0;
    return 0;
}

int dd_added_root(const dd_snap *before, const dd_snap *now, char *out, size_t n) {
    prefix_ctx p; p.pre[0] = 0; p.first = 1;
    if (dd_added(before, now, prefix_step, &p) == 0) return 0;
    if (!p.pre[0]) return 0;
    snprintf(out, n, "%s", p.pre);
    return 1;
}

/* --- which executable to run ----------------------------------------------- */

int dd_pe_is32(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[0x200] = {0};
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got < 0x40 || h[0] != 'M' || h[1] != 'Z') return -1;
    uint32_t pe = (uint32_t)h[0x3C] | (uint32_t)h[0x3D] << 8
                | (uint32_t)h[0x3E] << 16 | (uint32_t)h[0x3F] << 24;
    if (pe + 26 > got) return -1;
    uint16_t magic = (uint16_t)(h[pe + 24] | h[pe + 25] << 8);
    if (magic == 0x10B) return 1;
    if (magic == 0x20B) return 0;
    return -1;
}

/* A managed (.NET) image: the CLR header data directory is present. Such a
 * program's only import is mscoree!_CorExeMain and it needs a runtime this
 * project does not have, so the importer says so up front rather than after a
 * run that stops on one missing name. */
int dd_pe_is_managed(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[0x400] = {0};
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got < 0x40 || h[0] != 'M' || h[1] != 'Z') return 0;
    uint32_t pe = (uint32_t)h[0x3C] | (uint32_t)h[0x3D] << 8 | (uint32_t)h[0x3E] << 16 | (uint32_t)h[0x3F] << 24;
    if (pe + 26 > got) return 0;
    uint16_t magic = (uint16_t)(h[pe + 24] | h[pe + 25] << 8);
    uint32_t dirs = pe + 24 + (magic == 0x20B ? 112 : magic == 0x10B ? 96 : 0);
    if (!dirs) return 0;
    uint32_t com = dirs + 14 * 8;                     /* IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR */
    if (com + 8 > got) return 0;
    uint32_t size = (uint32_t)h[com + 4] | (uint32_t)h[com + 5] << 8 | (uint32_t)h[com + 6] << 16 | (uint32_t)h[com + 7] << 24;
    return size != 0;
}

/* Names that are never the game, matched as a whole word or a prefix. Being
 * wrong here only costs the executable its ranking, not its place in the
 * list, so a slightly over-eager entry is cheap and a missing one is not. */
static const char *NOT_THE_GAME[] = {
    "unins", "uninstall", "setup", "install", "vcredist", "vc_redist",
    "dxsetup", "dxwebsetup", "directx", "dotnet", "ndp4", "oalinst",
    "crashhandler", "crashreport", "crashsender", "unitycrashhandler",
    "notification_helper",           /* Godot's Windows helper */
    "python", "pythonw", "ffmpeg", "7za", "7z", "curl", "wget",
    "console", "config", "settings", "options", "editor", "server",
    "webview2", "msiexec", "regsvr32", "cleanup", "repair", "patcher",
    "benchmark", "report", "helper", "updater", "update",
};
/* Directories whose contents are never the game. */
static const char *NOT_HERE[] = {
    "_commonredist", "commonredist", "redist", "redistributable", "redistributables",
    "directx", "vcredist", "support", "prereq", "prerequisites", "dotnet",
    "tools", "docs", "manual", "extras", "soundtrack", "installers",
};

/* Case-insensitive substring. strcasestr exists on both platforms this
 * builds for, but only behind _GNU_SOURCE on one of them, and a five-line
 * helper is cheaper than a feature macro that has to be got right in three
 * build systems. */
static int ci_has(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (const char *p = hay; *p; p++) if (!strncasecmp(p, needle, n)) return 1;
    return 0;
}

static int name_in(const char *name, const char **list, size_t n) {
    for (size_t i = 0; i < n; i++) if (!strncasecmp(name, list[i], strlen(list[i]))) return 1;
    return 0;
}
static int dir_in_path(const char *rel, const char **list, size_t n) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *p = tmp; *p; ) {
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = 0;
        for (size_t i = 0; i < n; i++) if (!strcasecmp(p, list[i])) return 1;
        p = slash + 1;
    }
    return 0;
}
static int exists(const char *fmt, ...) {
    char path[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(path, sizeof path, fmt, ap);
    va_end(ap);
    struct stat st;
    return stat(path, &st) == 0;
}

typedef struct { dd_exe *out; int n, max; const char *root; const char *hint; } rank_ctx;

static int rank_step(void *c, const char *rel, const char *full,
                     uint64_t size, int64_t mtime, int is_dir) {
    (void)mtime;
    rank_ctx *r = (rank_ctx *)c;
    if (is_dir || r->n >= r->max) return 0;
    const char *dot = strrchr(rel, '.');
    if (!dot || strcasecmp(dot, ".exe")) return 0;

    dd_exe *e = &r->out[r->n];
    memset(e, 0, sizeof *e);
    snprintf(e->rel, sizeof e->rel, "%s", rel);
    e->size = size;
    e->is32 = dd_pe_is32(full);

    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    char stem[256];
    { size_t len = (size_t)(strrchr(base, '.') - base);
      if (len >= sizeof stem) len = sizeof stem - 1;
      memcpy(stem, base, len); stem[len] = 0; }

    int score = 0;
    char why[96] = "";
    #define NOTE(s) do { if (!why[0]) snprintf(why, sizeof why, "%s", s); } while (0)

    /* Engine fingerprints. These are the only signals worth much, because
     * they are structural rather than nominal: a Unity game *has* to have a
     * `<Name>_Data` folder beside its executable or it will not start. */
    char dir[4096];
    { snprintf(dir, sizeof dir, "%s/%s", r->root, rel);
      char *slash = strrchr(dir, '/'); if (slash) *slash = 0; }
    if (exists("%s/%s_Data", dir, stem))       { score += 120; NOTE("Unity (has a _Data folder)"); }
    if (exists("%s/UnityPlayer.dll", dir) && score < 120) { score += 40; NOTE("beside UnityPlayer.dll"); }
    if (exists("%s/%s.pck", dir, stem))        { score += 110; NOTE("Godot (has a .pck)"); }
    if (exists("%s/data.win", dir))            { score +=  90; NOTE("GameMaker (data.win beside it)"); }
    if (exists("%s/%s.pak", dir, stem))        { score +=  40; NOTE("has a matching .pak"); }
    if (strstr(rel, "Binaries/Win64/") || strstr(rel, "Binaries/Win32/")) {
        score += 100; NOTE("Unreal (in Binaries/Win<n>)");
    }
    if (ci_has(stem, "-Shipping"))              { score +=  60; NOTE("Unreal shipping build"); }
    if (exists("%s/nw.pak", dir) || exists("%s/resources.pak", dir)) { score += 50; NOTE("Chromium/NW.js bundle"); }

    /* Position. The game is at the top of its folder far more often than not,
     * and every level down makes it less likely. */
    int depth = 0;
    for (const char *p = rel; *p; p++) if (*p == '/') depth++;
    score += depth == 0 ? 40 : depth == 1 ? 15 : -10 * depth;

    /* The name the user knows it by. */
    if (r->hint && *r->hint && !strncasecmp(stem, r->hint, strlen(stem)) && strlen(stem) >= 3) {
        score += 50; NOTE("name matches the folder");
    }

    /* Size, as a weak tiebreak: a main binary is usually the biggest, but a
     * bundled runtime can be bigger, so this is worth less than any
     * fingerprint. Log-ish rather than linear, so a 400 MB executable does
     * not outrank a fingerprint on its own. */
    { uint64_t mb = size >> 20; int s = 0;
      while (mb && s < 25) { mb >>= 1; s += 5; }
      score += s; }

    if (name_in(stem, NOT_THE_GAME, sizeof NOT_THE_GAME / sizeof NOT_THE_GAME[0])) {
        score -= 150; snprintf(why, sizeof why, "a helper or installer, by name");
    }
    if (dir_in_path(rel, NOT_HERE, sizeof NOT_HERE / sizeof NOT_HERE[0])) {
        score -= 120; snprintf(why, sizeof why, "in a redistributable/support folder");
    }
    if (e->is32 < 0) { score -= 200; snprintf(why, sizeof why, "not a readable PE"); }
    #undef NOTE

    e->score = score;
    snprintf(e->why, sizeof e->why, "%s", why[0] ? why : "no strong signal");
    r->n++;
    return 0;
}

static int by_score(const void *a, const void *b) {
    const dd_exe *x = (const dd_exe *)a, *y = (const dd_exe *)b;
    if (x->score != y->score) return y->score - x->score;
    if (x->size != y->size) return x->size < y->size ? 1 : -1;
    return strcmp(x->rel, y->rel);
}

int dd_rank_exes(const char *root, const char *hint, dd_exe *out, int max) {
    struct stat st;
    if (stat(root, &st)) return 0;
    /* A single file: it is the answer, and there is nothing to rank. */
    if (!S_ISDIR(st.st_mode)) {
        if (max < 1) return 0;
        memset(out, 0, sizeof *out);
        const char *base = strrchr(root, '/');
        snprintf(out->rel, sizeof out->rel, "%s", base ? base + 1 : root);
        out->size = (uint64_t)st.st_size;
        out->is32 = dd_pe_is32(root);
        out->score = 100;
        snprintf(out->why, sizeof out->why, "the only file");
        return 1;
    }
    rank_ctx r = { out, 0, max > DD_MAX_EXES ? DD_MAX_EXES : max, root, hint };
    walk(root, rank_step, &r);
    if (r.n > 1) qsort(out, (size_t)r.n, sizeof *out, by_score);
    return r.n;
}
