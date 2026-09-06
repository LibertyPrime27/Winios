/* PE loader: PE32 (i386) and PE32+ (x86-64).
 *
 * Map the image at its preferred base (relocating if that is taken), copy
 * the sections, apply base relocations, resolve imports to stubs (or data
 * exports), set up static TLS, and hand back the entry point. Nothing here is
 * Windows-specific beyond the file format itself -- this is what the loader
 * in ntdll does before it calls the entry point.
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

typedef struct { uint32_t rva, size; } datadir;

static const char *lower(const char *s, char *buf, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) buf[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    buf[i] = 0;
    return buf;
}

int w32_load_pe(w32 *w, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st; fstat(fd, &st);
    uint8_t *f = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) { perror("mmap"); return -1; }
    size_t fsz = (size_t)st.st_size;

    if (fsz < 0x40 || f[0] != 'M' || f[1] != 'Z') { fprintf(stderr, "%s: not a PE file (no MZ)\n", path); return -1; }
    uint32_t pe = RD32(f + 0x3C);
    if (pe + 24 > fsz || memcmp(f + pe, "PE\0\0", 4)) { fprintf(stderr, "%s: no PE signature\n", path); return -1; }
    const uint8_t *coff = f + pe + 4;
    uint16_t machine = RD16(coff), nsec = RD16(coff + 2), optsz = RD16(coff + 16);
    const uint8_t *opt = coff + 20;
    uint16_t magic = RD16(opt);
    int plus = magic == 0x20B;
    if (!plus && magic != 0x10B) { fprintf(stderr, "%s: unknown optional header magic %#x\n", path, magic); return -1; }
    if ((plus && machine != 0x8664) || (!plus && machine != 0x14C)) { fprintf(stderr, "%s: machine %#x is not x86\n", path, machine); return -1; }
    w->is32 = !plus;

    uint32_t entry_rva = RD32(opt + 16);
    uint64_t pref_base = plus ? RD64(opt + 24) : RD32(opt + 28);
    uint32_t sect_align = RD32(opt + 32), file_align = RD32(opt + 36);
    uint32_t size_image = RD32(opt + 56), size_headers = RD32(opt + 60);
    uint16_t subsystem = RD16(opt + 68);
    uint64_t stack_reserve = plus ? RD64(opt + 72) : RD32(opt + 72);
    uint32_t ndirs = plus ? RD32(opt + 108) : RD32(opt + 92);
    const uint8_t *dirs = opt + (plus ? 112 : 96);
    datadir dir[16] = {{0, 0}};
    for (uint32_t i = 0; i < ndirs && i < 16; i++) { dir[i].rva = RD32(dirs + 8 * i); dir[i].size = RD32(dirs + 8 * i + 4); }
    const uint8_t *sec = opt + optsz;
    (void)file_align; (void)sect_align;

    /* memory for the image: preferred base if free, else wherever, then relocate */
    uint64_t base = w32_alloc_at(w, pref_base, size_image, 1);
    if (!base) {
        if (!dir[DIR_RELOC].size) { fprintf(stderr, "%s: preferred base %#llx unavailable and no relocations\n", path, (unsigned long long)pref_base); return -1; }
        base = w32_alloc(w, size_image, 1);
        if (!base) { fprintf(stderr, "%s: cannot allocate %u bytes for the image\n", path, size_image); return -1; }
    }
    uint8_t *img = W32P(w, base);
    memcpy(img, f, size_headers < fsz ? size_headers : fsz);
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = sec + 40 * i;
        uint32_t vsize = RD32(s + 8), vaddr = RD32(s + 12), rawsz = RD32(s + 16), rawoff = RD32(s + 20);
        if ((uint64_t)vaddr + vsize > size_image) { fprintf(stderr, "%s: section %d outside the image\n", path, i); return -1; }
        uint32_t n = rawsz < vsize ? rawsz : vsize;
        if (rawoff + (uint64_t)n <= fsz) memcpy(img + vaddr, f + rawoff, n);
        if (w->verbose) fprintf(stderr, "winrun: section %-8.8s rva %#x vsize %#x raw %#x\n", (const char *)s, vaddr, vsize, rawsz);
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
        if (w->verbose) fprintf(stderr, "winrun: relocated image by %#llx to %#llx\n", (unsigned long long)delta, (unsigned long long)base);
    }

    /* imports */
    int psz = plus ? 8 : 4;
    if (dir[DIR_IMPORT].size) {
        for (uint32_t d = dir[DIR_IMPORT].rva; ; d += 20) {
            uint32_t oft = RD32(img + d), name = RD32(img + d + 12), ft = RD32(img + d + 16);
            if (!name && !ft) break;
            char dll[64]; lower((const char *)img + name, dll, sizeof dll);
            uint32_t src = oft ? oft : ft;           /* ILT if present, else the IAT itself still holds the names */
            for (uint32_t k = 0; ; k++) {
                uint64_t th = plus ? RD64(img + src + 8 * k) : RD32(img + src + 4 * k);
                if (!th) break;
                char fname[128];
                if (th & (plus ? 1ull << 63 : 1ull << 31)) snprintf(fname, sizeof fname, "#%u", (unsigned)(th & 0xFFFF));
                else snprintf(fname, sizeof fname, "%s", (const char *)img + (uint32_t)th + 2);
                uint64_t addr = w32_stub_for(w, dll, fname);
                memcpy(img + ft + psz * k, &addr, psz);
            }
        }
    }

    /* static TLS: one module, index 0 */
    w->tls_index = 0;
    if (dir[DIR_TLS].size) {
        const uint8_t *t = img + dir[DIR_TLS].rva;
        uint64_t raw_start = plus ? RD64(t) : RD32(t), raw_end = plus ? RD64(t + 8) : RD32(t + 4);
        uint64_t idx_addr = plus ? RD64(t + 16) : RD32(t + 8), cb_addr = plus ? RD64(t + 24) : RD32(t + 12);
        uint32_t zero_fill = RD32(t + (plus ? 32 : 16));
        /* the directory holds VAs relative to the *preferred* base */
        raw_start += (uint64_t)delta; raw_end += (uint64_t)delta; idx_addr += (uint64_t)delta; if (cb_addr) cb_addr += (uint64_t)delta;
        uint64_t size = raw_end - raw_start + zero_fill;
        uint64_t block = w32_alloc(w, size + 16, 0);
        if (raw_end > raw_start) memcpy(W32P(w, block), W32P(w, raw_start), raw_end - raw_start);
        w32_write(w, idx_addr, 4, 0);
        /* ThreadLocalStoragePointer -> array whose [0] is our block */
        uint64_t arr = w32_alloc(w, 4096, 0);
        w32_write(w, arr, psz, block);
        w32_write(w, w->teb + (plus ? TEB64_TLSPTR : TEB32_TLSPTR), psz, arr);
        w->tls_slots[0] = block;
        w->tls_callbacks = cb_addr;                  /* run by the runtime once the thread exists */
    }

    w->image_base = base; w->image_size = size_image; w->entry = base + entry_rva;
    if (w->verbose) fprintf(stderr, "winrun: %s: %s, base %#llx, entry %#llx, subsystem %u, stack %#llx\n", path,
                            plus ? "PE32+" : "PE32", (unsigned long long)base, (unsigned long long)w->entry, subsystem,
                            (unsigned long long)stack_reserve);
    munmap(f, fsz);
    return 0;
}
