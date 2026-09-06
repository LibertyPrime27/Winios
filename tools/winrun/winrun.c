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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096ull
#define PAGE_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

static w32 g_w;
static xc_mem g_mem;
static xc_cpu g_cpu;

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
    e->type = H_NONE;
}

/* --------------------------------------------------------------- stubs */

static const w32_dll g_dlls[] = {
    { "kernel32.dll", w32_kernel32, 0 },
    { "msvcrt.dll",   w32_msvcrt,   0 },
    { "ntdll.dll",    w32_ntdll,    0 },
    { "user32.dll",   w32_user32,   0 },
};
enum { NDLLS = sizeof g_dlls / sizeof g_dlls[0], STUB_RETURN = 0, STUB_EXIT = 1, STUB_FIRST = 2 };

static uint64_t stub_addr(w32 *w, int i) { return w->stub_base + 16u * i; }

static int stub_new(w32 *w, const w32_dll *dll, const w32_api *api, const char *missing) {
    if (w->nstubs >= W32_MAX_STUBS) { fprintf(stderr, "winrun: out of stubs\n"); exit(2); }
    int i = w->nstubs++;
    w->stubs[i].dll = dll; w->stubs[i].api = api; w->stubs[i].missing = missing;
    return i;
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
        for (const w32_api *a = g_dlls[d].apis; a->name; a++) if (!strcmp(a->name, name)) {
            int i = stub_new(w, &g_dlls[d], a, 0);
            if (a->data_size) { w->data_exports[i] = w32_heap_alloc(w, (uint64_t)a->data_size); return w->data_exports[i]; }
            return stub_addr(w, i);
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

static void dispatch(w32 *w, int i) {
    xc_cpu *c = w->c;
    const w32_api *a = w->stubs[i].api;
    if (!a) {
        fprintf(stderr, "winrun: call to unimplemented %s at return address %#llx\n",
                w->stubs[i].missing, (unsigned long long)w32_read(w, c->gpr[XC_RSP], w->is32 ? 4 : 8));
        w32_exit(w, 127);
        return;
    }
    if (w->verbose > 1) fprintf(stderr, "winrun: %s!%s(%#llx, %#llx, %#llx, %#llx)\n", w->stubs[i].dll->name, a->name,
                                (unsigned long long)w32_arg(w, 0), (unsigned long long)w32_arg(w, 1),
                                (unsigned long long)w32_arg(w, 2), (unsigned long long)w32_arg(w, 3));
    uint64_t rsp = c->gpr[XC_RSP];
    a->fn(w);
    if (w->exited) return;
    /* return to the caller: pop the return address (and the arguments for stdcall on x86) */
    if (w->is32) {
        c->rip = w32_read(w, rsp, 4);
        c->gpr[XC_RSP] = (uint32_t)(rsp + 4 + (a->conv == 0 ? 4u * a->nargs : 0));
    } else {
        c->rip = w32_read(w, rsp, 8);
        c->gpr[XC_RSP] = rsp + 8;
    }
}

/* Run until the process exits (depth 0) or the return-to-host stub is hit
 * (depth > 0, i.e. inside w32_call_guest). */
static int run_loop(w32 *w) {
    xc_cpu *c = w->c;
    for (;;) {
        if (w->exited) return 0;
        xc_stop st = xc_run(c, 1u << 20);
        if (st == XC_STOP_STEPS) continue;
        if (st == XC_STOP_BREAKPOINT) {
            uint64_t at = c->rip - 1;
            if (at >= w->stub_base && at < w->stub_base + 16u * W32_MAX_STUBS && !((at - w->stub_base) & 15)) {
                int i = (int)((at - w->stub_base) / 16);
                if (i == STUB_RETURN) { if (w->depth > 0) return 1; fprintf(stderr, "winrun: stray return-to-host\n"); w32_exit(w, 126); return 0; }
                if (i == STUB_EXIT) { w32_exit(w, (int)(uint32_t)c->gpr[XC_RAX]); return 0; }
                if (i < w->nstubs) { dispatch(w, i); continue; }
            }
            fprintf(stderr, "winrun: int3 in guest code at %#llx\n", (unsigned long long)at);
            w32_exit(w, 128 + 5); return 0;
        }
        char dis[128]; xc_disasm(c, c->rip, dis, sizeof dis);
        fprintf(stderr, "winrun: stopped: %s at rip=%#llx  [%s]", xc_stop_name(st), (unsigned long long)c->rip, dis);
        if (st == XC_STOP_FAULT) fprintf(stderr, "  fault_addr=%#llx", (unsigned long long)c->fault_addr);
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
static void on_crash(int sig, siginfo_t *si, void *uctx) {
    (void)uctx;
    xc_cpu *c = g_w.c;
    uint64_t lo = 0, hi = 0; int have = xc_jit_code_range(&lo, &hi);
    uint64_t fault = (uint64_t)(uintptr_t)si->si_addr;
    char buf[768];
    int n = snprintf(buf, sizeof buf,
        "winrun: host %s at address %#llx; guest rip=%#llx rsp=%#llx (%d-bit, %s); fault %s the JIT code region%s\n"
        "winrun: state: image %#llx+%#x entry %#llx stubs %#llx teb %#llx peb %#llx stack %#llx..%#llx heap %#llx depth %d exited %d\n",
        sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", (unsigned long long)fault,
        (unsigned long long)(c ? c->rip : 0), (unsigned long long)(c ? c->gpr[XC_RSP] : 0), g_w.is32 ? 32 : 64,
        xc_jit_enabled() ? "jit" : "interpreter",
        have && fault >= lo && fault < hi ? "inside" : "outside",
        g_w.is32 && fault >= (uint64_t)(uintptr_t)g_w.base && fault < (uint64_t)(uintptr_t)g_w.base + (1ull << 32)
            ? " -- inside the 4 GB arena (unmapped guest page)" : "",
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
    w32_reset_statics();
    xc_cache_flush();
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
        else if (!strcmp(argv[ai], "-L") && ai + 1 < argc) {       /* extra directory to find guest DLLs in */
            static char dir[512];
            snprintf(dir, sizeof dir, "%s%s", argv[ai + 1], argv[ai + 1][strlen(argv[ai + 1]) - 1] == '/' ? "" : "/");
            w->dll_dir = dir; ai++;
        }
        else { fprintf(stderr, "usage: winrun [-v] [-L dlldir] program.exe [args...]\n"); return 2; }
        ai++;
    }
    if (ai >= argc) { fprintf(stderr, "usage: winrun [-v] [-L dlldir] program.exe [args...]\n"); return 2; }
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
    stubs_init(w);
    /* TEB/PEB/stack/heap first (TLS callbacks need them); they come from the
     * arena's bump allocator or host mmap, neither of which lands on a PE32+
     * preferred base (0x140000000) or a PE32 one (0x400000) */
    process_init(w, argc - ai, argv + ai);
    if (w32_load_pe(w, argv[ai])) return 2;
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
