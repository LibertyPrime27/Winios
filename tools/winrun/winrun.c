/* winrun: run a Windows executable on xcore.
 *
 *   winrun [-v] program.exe [args...]
 *
 * The runtime half of win32/: guest memory (identity mapping for PE32+,
 * a 4 GB arena for PE32 -- the same two models as xrun), the int3 stub
 * mechanism that turns imports into host calls, TEB/PEB, the process heap,
 * handles, and the run loop. The DLL surface itself is in win32/kernel32.c
 * and win32/msvcrt.c. See win32/w32.h for the shape of it all.
 */
#define _GNU_SOURCE
#include "../../win32/w32.h"

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <dirent.h>
#include <unistd.h>

#define PAGE 4096ull
#define PAGE_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

static w32 g_w;
static xc_mem g_mem;
static xc_cpu g_cpu;

enum { MAX_SCRIPT = 256 };
static struct { int frame, kind, a, b; } g_script[MAX_SCRIPT];
static int g_nscript, g_frame;
enum { EV_KEY = 1, EV_CHAR, EV_MOVE, EV_DELTA, EV_BUTTON, EV_WHEEL };

/* winrun's chain link in the present callback, so the input script can watch
 * frames go by without taking them from whoever is drawing. `g_hook_on` says
 * we are the installed callback, so a second run in the same process puts
 * back what was there instead of chaining the hook to itself -- which is an
 * infinite loop, and is what the in-process harness found the first time it
 * ran a scripted guest twice. */
static w32_present_fn g_next_present;
static void *g_next_present_ctx;
static int g_hook_on;

/* Where the run loop resumes when the signal handler turns a host SIGSEGV
 * back into a guest fault. Defined here because both the run loop and the
 * handler need it, and they are far apart. See on_crash. */
static sigjmp_buf g_fault_jmp;
static volatile int g_fault_armed;

/* ------------------------------------------------------------- memory */

void *W32P(w32 *w, uint64_t addr) {
    if (!addr) return 0;
    if (w->is32) return w->base + (uint32_t)addr;
    return (void *)(uintptr_t)addr;
}

static uint64_t g_bump32 = 0x10000000u;        /* arena bump allocator: 256 MB .. 3.5 GB */

/* The host's page size. The guest thinks in 4 KB pages; Apple silicon (macOS
 * and iOS) maps in 16 KB ones, and mmap(MAP_FIXED) / mprotect reject an
 * address that is not a multiple of it. Guest ranges are therefore widened
 * to host pages before they reach the kernel. */
static uint64_t g_hpage;
uint64_t w32_host_page(void) {
    if (!g_hpage) {
        long p = sysconf(_SC_PAGESIZE);
        const char *sim = getenv("WINRUN_HOST_PAGE");         /* test hook: pretend to be a 16 KB host */
        if (sim && atol(sim) > p) p = atol(sim);
        g_hpage = p > (long)PAGE ? (uint64_t)p : PAGE;
    }
    return g_hpage;
}
#define hpage() w32_host_page()
#define HP_DOWN(x) ((x) & ~(hpage() - 1))
#define HP_UP(x)   (((x) + hpage() - 1) & ~(hpage() - 1))

/* Every host mapping made for a 64-bit (identity-mapped) guest, so a second
 * guest in the same process starts with its address space clear. A PE32+ image
 * wants its preferred base (0x140000000) and the stub page wants 0x7FF7...;
 * leaving the first process's mappings there makes the second one relocate --
 * or fail outright when it has no relocations. The 32-bit arena needs no list:
 * it is one reservation, and unmapping it releases everything inside. */
enum { W32_MAX_MAPS = 512 };
static struct { void *p; size_t n; } g_maps[W32_MAX_MAPS];
static int g_nmaps;
static void track_map(void *p, size_t n) {
    if (g_nmaps < W32_MAX_MAPS) { g_maps[g_nmaps].p = p; g_maps[g_nmaps].n = n; g_nmaps++; }
}

/* Guest memory is never executed by the host -- the interpreter reads it and
 * the dynarec translates it -- so it is always mapped RW, whatever the guest
 * asked for. That matters: Apple silicon refuses RWX mappings that are not
 * MAP_JIT, and iOS refuses them outright. `exec` is kept in the signature as
 * documentation of what the guest wanted. */
#define GUEST_PROT (PROT_READ | PROT_WRITE)

uint64_t w32_alloc_at(w32 *w, uint64_t addr, uint64_t size, int exec) {
    (void)exec;
    size = PAGE_UP(size);
    if (w->is32) {
        if (addr + size > 0xF0000000ull || addr < 0x10000) return 0;
        /* the arena is one PROT_NONE reservation; claiming a range is a MAP_FIXED over it.
         * We do not track what the image already claimed -- callers ask for the
         * image first, then everything else comes from the bump allocator above 256 MB.
         * Widen to host pages (a MAP_FIXED replaces what was there, so the bump
         * allocator below hands out host-page-aligned ranges and nothing shares one). */
        uint64_t a0 = HP_DOWN(addr), a1 = HP_UP(addr + size);
        void *p = mmap(w->base + a0, a1 - a0, GUEST_PROT, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { if (w->verbose) fprintf(stderr, "winrun: map %#llx+%#llx: %s\n", (unsigned long long)a0, (unsigned long long)(a1 - a0), strerror(errno)); return 0; }
        if (a1 > g_bump32 && a0 < 0xF0000000ull && a0 >= 0x10000000ull) g_bump32 = a1;
        return addr;
    }
#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap((void *)(uintptr_t)addr, size, GUEST_PROT, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#else
    void *p = mmap((void *)(uintptr_t)addr, size, GUEST_PROT, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   /* hint; verified below */
#endif
    if (p == MAP_FAILED) return 0;
    if ((uint64_t)(uintptr_t)p != addr) { munmap(p, size); return 0; }
    track_map(p, size);
    return addr;
}
/* Guest pages backed by a file.
 *
 * A game opens its archives with MapViewOfFile precisely so the whole thing
 * does not have to be resident, so this maps rather than reads: guest space is
 * reserved the ordinary way and the file is mapped over it. If that cannot be
 * done -- an offset that is not host-page aligned, or a kernel that refuses --
 * the anonymous pages are still there and the caller reads into them instead,
 * which is slower and uses real memory but is never wrong. */
uint64_t w32_map_file(w32 *w, int fd, uint64_t offset, uint64_t size, int writable, int *mapped) {
    if (mapped) *mapped = 0;
    uint64_t a = w32_alloc(w, size, 0);
    if (!a) return 0;
    uint64_t hp = w32_host_page();
    if (fd < 0 || (offset & (hp - 1))) return a;          /* caller fills it */
    void *host = W32P(w, a);
    void *p = mmap(host, (size_t)size, writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   MAP_FIXED | (writable ? MAP_SHARED : MAP_PRIVATE), fd, (off_t)offset);
    if (p == MAP_FAILED) {
        if (w->verbose) fprintf(stderr, "winrun: mmap of %llu bytes at offset %llu: %s\n",
                                (unsigned long long)size, (unsigned long long)offset, strerror(errno));
        return a;                                        /* anonymous pages remain; caller fills */
    }
    if (mapped) *mapped = 1;
    return a;
}

uint64_t w32_alloc(w32 *w, uint64_t size, int exec) {
    size = HP_UP(PAGE_UP(size));
    if (w->is32) {
        uint64_t a = HP_UP(g_bump32);
        if (a + size > 0xF0000000ull) return 0;
        g_bump32 = a + size;
        return w32_alloc_at(w, a, size, exec) ? a : 0;
    }
    void *p = mmap(0, size, GUEST_PROT, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    track_map(p, size);
    return (uint64_t)(uintptr_t)p;
}

/* Heap: a bump allocator over w32_alloc'd chunks with a 16-byte header
 * (size, magic) in front of each block. Freed blocks are recycled through
 * per-size-class free lists kept in the header's `size` word (as a next
 * pointer), which is all a CRT's malloc traffic needs to stay bounded. */
enum { HEAP_MAGIC = 0x48454150 /* HEAP */ };
static uint64_t g_free[48];                    /* free lists by size class (16 << k) */
static int size_class(uint64_t n) { int k = 0; while ((16ull << k) < n) k++; return k; }

uint64_t w32_heap_alloc(w32 *w, uint64_t size) {
    if (size == 0) size = 1;
    int k = size_class(size);
    uint64_t bsz = 16ull << k;
    if (g_free[k]) {
        uint64_t hdr = g_free[k];
        g_free[k] = w32_read(w, hdr, 8);
        w32_write(w, hdr, 8, bsz); w32_write(w, hdr + 8, 8, HEAP_MAGIC);
        memset(W32P(w, hdr + 16), 0, bsz);
        return hdr + 16;
    }
    if (w->heap_cur + 16 + bsz > w->heap_end) {
        uint64_t chunk = bsz + 16 > (1ull << 24) ? PAGE_UP(bsz + 16) : (1ull << 24);
        uint64_t c = w32_alloc(w, chunk, 0);
        if (!c) return 0;
        w->heap_cur = c; w->heap_end = c + chunk;
    }
    uint64_t hdr = w->heap_cur;
    w->heap_cur += 16 + bsz;
    w32_write(w, hdr, 8, bsz); w32_write(w, hdr + 8, 8, HEAP_MAGIC);
    return hdr + 16;
}
uint64_t w32_heap_size(w32 *w, uint64_t addr) {
    if (!addr || w32_read(w, addr - 8, 8) != HEAP_MAGIC) return 0;
    return w32_read(w, addr - 16, 8);
}
void w32_heap_free(w32 *w, uint64_t addr) {
    if (!addr) return;
    uint64_t bsz = w32_heap_size(w, addr);
    if (!bsz) { if (w->verbose) fprintf(stderr, "winrun: free(%#llx): not a heap block\n", (unsigned long long)addr); return; }
    int k = size_class(bsz);
    w32_write(w, addr - 8, 8, 0);                          /* magic cleared: double free is harmless */
    w32_write(w, addr - 16, 8, g_free[k]);
    g_free[k] = addr - 16;
}
uint64_t w32_heap_realloc(w32 *w, uint64_t addr, uint64_t size) {
    if (!addr) return w32_heap_alloc(w, size);
    uint64_t old = w32_heap_size(w, addr);
    if (size <= old) return addr;
    uint64_t n = w32_heap_alloc(w, size);
    if (!n) return 0;
    memcpy(W32P(w, n), W32P(w, addr), old);
    w32_heap_free(w, addr);
    return n;
}
uint64_t w32_strdup(w32 *w, const char *s) {
    size_t n = strlen(s) + 1;
    uint64_t p = w32_heap_alloc(w, n);
    memcpy(W32P(w, p), s, n);
    return p;
}
uint64_t w32_wstrdup(w32 *w, const char *s) {
    size_t n = strlen(s);
    uint64_t p = w32_heap_alloc(w, 2 * (n + 1));
    uint16_t *d = W32P(w, p);
    for (size_t i = 0; i <= n; i++) d[i] = (uint8_t)s[i];
    return p;
}
size_t w32_wcslen(w32 *w, uint64_t p) {
    const uint16_t *s = W32P(w, p); size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}
const char *w32_str(w32 *w, uint64_t addr) { const char *s = W32P(w, addr); return s ? s : ""; }
void w32_wtoa(w32 *w, uint64_t wp, char *out, size_t n) {
    const uint16_t *s = W32P(w, wp); size_t i = 0;
    if (s) for (; s[i] && i + 1 < n; i++) out[i] = s[i] < 128 ? (char)s[i] : '?';
    out[i] = 0;
}
uint64_t w32_read(w32 *w, uint64_t addr, int bytes) {
    uint64_t v = 0; const void *p = W32P(w, addr);
    if (p) memcpy(&v, p, bytes);
    return v;
}
void w32_write(w32 *w, uint64_t addr, int bytes, uint64_t v) {
    void *p = W32P(w, addr);
    if (p) memcpy(p, &v, bytes);
}
uint64_t w32_ptrsize(w32 *w) { return w->is32 ? 4 : 8; }

/* --------------------------------------------------- calling convention */

uint64_t w32_arg(w32 *w, int i) {
    xc_cpu *c = w->c;
    if (w->is32) return w32_read(w, c->gpr[XC_RSP] + 4 + 4u * i, 4);
    switch (i) {
    case 0: return c->gpr[XC_RCX];
    case 1: return c->gpr[XC_RDX];
    case 2: return c->gpr[XC_R8];
    case 3: return c->gpr[XC_R9];
    default: return w32_read(w, c->gpr[XC_RSP] + 8 + 0x20 + 8u * (i - 4), 8);
    }
}
double w32_farg(w32 *w, int i) {
    double d;
    if (w->is32) { uint64_t v = w32_read(w, w->c->gpr[XC_RSP] + 4 + 4u * i, 8); memcpy(&d, &v, 8); return d; }
    if (i < 4) { memcpy(&d, &w->c->xmm[i].lo, 8); return d; }
    uint64_t v = w32_arg(w, i); memcpy(&d, &v, 8); return d;
}
void w32_ret(w32 *w, uint64_t v) { w->c->gpr[XC_RAX] = w->is32 ? (uint32_t)v : v; }
void w32_ret64(w32 *w, uint64_t v) {
    if (w->is32) { w->c->gpr[XC_RAX] = (uint32_t)v; w->c->gpr[XC_RDX] = (uint32_t)(v >> 32); }
    else w->c->gpr[XC_RAX] = v;
}
/* double -> 80-bit extended, for x87 return values (32-bit cdecl math) */
static xc_f80 f80_from_double(double v) {
    uint64_t b; memcpy(&b, &v, 8);
    uint16_t sign = (uint16_t)((b >> 63) << 15);
    int e = (int)((b >> 52) & 0x7FF);
    uint64_t m = b & ((1ull << 52) - 1);
    xc_f80 r;
    if (e == 0x7FF) { r.mant = (1ull << 63) | (m << 11); r.se = (uint16_t)(sign | 0x7FFF); return r; }
    if (e == 0) {
        if (!m) { r.mant = 0; r.se = sign; return r; }
        e = 1; while (!(m & (1ull << 52))) { m <<= 1; e--; }            /* normalise the subnormal */
    }
    r.mant = (m | (1ull << 52)) << 11;
    r.se = (uint16_t)(sign | (uint16_t)(e - 1023 + 16383));
    return r;
}
void w32_fret(w32 *w, double v) {
    if (w->is32) {
        /* x87 ST(0): the interpreter owns the stack; push through the CPU struct */
        xc_cpu *c = w->c;
        int top = (c->fsw >> 11) & 7;
        top = (top - 1) & 7;
        c->fsw = (uint16_t)((c->fsw & ~0x3800) | (top << 11));
        c->fpr[top] = f80_from_double(v);
        c->ftag_empty &= (uint8_t)~(1u << top);
    } else memcpy(&w->c->xmm[0].lo, &v, 8);
}
void w32_set_last_error(w32 *w, uint32_t e) {
    w->last_error = e;
    w32_write(w, w->teb + (w->is32 ? TEB32_LASTERROR : TEB64_LASTERROR), 4, e);
}

/* ------------------------------------------------------------- handles */

uint64_t w32_handle_new(w32 *w, w32_htype t, int fd) {
    for (int i = 1; i < W32_MAX_HANDLES; i++) if (w->handles[i].type == H_NONE) {
        w->handles[i].type = t; w->handles[i].fd = fd; w->handles[i].flags = 0;
        return (uint64_t)i * 4;
    }
    return 0;
}
w32_handle *w32_handle_get(w32 *w, uint64_t h) {
    if (h == W32_STD_INPUT) h = 4; else if (h == W32_STD_OUTPUT) h = 8; else if (h == W32_STD_ERROR) h = 12;
    h = w->is32 ? (uint32_t)h : h;
    if (h == 0 || h % 4 || h / 4 >= W32_MAX_HANDLES) return 0;
    w32_handle *e = &w->handles[h / 4];
    return e->type == H_NONE ? 0 : e;
}
void w32_handle_close(w32 *w, uint64_t h) {
    w32_handle *e = w32_handle_get(w, h);
    if (!e) return;
    if (e->type == H_FILE && e->fd > 2) close(e->fd);
    if (e->type == H_FIND) {
        if (e->p) closedir((DIR *)e->p);
        free((void *)(uintptr_t)e->u1);
        free((void *)(uintptr_t)e->u2);
    }
    /* a mapping does not own its descriptor: the file handle does */
    memset(e, 0, sizeof *e);
}

/* --------------------------------------------------------------- stubs */

static const w32_dll g_dlls[] = {
    /* name, exports, a second table of exports (SEH lives in its own file
     * but belongs to these two DLLs), module handle */
    { "kernel32.dll", w32_kernel32, w32_seh_kernel32, 0 },
    { "msvcrt.dll",   w32_msvcrt,   0,                0 },
    { "ntdll.dll",    w32_ntdll,    w32_seh_ntdll,    0 },
    { "user32.dll",   w32_user32,   0,                0 },
    { "winmm.dll",    w32_winmm,    0,                0 },
    { "d3d9.dll",     w32_d3d9,     0,                0 },
    { "advapi32.dll", w32_advapi32, 0,                0 },
};
enum { NDLLS = sizeof g_dlls / sizeof g_dlls[0], STUB_RETURN = 0, STUB_EXIT = 1, STUB_FIRST = 2 };

static uint64_t stub_addr(w32 *w, int i) { return w->stub_base + 16u * i; }

static int stub_new(w32 *w, const w32_dll *dll, const w32_api *api, const char *missing) {
    if (w->nstubs >= W32_MAX_STUBS) { fprintf(stderr, "winrun: out of stubs\n"); exit(2); }
    int i = w->nstubs++;
    w->stubs[i].dll = dll; w->stubs[i].api = api; w->stubs[i].missing = missing;
    return i;
}

/* One stub bound to a given implementation -- what com.c builds vtables out
 * of. Import stubs go through w32_stub_for, which finds the implementation by
 * name first; a vtable slot already knows which one it wants. */
uint64_t w32_stub_alloc(w32 *w, const w32_dll *dll, const w32_api *api, char *missing) {
    return stub_addr(w, stub_new(w, dll, api, missing));
}

uint64_t w32_module_handle(w32 *w, const char *name) {
    char buf[64]; size_t n = 0;
    for (; name[n] && n + 1 < sizeof buf; n++) buf[n] = (char)((name[n] >= 'A' && name[n] <= 'Z') ? name[n] + 32 : name[n]);
    buf[n] = 0;
    if (!strchr(buf, '.')) strncat(buf, ".dll", sizeof buf - strlen(buf) - 1);
    const char *b = strrchr(buf, '\\'); if (b) memmove(buf, b + 1, strlen(b));
    for (int d = 0; d < NDLLS; d++) if (!strcmp(g_dlls[d].name, buf)) return w->stub_base + 0x10000u * (d + 1);
    /* api-ms-win-* and other forwarders: treat as kernel32 */
    if (!strncmp(buf, "api-ms-win-", 11)) return w->stub_base + 0x10000u;
    return 0;
}

uint64_t w32_stub_for(w32 *w, const char *dll, const char *name) {
    int d = -1;
    for (int k = 0; k < NDLLS; k++) if (!strcmp(g_dlls[k].name, dll)) d = k;
    if (d < 0 && !strncmp(dll, "api-ms-win-crt", 14)) d = 1;       /* UCRT forwarders -> msvcrt */
    if (d < 0 && !strncmp(dll, "api-ms-win", 10)) d = 0;
    if (d >= 0) {
        /* already have a stub? */
        for (int i = STUB_FIRST; i < w->nstubs; i++)
            if (w->stubs[i].dll == &g_dlls[d] && w->stubs[i].api && !strcmp(w->stubs[i].api->name, name))
                return w->stubs[i].api->data_size ? w->data_exports[i] : stub_addr(w, i);
        for (int t = 0; t < 2; t++) {
            const w32_api *tab = t ? g_dlls[d].apis2 : g_dlls[d].apis;
            for (const w32_api *a = tab; a && a->name; a++) if (!strcmp(a->name, name)) {
                int i = stub_new(w, &g_dlls[d], a, 0);
                if (a->data_size) { w->data_exports[i] = w32_heap_alloc(w, (uint64_t)a->data_size); return w->data_exports[i]; }
                return stub_addr(w, i);
            }
        }
    }
    /* unknown: a stub that reports the name when (if) it is called */
    char *m = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(m, "%s!%s", dll, name);
    if (w->verbose) fprintf(stderr, "winrun: unresolved import %s\n", m);
    return stub_addr(w, stub_new(w, d >= 0 ? &g_dlls[d] : 0, 0, m));
}

static void stubs_init(w32 *w) {
    /* one region: 64 KB of stubs, then a page per DLL as its "module" (so
     * module handles are distinct, non-null and inside guest memory) */
    w->stub_base = w->is32 ? w32_alloc_at(w, 0x7F000000u, 0x10000u * (NDLLS + 1), 1)
                           : w32_alloc_at(w, 0x7FF700000000ull, 0x10000u * (NDLLS + 1), 1);
    if (!w->stub_base) w->stub_base = w32_alloc(w, 0x10000u * (NDLLS + 1), 1);
    if (!w->stub_base) { fprintf(stderr, "winrun: cannot map the import stubs: %s\n", strerror(errno)); exit(2); }
    uint8_t *p = W32P(w, w->stub_base);
    for (int i = 0; i < W32_MAX_STUBS; i++) { p[16 * i] = 0xCC; memset(p + 16 * i + 1, 0x90, 15); }
    /* fake module pages: "MZ" so nothing that peeks falls over */
    for (int d = 0; d < NDLLS; d++) { uint8_t *m = W32P(w, w->stub_base + 0x10000u * (d + 1)); m[0] = 'M'; m[1] = 'Z'; }
    stub_new(w, 0, 0, "<return to host>");
    stub_new(w, 0, 0, "<entry returned>");
}

/* ------------------------------------------------------------ run loop */

void w32_exit(w32 *w, int code) { w->exited = 1; w->exit_code = code; w->c->stop = XC_STOP_HLT; }

/* Remember that something we do not implement was called. Names are the
 * stub's own "dll!Name" strings, which outlive the run. */
static void note_unimplemented(w32 *w, const char *name) {
    for (int k = 0; k < w->nunimpl; k++)
        if (w->unimpl[k].name == name) { w->unimpl[k].calls++; return; }
    if (w->nunimpl >= (int)(sizeof w->unimpl / sizeof w->unimpl[0])) { w->unimpl_dropped++; return; }
    w->unimpl[w->nunimpl].name = name;
    w->unimpl[w->nunimpl].calls = 1;
    w->nunimpl++;
}

static void dispatch(w32 *w, int i) {
    xc_cpu *c = w->c;
    const w32_api *a = w->stubs[i].api;
    if (!a) {
        const char *full = w->stubs[i].missing;
        const char *bang = full ? strchr(full, '!') : 0;
        const char *name = bang ? bang + 1 : full;
        note_unimplemented(w, full);

        /* On x86 a stdcall callee pops its own arguments, so returning needs
         * the byte count -- and getting it wrong corrupts the caller's stack
         * far away from here. The generated table has ~9800 of them; a name
         * that is not in it cannot be returned from safely, and saying so is
         * better than guessing. x64 has no callee-pop, so anything can return. */
        int bytes = name ? w32_stdcall_bytes(name) : -1;
        if (!w->keep_going || (w->is32 && bytes < 0)) {
            fprintf(stderr, "winrun: call to unimplemented %s at return address %#llx%s\n",
                    full, (unsigned long long)w32_read(w, c->gpr[XC_RSP], w->is32 ? 4 : 8),
                    w->keep_going && bytes < 0 ? "  (argument count unknown: cannot return from it)" : "");
            w32_exit(w, 127);
            return;
        }
        uint64_t rsp = c->gpr[XC_RSP];
        if (w->is32) {
            c->rip = w32_read(w, rsp, 4);
            c->gpr[XC_RSP] = (uint32_t)(rsp + 4 + (uint32_t)bytes);
        } else {
            c->rip = w32_read(w, rsp, 8);
            c->gpr[XC_RSP] = rsp + 8;
        }
        c->gpr[XC_RAX] = 0;          /* the most common "no" -- see the caveat in WIN32.md */
        return;
    }
    if (w->verbose > 1) fprintf(stderr, "winrun: %s!%s(%#llx, %#llx, %#llx, %#llx)\n",
                                w->stubs[i].dll ? w->stubs[i].dll->name : "?", a->name,
                                (unsigned long long)w32_arg(w, 0), (unsigned long long)w32_arg(w, 1),
                                (unsigned long long)w32_arg(w, 2), (unsigned long long)w32_arg(w, 3));
    uint64_t rsp = c->gpr[XC_RSP];
    a->fn(w);
    if (w->exited) return;
    /* An implementation that set rip and rsp itself -- RaiseException handing
     * control to a handler, NtContinue restoring a context -- has already
     * decided where the guest goes next. Returning for it would undo that. */
    if (w->redirected) { w->redirected = 0; return; }
    /* return to the caller: pop the return address (and the arguments for stdcall on x86) */
    if (w->is32) {
        c->rip = w32_read(w, rsp, 4);
        c->gpr[XC_RSP] = (uint32_t)(rsp + 4 + (a->conv == 0 ? 4u * a->nargs : 0));
    } else {
        c->rip = w32_read(w, rsp, 8);
        c->gpr[XC_RSP] = rsp + 8;
    }
}

/* The app's Stop button, and any other thread that wants a guest to end.
 * Checked between execution slices rather than interrupting one, so there is
 * no question of stopping halfway through an instruction. */
static volatile int g_stop_request;
void w32_request_stop(void) { g_stop_request = 1; }

static uint64_t now_ns_host(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Run until the process exits (depth 0) or the return-to-host stub is hit
 * (depth > 0, i.e. inside w32_call_guest). */
static int run_loop(w32 *w) {
    xc_cpu *c = w->c;
    for (;;) {
        if (w->exited) return 0;
        if (g_stop_request) { w->stop_reason = "stopped by request"; w32_exit(w, 124); return 0; }
        if (w->deadline_ns && now_ns_host() > w->deadline_ns) {
            w->stop_reason = "ran past its time limit";
            w32_exit(w, 124); return 0;
        }
        xc_stop st;
        if (sigsetjmp(g_fault_jmp, 1) == 0) {
            g_fault_armed = 1;
            st = xc_run(c, 1u << 20);
            g_fault_armed = 0;
        } else {
            st = XC_STOP_FAULT;                 /* on_crash recovered a guest access */
        }
        if (st == XC_STOP_STEPS) continue;
        if (st == XC_STOP_BREAKPOINT) {
            uint64_t at = c->rip - 1;
            if (at >= w->stub_base && at < w->stub_base + 16u * W32_MAX_STUBS && !((at - w->stub_base) & 15)) {
                int i = (int)((at - w->stub_base) / 16);
                if (i == STUB_RETURN) { if (w->depth > 0) return 1; w->stop_reason = "a stray return-to-host stub"; fprintf(stderr, "winrun: stray return-to-host\n"); w32_exit(w, 126); return 0; }
                if (i == STUB_EXIT) { w32_exit(w, (int)(uint32_t)c->gpr[XC_RAX]); return 0; }
                if (i < w->nstubs) { dispatch(w, i); continue; }
            }
            fprintf(stderr, "winrun: int3 in guest code at %#llx\n", (unsigned long long)at);
            w32_exit(w, 128 + 5); return 0;
        }
        /* A fault or an illegal instruction is not the end of the run: it is
         * an exception, and the guest may well have a handler for it. Only if
         * nothing takes it does the run stop -- which is what Windows does. */
        /* Deliberately only XC_STOP_FAULT. An undefined instruction is
         * ambiguous here -- it is either the guest's own UD2 or an opcode we
         * have not implemented -- and handing our own gap to the guest's
         * handler would hide it. A stop that names the instruction is worth
         * more than an exception that is probably a lie. */
        if (st == XC_STOP_FAULT) {
            uint64_t at = c->rip, addr = c->fault_addr;
            xc_stop kind = st;
            c->stop = XC_STOP_NONE;
            if (w32_fault_to_exception(w)) continue;
            if (w->exited) return 0;
            uint64_t ea = 0; uint32_t code = w32_last_exception(&ea);
            w->stop_reason = "an unhandled exception";
            char dis[128]; xc_disasm(c, at, dis, sizeof dis);
            fprintf(stderr, "winrun: unhandled %s (%#x) at rip=%#llx  [%s]",
                    w32_exception_name(code), code, (unsigned long long)at, dis);
            if (kind == XC_STOP_FAULT && c->fault_kind == XC_FAULT_MEM)
                fprintf(stderr, "  fault_addr=%#llx", (unsigned long long)addr);
            fprintf(stderr, "\n");
            w32_exit(w, 129); return 0;
        }
        w->stop_reason = xc_stop_name(st);
        char dis[128]; xc_disasm(c, c->rip, dis, sizeof dis);
        fprintf(stderr, "winrun: stopped: %s at rip=%#llx  [%s]", xc_stop_name(st), (unsigned long long)c->rip, dis);
        fprintf(stderr, "\n");
        w32_exit(w, 125); return 0;
    }
}

uint64_t w32_call_guest(w32 *w, uint64_t fn, int nargs, const uint64_t *args) {
    xc_cpu *c = w->c;
    uint64_t saved_gpr[16]; memcpy(saved_gpr, c->gpr, sizeof saved_gpr);
    uint64_t saved_rip = c->rip;
    uint64_t sp = c->gpr[XC_RSP];
    if (w->is32) {
        sp = (sp - 4u * nargs - 4) & ~15ull;
        for (int i = 0; i < nargs; i++) w32_write(w, sp + 4 + 4u * i, 4, args[i]);
        w32_write(w, sp, 4, stub_addr(w, STUB_RETURN));
    } else {
        int stackargs = nargs > 4 ? nargs - 4 : 0;
        sp = (sp - 0x20 - 8u * stackargs - 64) & ~15ull;      /* shadow space + stack args, 16-aligned before the call */
        for (int i = 4; i < nargs; i++) w32_write(w, sp + 0x20 + 8u * (i - 4), 8, args[i]);
        sp -= 8;                                               /* the "call" pushes the return address */
        w32_write(w, sp, 8, stub_addr(w, STUB_RETURN));
        if (nargs > 0) c->gpr[XC_RCX] = args[0];
        if (nargs > 1) c->gpr[XC_RDX] = args[1];
        if (nargs > 2) c->gpr[XC_R8] = args[2];
        if (nargs > 3) c->gpr[XC_R9] = args[3];
    }
    c->gpr[XC_RSP] = sp;
    c->rip = fn;
    w->depth++;
    run_loop(w);
    w->depth--;
    uint64_t ret = c->gpr[XC_RAX];
    if (!w->exited) { memcpy(c->gpr, saved_gpr, sizeof saved_gpr); c->rip = saved_rip; c->gpr[XC_RAX] = ret; }
    return ret;
}

int w32_run(w32 *w) {
    run_loop(w);
    return w->exit_code;
}

/* --------------------------------------------------------- process setup */

static void process_init(w32 *w, int argc, char **argv) {
    int psz = w->is32 ? 4 : 8;
    /* standard handles first, so they are 4, 8 and 12 */
    w32_handle_new(w, H_FILE, 0); w32_handle_new(w, H_FILE, 1); w32_handle_new(w, H_FILE, 2);
    /* TEB + PEB: two pages each, zeroed */
    w->teb = w32_alloc(w, 0x2000, 0);
    w->peb = w32_alloc(w, 0x1000, 0);
    /* stack: 1 MB (mingw CRT probes the TEB stack limits) */
    uint64_t stack_size = 1u << 20;
    w->stack_limit = w32_alloc(w, stack_size, 0);
    w->stack_base = w->stack_limit + stack_size;
    if (!w->teb || !w->peb || !w->stack_limit) { fprintf(stderr, "winrun: cannot map TEB/PEB/stack: %s\n", strerror(errno)); exit(2); }
    if (w->is32) {
        w32_write(w, w->teb + 0x00, 4, 0xFFFFFFFFu);          /* ExceptionList: end of chain */
        w32_write(w, w->teb + 0x04, 4, w->stack_base);
        w32_write(w, w->teb + 0x08, 4, w->stack_limit);
        w32_write(w, w->teb + 0x18, 4, w->teb);                /* Self */
        w32_write(w, w->teb + 0x20, 4, 4242);                  /* pid */
        w32_write(w, w->teb + 0x24, 4, 4243);                  /* tid */
        w32_write(w, w->teb + TEB32_PEB, 4, w->peb);
        w32_write(w, w->peb + 0x18, 4, w32_handle_new(w, H_HEAP, -1));
        w32_write(w, w->peb + 0x64, 4, 4);                     /* NumberOfProcessors */
        w32_write(w, w->peb + 0xA4, 4, 10); w32_write(w, w->peb + 0xA8, 4, 0); w32_write(w, w->peb + 0xAC, 2, 19045);
    } else {
        w32_write(w, w->teb + 0x08, 8, w->stack_base);
        w32_write(w, w->teb + 0x10, 8, w->stack_limit);
        w32_write(w, w->teb + 0x30, 8, w->teb);                /* Self */
        w32_write(w, w->teb + 0x40, 8, 4242);                  /* ClientId.UniqueProcess */
        w32_write(w, w->teb + 0x48, 8, 4243);                  /* ClientId.UniqueThread */
        w32_write(w, w->teb + TEB64_PEB, 8, w->peb);
        w32_write(w, w->peb + 0x30, 8, w32_handle_new(w, H_HEAP, -1));
        w32_write(w, w->peb + 0xB8, 4, 4);                     /* NumberOfProcessors */
        w32_write(w, w->peb + 0x118, 4, 10); w32_write(w, w->peb + 0x11C, 4, 0); w32_write(w, w->peb + 0x120, 2, 19045);
    }
    /* command line: "C:\path\prog.exe" args..., Windows style */
    char cmd[4096] = ""; size_t n = 0;
    const char *slash = strrchr(argv[0], '/'); const char *exe = slash ? slash + 1 : argv[0];
    n += (size_t)snprintf(cmd + n, sizeof cmd - n, "\"C:\\xcore\\%s\"", exe);
    for (int i = 1; i < argc && n < sizeof cmd - 8; i++) {
        int q = strchr(argv[i], ' ') != 0;
        n += (size_t)snprintf(cmd + n, sizeof cmd - n, " %s%s%s", q ? "\"" : "", argv[i], q ? "\"" : "");
    }
    w->cmdline = w32_strdup(w, cmd);
    w->cmdline_w = w32_wstrdup(w, cmd);
    /* environment block: "VAR=value\0...\0\0" */
    const char *env = "PATH=C:\\Windows\\System32\0SYSTEMROOT=C:\\Windows\0TEMP=C:\\Temp\0TMP=C:\\Temp\0USERPROFILE=C:\\Users\\xcore\0HOMEDRIVE=C:\0HOMEPATH=\\Users\\xcore\0";
    size_t envlen = 0; while (env[envlen] || env[envlen + 1]) envlen++; envlen += 2;
    w->env_block = w32_heap_alloc(w, envlen); memcpy(W32P(w, w->env_block), env, envlen);
    w->env_block_w = w32_heap_alloc(w, 2 * envlen);
    { uint16_t *d = W32P(w, w->env_block_w); for (size_t i = 0; i < envlen; i++) d[i] = (uint8_t)env[i]; }
    w->argc = argc; w->argv = argv;
    (void)psz;
}

/* A host crash (SIGSEGV/SIGBUS) is almost always a guest access to memory
 * the arena/identity model does not cover, or a JIT bug. Say where we were
 * so a CI log is enough to start from, then die with the original signal. */
#include <signal.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif
/* Where the host was when the signal arrived. Needed to tell a guest access
 * to an unmapped guest page -- which the interpreter can turn into a guest
 * exception -- from the same access made by compiled code, where the guest's
 * registers are in host registers and no honest CONTEXT can be built. Returns
 * 0 where the layout is unknown, and 0 means "assume the worst". */
static uint64_t *host_pc_slot(void *uctx) {
#if defined(__APPLE__) && defined(__aarch64__)
    return (uint64_t *)&((ucontext_t *)uctx)->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__aarch64__)
    return (uint64_t *)&((ucontext_t *)uctx)->uc_mcontext.pc;
#else
    (void)uctx; return 0;                       /* only ARM64 has compiled code */
#endif
}

static uint64_t host_pc_of(void *uctx) {
#if defined(__APPLE__) && defined(__aarch64__)
    return (uint64_t)((ucontext_t *)uctx)->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uint64_t)((ucontext_t *)uctx)->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
    return (uint64_t)((ucontext_t *)uctx)->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
    return (uint64_t)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uctx; return 0;
#endif
}

/* A 32-bit guest's arena is one 4 GB PROT_NONE reservation with the pages it
 * has actually claimed mapped over it, so a guest dereference of a null or
 * stray pointer is a *host* SIGSEGV rather than something the memory model
 * reports. Windows would raise an access violation there and the guest may
 * have a handler for it, so the interpreter's faults come back here.
 *
 * Longjmp out of a signal handler is safe in exactly this case: the signal is
 * synchronous, we land back in the run loop on the same thread, and the
 * interpreter holds no allocation or lock across an instruction. It is only
 * done when the fault address is inside the guest's own arena -- a fault
 * anywhere else is a bug in this runtime, and recovering from it would hide
 * the bug -- and only when the faulting code was not a compiled block, whose
 * register state cannot be reconstructed yet. */
static void on_crash(int sig, siginfo_t *si, void *uctx) {
    xc_cpu *c = g_w.c;
    uint64_t lo = 0, hi = 0; int have = xc_jit_code_range(&lo, &hi);
    uint64_t fault = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = host_pc_of(uctx);
    int in_arena = g_w.is32 && g_w.base &&
                   fault >= (uint64_t)(uintptr_t)g_w.base &&
                   fault < (uint64_t)(uintptr_t)g_w.base + (1ull << 32);
    int in_jit = have && pc >= lo && pc < hi;
    /* A guest address: what the guest was reaching for, if this was a guest
     * access at all. The arena makes that unambiguous for a 32-bit guest; a
     * 64-bit guest is identity-mapped, so the address is already the guest's. */
    uint64_t gaddr = g_w.is32 ? fault - (uint64_t)(uintptr_t)g_w.base : fault;

    /* Inside a compiled block the guest's registers are in host registers, so
     * nothing can leave from here -- the compiled code has to write them back
     * itself, and it carries a recovery stub for exactly this instruction.
     * Move the PC there and return: the stub spills, sets the guest RIP and
     * exits through the dispatcher, and xc_run returns XC_STOP_FAULT.
     *
     * The exact-PC match is also what makes this safe to do at all. It is not
     * a guess about where the fault came from: the PC either is one of the
     * ldr/str instructions the compiler emitted for a guest access, or it is
     * not, and a fault anywhere else in the runtime stays a crash. That is a
     * stronger test than "the address looks like guest memory", so it needs
     * no help from the arena and works for both bitnesses. */
    if (g_fault_armed && in_jit && pc && c) {
        uint64_t grip = 0;
        void *stub = xc_jit_fault_stub(pc, &grip);
        uint64_t *pcp = host_pc_slot(uctx);
        if (stub && pcp) {
            c->stop = XC_STOP_FAULT;
            c->fault_kind = XC_FAULT_MEM;
            c->fault_addr = gaddr;
            c->rip = grip;                      /* the stub only spills; the RIP is ours to set */
            *pcp = (uint64_t)(uintptr_t)stub;
            return;
        }
    }
    /* The interpreter's own accesses. Here the cpu struct is already the
     * truth at an instruction boundary, so longjmp out to the run loop. Only
     * for a 32-bit guest, whose arena makes "this was the guest reaching for
     * memory it does not have" a fact rather than a hope -- a fault outside
     * it is a bug in this runtime, and recovering from one would hide it. */
    if (g_fault_armed && in_arena && !in_jit && pc && c) {
        c->stop = XC_STOP_FAULT;
        c->fault_kind = XC_FAULT_MEM;
        c->fault_addr = gaddr;
        g_fault_armed = 0;
        siglongjmp(g_fault_jmp, 1);
    }
    char buf[768];
    int n = snprintf(buf, sizeof buf,
        "winrun: host %s at address %#llx; guest rip=%#llx rsp=%#llx (%d-bit, %s); host pc %s a compiled block%s\n"
        "winrun: state: image %#llx+%#x entry %#llx stubs %#llx teb %#llx peb %#llx stack %#llx..%#llx heap %#llx depth %d exited %d\n",
        sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", (unsigned long long)fault,
        (unsigned long long)(c ? c->rip : 0), (unsigned long long)(c ? c->gpr[XC_RSP] : 0), g_w.is32 ? 32 : 64,
        xc_jit_enabled() ? "jit" : "interpreter",
        in_jit ? "inside" : "outside",
        in_jit ? " -- in compiled code, with no recovery stub for this instruction"
                 " (see core/src/jit/jit.c)"
               : in_arena ? " -- inside the 4 GB arena (unmapped guest page)" : "",
        (unsigned long long)g_w.image_base, (unsigned)g_w.image_size, (unsigned long long)g_w.entry,
        (unsigned long long)g_w.stub_base, (unsigned long long)g_w.teb, (unsigned long long)g_w.peb,
        (unsigned long long)g_w.stack_limit, (unsigned long long)g_w.stack_base, (unsigned long long)g_w.heap_cur,
        g_w.depth, g_w.exited);
    if (write(2, buf, (size_t)(n > 0 ? n : 0)) < 0) { }
#ifdef HAVE_BACKTRACE
    /* where the host was: the frames name the runtime function (or the core
     * primitive) that touched the bad address -- async-signal-unsafe in
     * theory, good enough for a last message in practice */
    void *frames[32]; int nf = backtrace(frames, 32);
    if (write(2, "winrun: host backtrace:\n", 24) < 0) { }
    backtrace_symbols_fd(frames, nf, 2);
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Reset every global this file owns, so winrun_main can be called more than
 * once in a process -- which is exactly what the device build does when the
 * button is tapped again. The block cache must go too: a second executable
 * maps its own code at the same guest addresses (0x400000 for a PE32), and a
 * stale compiled block there would run the previous program's instructions. */
static void winrun_reset(void) {
    if (g_w.is32 && g_w.base) munmap(g_w.base, 1ull << 32);
    for (int i = 0; i < g_nmaps; i++) munmap(g_maps[i].p, g_maps[i].n);
    g_nmaps = 0;
    memset(&g_w, 0, sizeof g_w);
    memset(&g_mem, 0, sizeof g_mem);
    memset(&g_cpu, 0, sizeof g_cpu);
    memset(g_free, 0, sizeof g_free);
    g_bump32 = 0x10000000u;
    g_stop_request = 0;
    w32_reset_statics();
    w32_registry_reset();
    w32_seh_reset();
    w32_input_reset();
    g_nscript = 0; g_frame = 0;
    if (g_hook_on) { w32_set_present(g_next_present, g_next_present_ctx); g_hook_on = 0; }
    g_next_present = 0; g_next_present_ctx = 0;
    w32_com_reset();
    w32_d3d9_reset();
    xc_cache_flush();
}

/* -input <file>: a script of keyboard and mouse events, delivered to the
 * guest at chosen frames.
 *
 * Input needs a driver. On the device it is a finger or a keyboard; here it
 * has to come from somewhere deterministic, or a recorded expectation is a
 * recording of the tester's reflexes. Each line is
 *
 *     [frame N] <event>
 *
 * with the events being `key down|up <vk>`, `char <n>`, `mouse move <x> <y>`,
 * `mouse delta <dx> <dy>`, `mouse down|up left|right|middle` and
 * `mouse wheel <delta>`; `#` starts a comment. Lines with no frame given are
 * delivered before the guest starts. A frame is a Present, so "frame 3" means
 * "just before the guest draws its fourth frame" -- the same moment a real
 * event would land in the middle of a frame loop.
 *
 * It is also how to reproduce an input bug: the script *is* the repro.
 */
static void script_fire(int frame) {
    for (int i = 0; i < g_nscript; i++) {
        if (g_script[i].frame != frame) continue;
        switch (g_script[i].kind) {
        case EV_KEY:    w32_input_key(g_script[i].a, g_script[i].b); break;
        case EV_CHAR:   w32_input_char((uint32_t)g_script[i].a); break;
        case EV_MOVE:   w32_input_mouse_move(g_script[i].a, g_script[i].b); break;
        case EV_DELTA:  w32_input_mouse_delta(g_script[i].a, g_script[i].b); break;
        case EV_BUTTON: w32_input_mouse_button(g_script[i].a, g_script[i].b); break;
        case EV_WHEEL:  w32_input_mouse_wheel(g_script[i].a); break;
        default: break;
        }
    }
}

static int script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') continue;
        int frame = 0, n = 0;
        if (!strncmp(p, "frame ", 6) && sscanf(p + 6, "%d%n", &frame, &n) == 1) {
            p += 6 + n;
            while (*p == ' ' || *p == ':' || *p == '\t') p++;
        }
        if (g_nscript >= MAX_SCRIPT) { fprintf(stderr, "winrun: -input: more than %d events\n", MAX_SCRIPT); break; }
        int kind = 0, a = 0, b = 0;
        char w1[16] = {0}, w2[16] = {0};
        if (sscanf(p, "key %15s %d", w1, &a) == 2) { kind = EV_KEY; b = !strcmp(w1, "down"); }
        else if (sscanf(p, "char %d", &a) == 1) kind = EV_CHAR;
        else if (sscanf(p, "mouse move %d %d", &a, &b) == 2) kind = EV_MOVE;
        else if (sscanf(p, "mouse delta %d %d", &a, &b) == 2) kind = EV_DELTA;
        else if (sscanf(p, "mouse wheel %d", &a) == 1) kind = EV_WHEEL;
        else if (sscanf(p, "mouse %15s %15s", w1, w2) == 2 &&
                 (!strcmp(w1, "down") || !strcmp(w1, "up"))) {
            kind = EV_BUTTON;
            b = !strcmp(w1, "down");
            a = !strcmp(w2, "right") ? 1 : !strcmp(w2, "middle") ? 2 : 0;
        }
        if (!kind) { fprintf(stderr, "winrun: -input: %s:%d: cannot read \"%s\"\n", path, lineno, p); fclose(f); return -1; }
        g_script[g_nscript].frame = frame;
        g_script[g_nscript].kind = kind;
        g_script[g_nscript].a = a; g_script[g_nscript].b = b;
        g_nscript++;
    }
    fclose(f);
    return 0;
}

/* Every presented frame passes through here so the script can be fired
 * between frames, then on to whatever else wanted the pixels. */
static void present_hook(void *ctx, const void *px, int w, int h, int pitch) {
    (void)ctx;
    g_frame++;
    script_fire(g_frame);
    if (g_next_present) g_next_present(g_next_present_ctx, px, w, h, pitch);
}

/* WINRUN_PRESENT_PPM=<prefix>: write every presented frame as <prefix>NNN.ppm.
 * The only way to look at what a guest drew when there is no screen -- CI, a
 * headless run, or checking a change by eye. */
static void present_ppm(void *ctx, const void *pixels, int width, int height, int pitch) {
    static int n;
    char path[512];
    snprintf(path, sizeof path, "%s%03d.ppm", (const char *)ctx, n++);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = (const uint8_t *)pixels + (size_t)y * pitch;
        for (int x = 0; x < width; x++) {          /* X8R8G8B8 in memory is B,G,R,X */
            uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "winrun: presented frame -> %s (%dx%d)\n", path, width, height);
}

/* What does this program need that we do not have?
 *
 * Every import that no host implementation and no guest DLL could satisfy
 * became a stub carrying its own name (w32_stub_for). Loading an executable
 * therefore produces the exact list of what is missing, before running a
 * single instruction -- which turns "what should we build next" from a guess
 * into a list taken from the binary itself. `winrun -imports game.exe` on a
 * real game is the roadmap.
 *
 * Grouped by DLL, because that is the shape the work has: a DLL we have none
 * of is a decision (write it, or stub it out), a DLL we have most of is an
 * afternoon.
 */
static int cmp_missing(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void report_imports(w32 *w) {
    printf("%s: %d-bit\n", w->exe_path, w->is32 ? 32 : 64);
    printf("\nmodules loaded:\n");
    for (int i = 0; i < w->nmods; i++)
        printf("  %-28s %s\n", w->mods[i].name,
               w->mods[i].is_exe ? "(the executable)" : "guest DLL, loaded and resolved");

    const char *miss[W32_MAX_STUBS];
    int n = 0, resolved = 0;
    for (int i = 0; i < w->nstubs; i++) {
        if (w->stubs[i].api) resolved++;
        else if (w->stubs[i].missing && strchr(w->stubs[i].missing, '!')) miss[n++] = w->stubs[i].missing;
    }
    qsort(miss, (size_t)n, sizeof miss[0], cmp_missing);

    printf("\n%d imports resolved, %d missing\n", resolved, n);
    if (!n) { printf("\nnothing is missing: this program can be run.\n"); return; }

    printf("\nmissing, by DLL:\n");
    int i = 0;
    while (i < n) {
        const char *bang = strchr(miss[i], '!');
        size_t dlen = (size_t)(bang - miss[i]);
        int j = i;
        while (j < n && !strncmp(miss[j], miss[i], dlen) && miss[j][dlen] == '!') j++;
        printf("\n  %.*s  (%d missing)\n", (int)dlen, miss[i], j - i);
        for (int k = i; k < j; k++) printf("      %s\n", miss[k] + dlen + 1);
        i = j;
    }
}

/* Everything worth knowing about where a run ended.
 *
 * Written for a clean exit as well as a crash, because "exited 0 having called
 * nine functions that returned nothing" is also a diagnosis, and because the
 * person reading this is usually not the person who ran it -- a report that
 * only appears on a crash is a report you cannot ask for.
 */
int w32_crash_report(w32 *w, char *out, size_t out_len) {
    xc_cpu *c = w->c;
    size_t n = 0;
    #define P(...) do { if (n < out_len) n += (size_t)snprintf(out + n, out_len - n, __VA_ARGS__); } while (0)

    P("winios run report\n");
    P("  program   %s (%d-bit)\n", w->exe_path ? w->exe_path : "?", w->is32 ? 32 : 64);
    P("  ended     %s\n", w->stop_reason ? w->stop_reason : "normally");
    P("  exit code %d\n", w->exit_code);

    P("\n  rip %016llx  rsp %016llx  rbp %016llx\n",
      (unsigned long long)c->rip, (unsigned long long)c->gpr[XC_RSP], (unsigned long long)c->gpr[XC_RBP]);
    static const char *rn[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                  "r8","r9","r10","r11","r12","r13","r14","r15" };
    for (int i = 0; i < (w->is32 ? 8 : 16); i++)
        P("  %-4s%016llx%s", rn[i], (unsigned long long)c->gpr[i], (i % 4) == 3 ? "\n" : "");
    if ((w->is32 ? 8 : 16) % 4) P("\n");

    /* Forward from RIP only: disassembling backwards on x86 is guesswork, and
     * a report that guesses is worse than one that says less. */
    P("\n  at rip:\n");
    uint64_t at = c->rip;
    for (int i = 0; i < 6; i++) {
        char dis[160];
        int len = xc_disasm(c, at, dis, sizeof dis);
        if (len <= 0) { P("    %016llx  (cannot decode)\n", (unsigned long long)at); break; }
        P("    %016llx  %s\n", (unsigned long long)at, dis);
        at += (uint64_t)len;
    }

    P("\n  modules:\n");
    for (int i = 0; i < w->nmods; i++)
        P("    %-24s %012llx..%012llx%s\n", w->mods[i].name,
          (unsigned long long)w->mods[i].base,
          (unsigned long long)(w->mods[i].base + w->mods[i].size),
          w->mods[i].is_exe ? "  (exe)" : "");
    P("    %-24s %012llx\n", "[import stubs]", (unsigned long long)w->stub_base);
    P("    %-24s %012llx..%012llx\n", "[stack]",
      (unsigned long long)w->stack_limit, (unsigned long long)w->stack_base);
    P("    %-24s %012llx\n", "[teb]", (unsigned long long)w->teb);

    /* Which module the fault is in is usually the whole answer. */
    for (int i = 0; i < w->nmods; i++)
        if (c->rip >= w->mods[i].base && c->rip < w->mods[i].base + w->mods[i].size)
            P("\n  rip is in %s, at +%#llx\n", w->mods[i].name,
              (unsigned long long)(c->rip - w->mods[i].base));

    if (w->nunimpl) {
        P("\n  called but not implemented (%d):\n", w->nunimpl);
        for (int k = 0; k < w->nunimpl; k++)
            P("    %6u x  %s\n", w->unimpl[k].calls, w->unimpl[k].name);
        if (w->unimpl_dropped) P("    (and %u more distinct)\n", w->unimpl_dropped);
    }
    #undef P
    return (int)n;
}

int winrun_main(int argc, char **argv) {
    winrun_reset();
    w32 *w = &g_w;
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO;
      sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); }
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-v")) w->verbose++;
        else if (!strcmp(argv[ai], "-vv")) w->verbose += 2;
        else if (!strcmp(argv[ai], "-imports")) w->imports_only = 1;
        else if (!strcmp(argv[ai], "-input") && ai + 1 < argc) { if (script_load(argv[++ai])) return 2; }
        else if (!strcmp(argv[ai], "-k")) w->keep_going = 1;
        else if (!strcmp(argv[ai], "-t") && ai + 1 < argc) { w->deadline_ns = now_ns_host() + (uint64_t)atoll(argv[ai + 1]) * 1000000000ull; ai++; }
        else if (!strcmp(argv[ai], "-C") && ai + 1 < argc) { w32_set_drive_c(argv[ai + 1]); ai++; }
        else if (!strcmp(argv[ai], "-L") && ai + 1 < argc) {       /* extra directory to find guest DLLs in */
            static char dir[512];
            snprintf(dir, sizeof dir, "%s%s", argv[ai + 1], argv[ai + 1][strlen(argv[ai + 1]) - 1] == '/' ? "" : "/");
            w->dll_dir = dir; ai++;
        }
        else { fprintf(stderr, "usage: winrun [-v] [-imports] [-k] [-t seconds] [-C drive_c] [-L dlldir] program.exe [args...]\n"); return 2; }
        ai++;
    }
    if (ai >= argc) { fprintf(stderr, "usage: winrun [-v] [-imports] [-k] [-t seconds] [-C drive_c] [-L dlldir] program.exe [args...]\n"); return 2; }
    w->exe_path = argv[ai];

    /* bitness decides the memory model, so peek at the header first */
    { FILE *f = fopen(argv[ai], "rb"); uint8_t h[0x200] = {0};
      if (!f) { perror(argv[ai]); return 2; }
      size_t got = fread(h, 1, sizeof h, f); fclose(f);
      if (got < 0x40 || h[0] != 'M' || h[1] != 'Z') { fprintf(stderr, "%s: not a Windows executable\n", argv[ai]); return 2; }
      uint32_t pe = h[0x3C] | h[0x3D] << 8 | h[0x3E] << 16 | (uint32_t)h[0x3F] << 24;
      if (pe + 26 > got) { fprintf(stderr, "%s: PE header out of reach\n", argv[ai]); return 2; }
      uint16_t magic = (uint16_t)(h[pe + 24] | h[pe + 25] << 8);
      w->is32 = magic == 0x10B; }

    w->c = &g_cpu; w->mem = &g_mem;
    if (w->is32) {
        void *base = mmap(0, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (base == MAP_FAILED) { perror("reserve 4 GB arena"); return 2; }
        w->base = base;
        xc_mem_init_arena(w->mem, base, 1ull << 32);
        xc_cpu_init(w->c, XC_MODE_32, w->mem);
    } else {
        xc_mem_init_identity(w->mem);
        xc_cpu_init(w->c, XC_MODE_64, w->mem);
    }
    { const char *c = getenv("WINRUN_DRIVE_C"); if (c && *c) w32_set_drive_c(c); }
    stubs_init(w);
    { const char *ppm = getenv("WINRUN_PRESENT_PPM"); if (ppm) w32_set_present(present_ppm, (void *)ppm); }
    if (g_nscript) {
        /* chain: the script fires first, then whatever wanted the frame */
        g_next_present = w32_get_present(&g_next_present_ctx);
        if (g_next_present == present_hook) { g_next_present = 0; g_next_present_ctx = 0; }
        w32_set_present(present_hook, 0);
        g_hook_on = 1;
        script_fire(0);                    /* events with no frame: before the first instruction */
    }
    /* TEB/PEB/stack/heap first (TLS callbacks need them); they come from the
     * arena's bump allocator or host mmap, neither of which lands on a PE32+
     * preferred base (0x140000000) or a PE32 one (0x400000) */
    process_init(w, argc - ai, argv + ai);
    if (w32_load_pe(w, argv[ai])) return 2;
    if (w->imports_only) { report_imports(w); return 0; }
    w32_write(w, w->peb + (w->is32 ? 0x08 : 0x10), w->is32 ? 4 : 8, w->image_base);   /* PEB.ImageBaseAddress */
    /* the initial thread: TEB in gs (x64) / fs (x86), stack, entry point */
    xc_cpu *c = w->c;
    if (w->is32) {
        c->fs_base = w->teb;
        c->sreg[1] = 0x1b; c->sreg[0] = c->sreg[2] = c->sreg[3] = 0x23; c->sreg[4] = 0x3b;   /* what a 32-bit Windows process sees */
        uint32_t sp = (uint32_t)(w->stack_base - 0x100);
        sp -= 4; w32_write(w, sp, 4, stub_addr(w, STUB_EXIT));   /* entry returns -> exit with eax */
        c->gpr[XC_RSP] = sp;
    } else {
        c->gs_base = w->teb;
        uint64_t sp = (w->stack_base - 0x100) & ~15ull;
        sp -= 8; w32_write(w, sp, 8, stub_addr(w, STUB_EXIT));
        c->gpr[XC_RSP] = sp;
        c->gpr[XC_RCX] = w->peb;                             /* what BaseThreadInitThunk passes */
    }
    c->rip = w->entry;
    /* a Windows process starts with the FPU in 53-bit precision (FCW 0x027F),
     * not the 8087 default the core's FNINIT sets; MSVC-built code relies on it */
    c->fcw = 0x027F;
    /* Statically imported DLLs are initialised before the executable's own
     * TLS callbacks and entry point, in the order they were loaded (which is
     * dependency order) -- this is what the loader in ntdll does, and code in
     * a DllMain relies on its own dependencies already being attached. */
    w32_attach_modules(w);
    c->rip = w->entry;
    /* TLS callbacks (DLL_PROCESS_ATTACH) before the entry point, as the loader does */
    if (w->tls_callbacks) {
        int psz = w->is32 ? 4 : 8;
        for (uint32_t k = 0; !w->exited; k++) {
            uint64_t cb = w32_read(w, w->tls_callbacks + (uint64_t)psz * k, psz);
            if (!cb) break;
            uint64_t args[3] = { w->image_base, 1, 0 };
            if (w->verbose) fprintf(stderr, "winrun: TLS callback %#llx\n", (unsigned long long)cb);
            w32_call_guest(w, cb, 3, args);
        }
        c->rip = w->entry;
    }
    if (w->verbose) {
        for (int i = 0; i < w->nmods; i++)
            fprintf(stderr, "winrun: module %-20s base %#llx size %#llx%s%s\n", w->mods[i].name,
                    (unsigned long long)w->mods[i].base, (unsigned long long)w->mods[i].size,
                    w->mods[i].is_exe ? "  (exe)" : "", w->mods[i].exp_size ? "  exports" : "");
    }
    if (w->verbose) fprintf(stderr, "winrun: %d-bit, entry %#llx, rsp %#llx, teb %#llx, peb %#llx\n", w->is32 ? 32 : 64,
                            (unsigned long long)c->rip, (unsigned long long)c->gpr[XC_RSP], (unsigned long long)w->teb, (unsigned long long)w->peb);
    int code = w32_run(w);
    fflush(stdout);
    /* A report whenever there is something to report: an abnormal end, or a
     * clean one that leaned on functions we do not have. */
    if (w->stop_reason || w->nunimpl) {
        static char rep[16384];
        w32_crash_report(w, rep, sizeof rep);
        printf("\n%s", rep);
        fflush(stdout);
    }
    if (w->verbose) {
        uint64_t jb, jco, jbytes, jl, jlw, jls; xc_jit_stats(&jb, &jco, &jbytes);
        xc_jit_link_stats(&jl, &jlw, &jls);
        fprintf(stderr, "winrun: exit %d; jit: %s, %llu blocks (%llu KB), %llu callouts, %llu links (%llu warm, %llu topped up)\n", code,
                xc_jit_enabled() ? "on" : "off", (unsigned long long)jb, (unsigned long long)(jbytes >> 10),
                (unsigned long long)jco, (unsigned long long)jl, (unsigned long long)jlw, (unsigned long long)jls);
    }
    return code;
}

#ifndef WINRUN_NO_MAIN
int main(int argc, char **argv) { return winrun_main(argc, argv); }
#endif
