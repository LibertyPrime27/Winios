/* PE loader: PE32 (i386) and PE32+ (x86-64), executables and DLLs.
 *
 * Map an image at its preferred base (relocating if that is taken), copy the
 * sections, apply base relocations, resolve imports, set up static TLS, and
 * record the result in the module table. Nothing here is Windows-specific
 * beyond the file format itself -- this is what the loader in ntdll does
 * before it calls the entry point.
 *
 * An import names a DLL. Two things can satisfy it:
 *
 *   - a DLL we implement on the host (kernel32, msvcrt, ntdll, user32), whose
 *     exports become `int3` stubs -- see w32.h;
 *   - a real PE DLL sitting next to the executable, which is loaded the same
 *     way the executable is and resolved through its export directory.
 *
 * The host list wins. A game shipping its own copy of kernel32.dll would get
 * ours, which is the point: the guest's code runs, the platform underneath it
 * does not. Its own DLLs -- the engine, the mod loader, and eventually our
 * d3d9.dll -- load as guest code.
 *
 * Loading is recursive and cycle-safe: a module is registered before its
 * imports are resolved, so a dependency loop finds the half-built module
 * rather than looping. Because a dependency finishes loading before its
 * dependent does, load order is dependency order, which is the order
 * DllMain has to be called in.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static inline uint16_t RD16(const void *p) { const uint8_t *b = p; return (uint16_t)(b[0] | b[1] << 8); }
static inline uint32_t RD32(const void *p) { const uint8_t *b = p; return (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); }
static inline uint64_t RD64(const void *p) { const uint8_t *b = p; return (uint64_t)RD32(b) | (uint64_t)RD32(b + 4) << 32; }

enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_RELOC = 5, DIR_TLS = 9 };
enum { DLL_PROCESS_ATTACH = 1 };

typedef struct { uint32_t rva, size; } datadir;

/* Lowercase, strip any path, and add ".dll" if there is no extension --
 * "..\\Bin\\Engine.DLL" and "engine" both name the same module. */
static void mod_name(const char *s, char *out, size_t n) {
    const char *b = s;
    for (const char *p = s; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    size_t i = 0;
    for (; b[i] && i + 5 < n; i++) out[i] = (char)((b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i]);
    out[i] = 0;
    if (!strchr(out, '.')) strncat(out, ".dll", n - strlen(out) - 1);
}

static w32_module *mod_find(w32 *w, const char *lname) {
    for (int i = 0; i < w->nmods; i++) if (!strcmp(w->mods[i].name, lname)) return &w->mods[i];
    return 0;
}
w32_module *w32_module_at(w32 *w, uint64_t base) {
    for (int i = 0; i < w->nmods; i++) if (w->mods[i].base == base) return &w->mods[i];
    return 0;
}

/* An RVA is only usable if it and everything read through it stay inside the
 * mapped image; a malformed or hostile file must not become a host crash. */
static int in_image(const w32_module *m, uint64_t rva, uint64_t len) {
    return rva && rva + len > rva && rva + len <= m->size;
}

static int load_image(w32 *w, const char *path, const char *lname, int is_exe, w32_module **out);

/* --------------------------------------------------------------- exports */

/* One export of `m`, by name (ordinal < 0) or by ordinal. Returns the guest
 * address, or 0. A forwarder -- an export whose address falls inside the
 * export directory -- is a "OTHERDLL.Name" string and resolves recursively. */
static uint64_t mod_export(w32 *w, w32_module *m, const char *name, int ordinal, int depth) {
    if (!m->exp_size || depth > 8) return 0;
    uint8_t *img = W32P(w, m->base);
    const uint8_t *e = img + m->exp_rva;
    uint32_t ord_base = RD32(e + 16), nfuncs = RD32(e + 20), nnames = RD32(e + 24);
    uint32_t funcs = RD32(e + 28), names = RD32(e + 32), ords = RD32(e + 36);
    if (!in_image(m, funcs, 4ull * nfuncs)) return 0;

    uint32_t idx = 0;
    if (ordinal >= 0) {
        if ((uint32_t)ordinal < ord_base || (uint32_t)ordinal - ord_base >= nfuncs) return 0;
        idx = (uint32_t)ordinal - ord_base;
    } else {
        if (!in_image(m, names, 4ull * nnames) || !in_image(m, ords, 2ull * nnames)) return 0;
        uint32_t lo = 0, hi = nnames, hit = nnames;      /* the name table is sorted */
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2, nrva = RD32(img + names + 4 * mid);
            if (!in_image(m, nrva, 1)) return 0;
            int cmp = strcmp((const char *)img + nrva, name);
            if (cmp == 0) { hit = mid; break; }
            if (cmp < 0) lo = mid + 1; else hi = mid;
        }
        if (hit == nnames) return 0;
        idx = RD16(img + ords + 2 * hit);
        if (idx >= nfuncs) return 0;
    }
    uint32_t rva = RD32(img + funcs + 4 * idx);
    if (!rva) return 0;
    if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {          /* forwarder */
        const char *fwd = (const char *)img + rva;
        const char *dot = strrchr(fwd, '.');
        if (!dot) return 0;
        char dll[128]; size_t dn = (size_t)(dot - fwd);
        if (dn >= sizeof dll) return 0;
        memcpy(dll, fwd, dn); dll[dn] = 0;
        if (w->verbose) fprintf(stderr, "winrun: %s!%s forwards to %s\n", m->name, name ? name : "#", fwd);
        return w32_import_addr(w, dll, dot + 1, -1, depth + 1);
    }
    return m->base + rva;
}

uint64_t w32_module_export(w32 *w, uint64_t hmodule, const char *name, int ordinal) {
    w32_module *m = w32_module_at(w, hmodule);
    return m ? mod_export(w, m, name, ordinal, 0) : 0;
}

/* ------------------------------------------------------- finding a module */

/* Where a guest DLL may live: beside the executable, then the search list
 * winrun was given, then the working directory. Deliberately short -- a real
 * Windows search order (SxS, KnownDLLs, %PATH%) buys nothing here and hides
 * which file was actually used. */
static int find_dll_file(w32 *w, const char *lname, char *out, size_t n) {
    char dir[512] = "";
    if (w->exe_path) {
        snprintf(dir, sizeof dir, "%s", w->exe_path);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
    }
    const char *roots[3]; int nr = 0;
    if (dir[0]) roots[nr++] = dir;
    if (w->dll_dir) roots[nr++] = w->dll_dir;
    roots[nr++] = "./";
    for (int i = 0; i < nr; i++) {
        snprintf(out, n, "%s%s", roots[i], lname);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

/* The module handle for `name`, loading it if it is a guest DLL we have not
 * seen. 0 if there is no such DLL anywhere. */
uint64_t w32_load_library(w32 *w, const char *name) {
    char lname[128]; mod_name(name, lname, sizeof lname);
    w32_module *m = mod_find(w, lname);
    if (m) { m->refs++; return m->base; }
    uint64_t host = w32_module_handle(w, lname);           /* kernel32 and friends win */
    if (host) return host;
    char path[512];
    if (!find_dll_file(w, lname, path, sizeof path)) return 0;
    if (w->nmods >= W32_MAX_MODULES) { fprintf(stderr, "winrun: too many modules (%s)\n", lname); return 0; }
    if (w->verbose) fprintf(stderr, "winrun: loading %s from %s\n", lname, path);
    if (load_image(w, path, lname, 0, &m)) return 0;
    return m->base;
}

/* The address an import resolves to: a guest DLL's export if one provides it,
 * otherwise a host stub (which also covers every name we have not implemented,
 * as a stub that reports itself if it is ever called). */
uint64_t w32_import_addr(w32 *w, const char *dll, const char *name, int ordinal, int depth) {
    char lname[128]; mod_name(dll, lname, sizeof lname);
    uint64_t h = w32_load_library(w, lname);
    w32_module *m = h ? w32_module_at(w, h) : 0;
    if (m) {
        uint64_t a = mod_export(w, m, name, ordinal, depth);
        if (a) return a;
        if (w->verbose) fprintf(stderr, "winrun: %s has no export %s\n", lname, name ? name : "(ordinal)");
    }
    char buf[32];
    if (!name) { snprintf(buf, sizeof buf, "#%d", ordinal); name = buf; }
    return w32_stub_for(w, lname, name);
}

/* --------------------------------------------------------------- loading */

static void setup_tls(w32 *w, w32_module *m, const uint8_t *t, int plus,
                      uint64_t pref_base, uint64_t base, int64_t delta) {
    int psz = plus ? 8 : 4;
    uint64_t raw_start = plus ? RD64(t) : RD32(t), raw_end = plus ? RD64(t + 8) : RD32(t + 4);
    uint64_t idx_addr = plus ? RD64(t + 16) : RD32(t + 8), cb_addr = plus ? RD64(t + 24) : RD32(t + 12);
    uint32_t zero_fill = RD32(t + (plus ? 32 : 16));
    /* the directory holds absolute VAs; the base-relocation pass has normally
     * fixed them already, but a linker that omitted those fixups would leave
     * them at the preferred base -- rebase only in that case */
    #define REBASE(v) do { if ((v) && (v) >= pref_base && (v) < pref_base + m->size && base != pref_base) (v) += (uint64_t)delta; } while (0)
    REBASE(raw_start); REBASE(raw_end); REBASE(idx_addr); REBASE(cb_addr);
    #undef REBASE
    if (w->ntls >= W32_MAX_TLS) { fprintf(stderr, "winrun: out of TLS slots for %s\n", m->name); return; }
    uint32_t index = (uint32_t)w->ntls++;

    uint64_t size = raw_end > raw_start ? raw_end - raw_start : 0;
    uint64_t block = w32_alloc(w, size + zero_fill + 16, 0);
    if (size) memcpy(W32P(w, block), W32P(w, raw_start), size);
    w32_write(w, idx_addr, 4, index);
    /* ThreadLocalStoragePointer -> the array of per-module blocks */
    if (!w32_self()->tls_array) {
        w32_self()->tls_array = w32_alloc(w, 4096, 0);
        w32_write(w, w32_self()->teb + (plus ? TEB64_TLSPTR : TEB32_TLSPTR), psz, w32_self()->tls_array);
    }
    w32_write(w, w32_self()->tls_array + (uint64_t)psz * index, psz, block);
    w->tls_slots[index] = block;
    m->tls_callbacks = cb_addr;
    if (m->is_exe) { w->tls_index = index; w->tls_callbacks = cb_addr; }
    if (w->verbose) fprintf(stderr, "winrun: %s: TLS index %u, %llu bytes\n", m->name, index, (unsigned long long)(size + zero_fill));
}

static int load_image(w32 *w, const char *path, const char *lname, int is_exe, w32_module **out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st; fstat(fd, &st);
    uint8_t *f = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) { perror("mmap"); return -1; }
    size_t fsz = (size_t)st.st_size;
    int rc = -1;

    if (fsz < 0x40 || f[0] != 'M' || f[1] != 'Z') { fprintf(stderr, "%s: not a PE file (no MZ)\n", path); goto done; }
    uint32_t pe = RD32(f + 0x3C);
    if (pe + 24 > fsz || memcmp(f + pe, "PE\0\0", 4)) { fprintf(stderr, "%s: no PE signature\n", path); goto done; }
    const uint8_t *coff = f + pe + 4;
    uint16_t machine = RD16(coff), nsec = RD16(coff + 2), chars = RD16(coff + 18), optsz = RD16(coff + 16);
    const uint8_t *opt = coff + 20;
    uint16_t magic = RD16(opt);
    int plus = magic == 0x20B;
    if (!plus && magic != 0x10B) { fprintf(stderr, "%s: unknown optional header magic %#x\n", path, magic); goto done; }
    if ((plus && machine != 0x8664) || (!plus && machine != 0x14C)) { fprintf(stderr, "%s: machine %#x is not x86\n", path, machine); goto done; }
    if (is_exe) w->is32 = !plus;
    else if (w->is32 == plus) { fprintf(stderr, "%s: %d-bit DLL in a %d-bit process\n", path, plus ? 64 : 32, w->is32 ? 32 : 64); goto done; }
    if (!is_exe && !(chars & 0x2000)) fprintf(stderr, "winrun: %s is not marked as a DLL; loading it anyway\n", path);

    uint32_t entry_rva = RD32(opt + 16);
    uint64_t pref_base = plus ? RD64(opt + 24) : RD32(opt + 28);
    uint32_t size_image = RD32(opt + 56), size_headers = RD32(opt + 60);
    uint16_t subsystem = RD16(opt + 68);
    uint64_t stack_reserve = plus ? RD64(opt + 72) : RD32(opt + 72);
    uint32_t ndirs = plus ? RD32(opt + 108) : RD32(opt + 92);
    const uint8_t *dirs = opt + (plus ? 112 : 96);
    datadir dir[16] = {{0, 0}};
    for (uint32_t i = 0; i < ndirs && i < 16; i++) { dir[i].rva = RD32(dirs + 8 * i); dir[i].size = RD32(dirs + 8 * i + 4); }
    const uint8_t *sec = opt + optsz;

    /* memory for the image: preferred base if free, else wherever, then relocate */
    uint64_t base = w32_alloc_at(w, pref_base, size_image, 1);
    if (!base) {
        if (!dir[DIR_RELOC].size) { fprintf(stderr, "%s: preferred base %#llx unavailable and no relocations\n", path, (unsigned long long)pref_base); goto done; }
        base = w32_alloc(w, size_image, 1);
        if (!base) { fprintf(stderr, "%s: cannot allocate %u bytes for the image\n", path, size_image); goto done; }
    }
    uint8_t *img = W32P(w, base);
    memcpy(img, f, size_headers < fsz ? size_headers : fsz);
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = sec + 40 * i;
        uint32_t vsize = RD32(s + 8), vaddr = RD32(s + 12), rawsz = RD32(s + 16), rawoff = RD32(s + 20);
        if ((uint64_t)vaddr + vsize > size_image) { fprintf(stderr, "%s: section %d outside the image\n", path, i); goto done; }
        uint32_t n = rawsz < vsize ? rawsz : vsize;
        if (rawoff + (uint64_t)n <= fsz) memcpy(img + vaddr, f + rawoff, n);
        if (w->verbose) fprintf(stderr, "winrun: %s section %-8.8s rva %#x vsize %#x raw %#x\n", lname, (const char *)s, vaddr, vsize, rawsz);
    }

    /* base relocations */
    int64_t delta = (int64_t)(base - pref_base);
    if (delta && dir[DIR_RELOC].size) {
        uint32_t off = dir[DIR_RELOC].rva, end = off + dir[DIR_RELOC].size;
        while (off + 8 <= end) {
            uint32_t page = RD32(img + off), blk = RD32(img + off + 4);
            if (blk < 8) break;
            for (uint32_t k = 8; k + 2 <= blk; k += 2) {
                uint16_t e = RD16(img + off + k);
                uint32_t type = e >> 12, at = page + (e & 0xFFF);
                if (type == 3) { uint32_t v = RD32(img + at) + (uint32_t)delta; memcpy(img + at, &v, 4); }
                else if (type == 10) { uint64_t v = RD64(img + at) + (uint64_t)delta; memcpy(img + at, &v, 8); }
            }
            off += blk;
        }
        if (w->verbose) fprintf(stderr, "winrun: %s relocated by %#llx to %#llx\n", lname, (unsigned long long)delta, (unsigned long long)base);
    }

    /* Register before resolving imports: a dependency cycle then finds this
     * module half-built instead of loading it a second time. */
    w32_module *m = &w->mods[w->nmods++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", lname);
    snprintf(m->path, sizeof m->path, "%s", path);
    m->base = base; m->size = size_image; m->is_exe = is_exe; m->refs = 1;
    m->entry = entry_rva ? base + entry_rva : 0;
    if (dir[DIR_EXPORT].size && in_image(m, dir[DIR_EXPORT].rva, 40)) {
        m->exp_rva = dir[DIR_EXPORT].rva; m->exp_size = dir[DIR_EXPORT].size;
    }
    if (out) *out = m;

    /* imports */
    int psz = plus ? 8 : 4;
    if (dir[DIR_IMPORT].size) {
        for (uint32_t d = dir[DIR_IMPORT].rva; ; d += 20) {
            uint32_t oft = RD32(img + d), name = RD32(img + d + 12), ft = RD32(img + d + 16);
            if (!name && !ft) break;
            char dll[128]; mod_name((const char *)img + name, dll, sizeof dll);
            uint32_t src = oft ? oft : ft;           /* ILT if present, else the IAT itself still holds the names */
            for (uint32_t k = 0; ; k++) {
                uint64_t th = plus ? RD64(img + src + 8 * k) : RD32(img + src + 4 * k);
                if (!th) break;
                uint64_t addr;
                if (th & (plus ? 1ull << 63 : 1ull << 31)) addr = w32_import_addr(w, dll, 0, (int)(th & 0xFFFF), 0);
                else addr = w32_import_addr(w, dll, (const char *)img + (uint32_t)th + 2, -1, 0);
                memcpy(img + ft + psz * k, &addr, psz);
            }
        }
    }

    if (dir[DIR_TLS].size && in_image(m, dir[DIR_TLS].rva, plus ? 40 : 24))
        setup_tls(w, m, W32P(w, base) + dir[DIR_TLS].rva, plus, pref_base, base, delta);

    m->seq = ++w->nloaded;      /* finished, so every dependency is finished too */
    if (is_exe) { w->image_base = base; w->image_size = size_image; w->entry = base + entry_rva; }
    if (w->verbose) fprintf(stderr, "winrun: %s: %s, base %#llx, entry %#llx, subsystem %u, stack %#llx\n", path,
                            plus ? "PE32+" : "PE32", (unsigned long long)base, (unsigned long long)(m->entry),
                            subsystem, (unsigned long long)stack_reserve);
    rc = 0;
done:
    munmap(f, fsz);
    return rc;
}

int w32_load_pe(w32 *w, const char *path) {
    char lname[128]; mod_name(path, lname, sizeof lname);
    return load_image(w, path, lname, 1, 0);
}

/* Every DLL that has been loaded but not yet initialised, in dependency
 * order. That is the order they *finished* loading, not the order they were
 * registered: a module is registered before its imports are resolved (so a
 * cycle terminates), which puts a dependent ahead of its dependency in the
 * table. Ordering by completion instead puts sub.dll's DllMain before
 * mid.dll's, which is what mid.dll's DllMain calling into sub.dll requires.
 *
 * Called once the initial thread has a stack, since DllMain is guest code
 * and runs on it. */
void w32_attach_modules(w32 *w) {
    for (;;) {
        w32_module *m = 0;
        for (int i = 0; i < w->nmods; i++) {
            w32_module *c = &w->mods[i];
            if (c->is_exe || c->attached) continue;
            if (!m || c->seq < m->seq) m = c;
        }
        if (!m || w->exited) return;
        m->attached = 1;
        int psz = w->is32 ? 4 : 8;
        for (uint32_t k = 0; m->tls_callbacks && !w->exited; k++) {
            uint64_t cb = w32_read(w, m->tls_callbacks + (uint64_t)psz * k, psz);
            if (!cb) break;
            uint64_t args[3] = { m->base, DLL_PROCESS_ATTACH, 0 };
            if (w->verbose) fprintf(stderr, "winrun: %s TLS callback %#llx\n", m->name, (unsigned long long)cb);
            w32_call_guest(w, cb, 3, args);
        }
        if (!m->entry || w->exited) continue;
        uint64_t dargs[3] = { m->base, DLL_PROCESS_ATTACH, 0 };
        if (w->verbose) fprintf(stderr, "winrun: %s DllMain(%#llx, DLL_PROCESS_ATTACH)\n", m->name, (unsigned long long)m->base);
        uint64_t ok = w32_call_guest(w, m->entry, 3, dargs);
        if (!w->exited && !(ok & 0xFFFFFFFFu))
            fprintf(stderr, "winrun: %s DllMain returned FALSE; continuing anyway\n", m->name);
    }
}
