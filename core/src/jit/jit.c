/* xcore dynarec: x86 basic blocks -> AArch64 machine code.
 *
 * Shape
 * -----
 * The block cache (cache.c) decodes straight-line runs into `xop` form. This
 * file compiles each such block once, on first execution, into native code
 * that is entered from a dispatcher loop and returns to it at the block's
 * end with cpu->rip set. There is no IR: every x86 instruction lowers
 * directly to a handful of ARM64 instructions, with a static register map
 * (guest RAX..R15 live in x8-x17, x19-x24 while a block runs) and load-on-
 * first-use / store-on-exit so a block only touches the guest registers it
 * names. Anything the compiler does not handle natively is a *callout*: the
 * block spills, calls the interpreter for that one instruction, and carries
 * on. Coverage grows by moving instructions from the callout path to native
 * lowering; correctness never depends on how far that has got.
 *
 * Flags
 * -----
 * x86 arithmetic sets six flags per instruction; computing them eagerly
 * would cost more than the arithmetic. Instead the block records the last
 * flag-setting operation (kind, width, operands, result) in the cpu struct
 * -- the *lazy* state -- and xc_flags_sync() turns it into rflags on demand,
 * using the interpreter's own flag routines so the two paths agree exactly.
 * Within a block the compiler also knows which ARM condition codes are still
 * valid from the last ADDS/SUBS/ANDS it emitted, so `cmp; jcc` becomes a
 * native `subs; b.cond`. A liveness pre-pass drops the lazy store when the
 * next instruction overwrites every flag before anything reads them, which
 * is most of the time.
 *
 * Memory
 * ------
 * Guest addresses go through x25, the arena base: `ldr w0, [x25, w1, uxtw]`
 * is the 32-bit guest's base + zext32(addr) at no extra cost, and with x25 =
 * 0 the same form is the 64-bit guest's identity mapping. A guest access to
 * unmapped memory is a host fault, and each one carries an out-of-line
 * recovery stub so the runtime can turn it back into a guest fault with the
 * register state the faulting instruction had -- see "turning a host fault
 * into a guest one" below.
 *
 * Verification
 * ------------
 * The self-test replays the golden vectors through this path, so the JIT is
 * held to the same silicon recordings as the interpreter; the block cache's
 * byte check makes self-modifying code safe here too. test_jitdiff compares
 * final state against the interpreter over the whole differential suite, and
 * test_faultdiff does the same for state at a fault -- against what x86 says
 * it should be, rather than against the interpreter, since a faulting
 * instruction has no effect and that pins the answer exactly.
 *
 * Host: AArch64 only. On other hosts xc_jit_available() is 0 and xc_run
 * uses the interpreter.
 */
#include "xcore/cpu.h"
#include "../xop.h"

#include <Zydis/Zydis.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#define XC_JIT_HOST 1
#else
#define XC_JIT_HOST 0
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif

#include "a64.h"
#include "../cache.h"

/* ------------------------------------------------------------ platform */

static size_t g_code_used;
static uint64_t g_stat_blocks, g_stat_callouts;
/* Bytes ever emitted. Not the same as g_code_used, which is how much of the
 * arena is in use *now*: a cache flush resets that to zero while the block
 * count keeps climbing, and reporting the two together then reads as 4695
 * blocks in 64 KB. Blocks are cumulative, so the bytes beside them must be. */
static uint64_t g_stat_bytes;
/* x87 instructions the compiler lowered natively vs sent to the interpreter,
 * counted at compile time. The ratio is what says whether the 53-bit fast
 * path is actually engaging on a given program (see xc_jit_x87_stats). */
static uint64_t g_stat_x87_native, g_stat_x87_callout;
static int g_enabled = -1;
#if XC_JIT_HOST
static int g_callout_stats, g_stat_callouts_reg;   /* XCORE_JIT_CALLOUTS=1: histogram of interpreter callouts at exit */
#endif

#if XC_JIT_HOST
#include <sys/mman.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
/* Code memory. Written through `rw`, executed at `rx`; on Linux and macOS
 * they are the same mapping, on iOS the host supplies a dual-mapped arena
 * (xc_jit_set_code) whose RX side the debugger has blessed. */
static uint8_t *g_code_rw, *g_code_rx; static size_t g_code_cap;    /* block code */
static uint8_t *g_stub_rw, *g_stub_rx;                              /* the enter/dispatch stub, its own page */
static int g_external;                                              /* region came from xc_jit_set_code */
#define RX(p) ((uint8_t *)(p) - g_code_rw + g_code_rx)

#if !TARGET_OS_IPHONE
static void *rwx_alloc(size_t sz) {
#if defined(__APPLE__)
    void *p = mmap(0, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    void *p = mmap(0, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    return p == MAP_FAILED ? 0 : p;
}
#endif
static int code_alloc(size_t sz) {
    if (g_external) return 1;
#if TARGET_OS_IPHONE
    return 0;                       /* iOS: only an arena the debugger blessed will do */
#else
    if (!(g_code_rw = rwx_alloc(sz))) return 0;
    g_code_rx = g_code_rw;
    g_code_cap = sz; g_code_used = 0;
    g_stub_rw = rwx_alloc(4096);
    g_stub_rx = g_stub_rw;
    return g_stub_rw != 0;
#endif
}
/* macOS toggles the thread's JIT write permission (MAP_JIT); iOS code memory
 * is the host's dual-mapped arena, written through its RW alias. */
static void code_write_begin(void) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    if (!g_external) pthread_jit_write_protect_np(0);
#endif
}
/* p is a write-side pointer; the instruction cache is invalidated on the
 * execute side. */
static void code_write_end(void *p, size_t n, void *rx) {
#if defined(__APPLE__)
#if !TARGET_OS_IPHONE
    if (!g_external) pthread_jit_write_protect_np(1);
#endif
    sys_icache_invalidate(rx, n);
#else
    (void)rx;
    __builtin___clear_cache((char *)p, (char *)p + n);
#endif
}
#endif

/* Host-supplied code memory: `rw` and `rx` map the same `size` bytes. The
 * first 4 KB become the dispatcher, the rest block code. Call before the
 * first xc_run. */
int xc_jit_set_code(void *rw, void *rx, size_t size) {
#if XC_JIT_HOST
    if (size < 65536 + 4096) return 0;
    g_stub_rw = rw; g_stub_rx = rx;
    g_code_rw = (uint8_t *)rw + 4096; g_code_rx = (uint8_t *)rx + 4096;
    g_code_cap = size - 4096; g_code_used = 0;
    g_external = 1;
    return 1;
#else
    (void)rw; (void)rx; (void)size; return 0;
#endif
}

int xc_jit_available(void) {
#if XC_JIT_HOST && TARGET_OS_IPHONE
    return g_external;
#else
    return XC_JIT_HOST;
#endif
}
void xc_jit_enable(int on) { g_enabled = on ? 1 : 0; }
int xc_jit_enabled(void) {
    if (g_enabled < 0) { const char *e = getenv("XCORE_JIT"); g_enabled = XC_JIT_HOST && !(e && e[0] == '0'); }
    return g_enabled && xc_jit_available();
}
/* Block chaining. A block whose exit target is a constant ends in a `b` that
 * initially falls through to the slow path (store rip, hand the site address
 * to the dispatcher). The first time the dispatcher resolves that target it
 * patches the `b` to jump straight into the target's code, and records the
 * site on the target block so the link can be undone if the target is ever
 * dropped (self-modifying code, unmapping). After that the two blocks run
 * back to back with no dispatcher, no hash lookup and no byte check in
 * between; the step budget is checked in each block's prologue instead.
 *
 * A link also carries the guest registers. Every block begins with a prologue
 * that loads the registers it refers to (`block.live_in`) into their fixed
 * host registers, and `block.warm` is the entry just past it. A chained exit
 * publishes, in a word next to its branch, the set it is leaving live; when
 * the link is made, whatever the target wants and the predecessor does not
 * already hold is loaded by a small stub, and the branch goes to `warm`. A
 * loop that chains back to itself needs no stub at all and reloads nothing:
 * its registers stay in x8-x24 and v16-v31 for as long as it spins.
 *
 * The stores are still there -- an exit flushes dirty registers to xc_cpu
 * before it leaves, because a callout, a fault or a dispatcher exit further
 * down the chain has to find the architectural state where it always was.
 * Dropping those too needs the dirty set to reach a fixpoint around the loop,
 * which is a bigger change than this one. */
typedef struct { uint32_t *site_rw, *site_rx; uint32_t next; } link_rec;
enum { MAX_LINKS = 1u << 17 };
static uint32_t g_nlinks;                                 /* index 0 is the list terminator */
static uint64_t g_stat_links, g_stat_link_warm, g_stat_link_stub;
#if XC_JIT_HOST
static link_rec *g_links;
static int g_chain = -1;
#endif

/* Recovery stubs: see "turning a host fault into a guest one" below. */
typedef struct { uint64_t at, stub, rip; } faultsite;   /* the access, its recovery stub, the guest instruction */
enum { MAX_FAULT_SITES = 16384, MAX_BLOCK_SITES = 384 };
static faultsite g_fsite[MAX_FAULT_SITES];
static int g_nfsite;

void xc_jit_code_reset(void) { g_code_used = 0; g_nlinks = 0; g_nfsite = 0; }

/* The recovery stub for a host PC, or NULL if that PC is not a guest memory
 * access this compiler emitted. An exact match is required and is what makes
 * the answer trustworthy: the PC either is one of the recorded ldr/str
 * instructions or it is something else entirely, and there is no third case
 * to guess about. */
void *xc_jit_fault_stub(uint64_t host_pc, uint64_t *guest_rip) {
    int lo = 0, hi = g_nfsite - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_fsite[m].at == host_pc) {
            if (guest_rip) *guest_rip = g_fsite[m].rip;
            return (void *)(uintptr_t)g_fsite[m].stub;
        }
        if (g_fsite[m].at < host_pc) lo = m + 1; else hi = m - 1;
    }
    return 0;
}
uint64_t xc_jit_fault_sites(void) { return (uint64_t)g_nfsite; }

#if XC_JIT_HOST
void xc_jit_unlink(block *b) {
    if (!g_links) return;
    for (uint32_t i = b->links; i; ) {
        link_rec *l = &g_links[i];
        code_write_begin();
        *l->site_rw = 0x14000002u;                      /* b +8: over the live-out word, back to the slow path */
        code_write_end(l->site_rw, 4, l->site_rx);
        i = l->next;
    }
    b->links = 0;
}
#else
void xc_jit_unlink(block *b) { (void)b; }
#endif

void xc_jit_stats(uint64_t *blocks, uint64_t *callouts, uint64_t *bytes) {
    if (blocks) *blocks = g_stat_blocks;
    if (callouts) *callouts = g_stat_callouts;
    if (bytes) *bytes = g_stat_bytes;
}
uint64_t xc_jit_links(void) { return g_stat_links; }
void xc_jit_link_stats(uint64_t *links, uint64_t *warm, uint64_t *stub) {
    if (links) *links = g_stat_links;
    if (warm) *warm = g_stat_link_warm;
    if (stub) *stub = g_stat_link_stub;
}
void xc_jit_x87_stats(uint64_t *native, uint64_t *callout) {
    if (native) *native = g_stat_x87_native;
    if (callout) *callout = g_stat_x87_callout;
}
#if !XC_JIT_HOST
int xc_jit_callout_top(int n, const char **names, uint32_t *counts) { (void)n; (void)names; (void)counts; return 0; }
#endif
/* Where the generated code lives (execute side), for crash reports. */
int xc_jit_code_range(uint64_t *lo, uint64_t *hi) {
#if XC_JIT_HOST
    if (!g_code_rx) return 0;
    *lo = (uint64_t)(uintptr_t)g_code_rx; *hi = *lo + g_code_cap; return 1;
#else
    (void)lo; (void)hi; return 0;
#endif
}

#if XC_JIT_HOST

/* ----------------------------------------------------------- C helpers */
/* Called from generated code. All of them may clobber x0-x17, so the
 * compiler spills the guest register cache around every call. */

/* MXCSR <-> host FP control/status. The rounding mode and flush-to-zero are
 * mirrored into FPCR while JIT code runs; exception flags accumulate in FPSR
 * and are folded into MXCSR's sticky bits whenever C code (which may read or
 * clear them) is about to run. FPSR: IOC 0, DZC 1, OFC 2, UFC 3, IXC 4, IDC 7;
 * MXCSR: IE 0, DE 1, ZE 2, OE 3, UE 4, PE 5. */
static inline uint64_t rd_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void wr_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }
static inline uint64_t rd_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void wr_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static uint64_t g_host_fpcr;
static void fold_fpsr(xc_cpu *c) {
    uint64_t f = rd_fpsr() & 0x9f;
    if (!f) return;
    c->mxcsr |= (uint32_t)((f & 1) | ((f & 0x1e) << 1) | (((f >> 7) & 1) << 1));
    wr_fpsr(0);
}
static void fpcr_from_mxcsr(const xc_cpu *c) {
    static const uint64_t rmode[4] = { 0, 2, 1, 3 };           /* x86 RN,RM,RP,RZ -> ARM RN,RP,RM,RZ */
    uint64_t v = (g_host_fpcr & ~((3ull << 22) | (1ull << 24))) | (rmode[(c->mxcsr >> 13) & 3] << 22);
    if ((c->mxcsr & 0x8040) == 0x8040) v |= 1ull << 24;          /* FTZ+DAZ -> FZ */
    wr_fpcr(v);
}

/* One instruction through the interpreter. Returns 1 if the block must exit
 * afterwards (stop condition, or RIP left the straight line), else 0. */
/* Callouts by mnemonic, always counted (one increment per callout, which is
 * already a C call): a run that ends by the time limit can say what the
 * dynarec kept handing to the interpreter, which is where its time went. */
static uint32_t g_callout_hist[ZYDIS_MNEMONIC_MAX_VALUE + 1];
int xc_jit_callout_top(int n, const char **names, uint32_t *counts) {
    int top[12] = {0}; int k = 0;
    if (n > 12) n = 12;
    for (int m = 0; m <= ZYDIS_MNEMONIC_MAX_VALUE; m++) {
        if (!g_callout_hist[m]) continue;
        int i = k < n ? k++ : n - 1;
        if (i == n - 1 && k == n && g_callout_hist[m] <= g_callout_hist[top[n - 1]]) continue;
        top[i] = m;
        for (; i > 0 && g_callout_hist[top[i]] > g_callout_hist[top[i - 1]]; i--) { int t = top[i]; top[i] = top[i - 1]; top[i - 1] = t; }
    }
    for (int i = 0; i < k; i++) { names[i] = ZydisMnemonicGetString((ZydisMnemonic)top[i]); counts[i] = g_callout_hist[top[i]]; }
    return k;
}
static void callout_report(void) {
    const char *names[12]; uint32_t counts[12];
    int n = xc_jit_callout_top(12, names, counts);
    fprintf(stderr, "[jit] callouts by mnemonic:\n");
    for (int i = 0; i < n; i++) fprintf(stderr, "  %10u  %s\n", counts[i], names[i]);
}
static void x87_materialize(xc_cpu *c);
static void x87_refresh(xc_cpu *c);
static int jit_callout(xc_cpu *c, const dinsn *d) {
    g_stat_callouts++;
    g_callout_hist[d->in.mnemonic]++;
    if (g_callout_stats && !g_stat_callouts_reg) { atexit(callout_report); g_stat_callouts_reg = 1; }
    xc_flags_sync(c);
    fold_fpsr(c);
    x87_materialize(c);                     /* registers the JIT holds as doubles -> fpr[] */
    wr_fpcr(g_host_fpcr);                   /* the interpreter's own C math (libm transcendentals) runs in the host's mode... */
    xc_stop st = xc_exec_decoded(c, &d->in, xc_cache_ops(d));
    wr_fpsr(0);                             /* ...and whatever flags it raised are not the guest's */
    fpcr_from_mxcsr(c);                     /* the interpreter may have changed MXCSR, or reset the host rounding mode */
    if (d->in.meta.isa_ext == ZYDIS_ISA_EXT_X87 || d->in.mnemonic == ZYDIS_MNEMONIC_FXRSTOR || d->in.mnemonic == ZYDIS_MNEMONIC_FXRSTOR64)
        x87_refresh(c);                     /* fpr[] changed: which registers are doubles now? */
    if (st != XC_STOP_NONE) return 1;
    return c->rip != d->rip + d->in.length;
}
/* REP MOVS / REP STOS (and the single-shot forms) on host pointers: one call
 * per instruction instead of one interpreter element step per byte. Falls
 * back to the interpreter for DF=1, unmapped ranges and overlapping copies,
 * whose byte-serial semantics the interpreter already has right. */
static int jit_string(xc_cpu *c, const dinsn *d) {
    const ZydisDecodedInstruction *in = &d->in;
    int aw = in->address_width, bits, movs;
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: bits = 8;  movs = 1; break;
    case ZYDIS_MNEMONIC_MOVSW: bits = 16; movs = 1; break;
    case ZYDIS_MNEMONIC_MOVSD: bits = 32; movs = 1; break;
    case ZYDIS_MNEMONIC_MOVSQ: bits = 64; movs = 1; break;
    case ZYDIS_MNEMONIC_STOSB: bits = 8;  movs = 0; break;
    case ZYDIS_MNEMONIC_STOSW: bits = 16; movs = 0; break;
    case ZYDIS_MNEMONIC_STOSD: bits = 32; movs = 0; break;
    default:                   bits = 64; movs = 0; break;
    }
    int esz = bits / 8;
    int rep = (in->attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
    uint64_t amask = aw == 64 ? ~0ull : 0xFFFFFFFFull;
    if (c->rflags & (1u << 10)) return jit_callout(c, d);               /* DF: backwards */
    uint64_t n = rep ? (c->gpr[XC_RCX] & amask) : 1;
    if (n == 0) { c->rip = d->rip + in->length; return 0; }
    uint64_t di = c->gpr[XC_RDI] & amask, si = c->gpr[XC_RSI] & amask, len = n * (uint64_t)esz;
    if (len / esz != n || di + len < di || (movs && si + len < si)) return jit_callout(c, d);
    uint8_t *dst = xc_mem_ptr(c->mem, di, len);
    if (!dst) return jit_callout(c, d);
    if (movs) {
        const uint8_t *src = xc_mem_ptr(c->mem, si, len);
        if (!src || (dst > src && dst < src + len)) return jit_callout(c, d);   /* forward-overlap: byte-serial semantics */
        memmove(dst, src, len);
        c->gpr[XC_RSI] = (si + len) & amask;
    } else {
        uint64_t v = c->gpr[XC_RAX];
        if (esz == 1) memset(dst, (int)(v & 0xff), len);
        else for (uint64_t i = 0; i < n; i++) memcpy(dst + i * esz, &v, esz);
    }
    c->gpr[XC_RDI] = (di + len) & amask;
    if (rep) c->gpr[XC_RCX] = 0;
    c->rip = d->rip + in->length;
    return 0;
}
/* Evaluate an x86 condition (0..15) against the true flags. */
static int jit_cc(xc_cpu *c, int cc) {
    xc_flags_sync(c);
    uint64_t f = c->rflags;
    int cf = (f >> 0) & 1, pf = (f >> 2) & 1, zf = (f >> 6) & 1, sf = (f >> 7) & 1, of = (f >> 11) & 1, r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf | zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf ^ of; break;
    default: r = (sf ^ of) | zf; break;
    }
    return (cc & 1) ? !r : r;
}
static int jit_cf(xc_cpu *c) { xc_flags_sync(c); return (int)(c->rflags & 1); }
/* A guest address outside a bounded arena: report it as the interpreter would. */
static void jit_fault(xc_cpu *c, uint64_t addr, uint64_t rip) { c->stop = XC_STOP_FAULT; c->fault_kind = XC_FAULT_MEM; c->fault_addr = addr; c->rip = rip; }
static void jit_sync(xc_cpu *c) { xc_flags_sync(c); }

/* --------------------------------------------------------- the compiler */

/* host registers holding guest GPRs 0..15 (x18 is Apple's platform register) */
static const int HREG[16] = { 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24 };
enum { R_BASE = 25, R_CPU = 26, R_DISP = 27, R_TMP = 28 };
/* x0-x4 scratch; x5,x6,x7 hold a,b,r of the last flag-setting op when jc.abr is set */
enum { T0 = 0, T1, T2, T3, T4, RA = 5, RB = 6, RR = 7 };

#define OFF(f) ((uint32_t)offsetof(xc_cpu, f))

/* what the compiler knows about condition flags at this point */
enum { NZ_NONE = 0, NZ_ADD, NZ_SUB, NZ_LOGIC, NZ_INC };
/* what it knows about cpu->lz_op / rflags validity */
enum { LZ_UNKNOWN = 0, LZ_VALID /* rflags exact, lz_op NONE */, LZ_LOGIC0 /* CF known 0 */, LZ_ABR /* lazy add/sub, a,b,r in x5-x7 */, LZ_INCDEC /* lz_cf holds CF */ };

typedef struct {
    a64 a;
    int mode;                   /* 32 or 64 */
    uint16_t loaded, dirty;     /* guest GPRs cached in HREG[] */
    uint16_t xloaded, xdirty;   /* guest XMMs cached in v16-v31 */
    int nz, nz_bits;
    int lz;
    const block *b;
    const dinsn *d;             /* current instruction */
    const xop *ops;
    int flags_live;             /* after the current instruction */
    int failed;
    int ended;                  /* the block was closed early (an exit was emitted); stop compiling */
    int probe;                  /* first pass: emitting only to find out what the block loads (see compile) */
    int invalidated;            /* a callout has cleared the cache, so later loads are not entry loads */
    uint32_t lazy;              /* registers loaded on demand before that point -- GPR g in bit g, XMM x in bit 16+x */
    uint64_t bound;             /* arena size when smaller than 4 GB (32-bit mode), else 0: no checks */

    /* x87 (see jit_x87.h). The eight physical registers live in d8-d15 as
     * doubles while the block runs; x87_v[] is the renaming (FXCH swaps
     * entries). TOP and the tag word are compile-time state, checked by a
     * guard when the block first touches the FPU. */
    int x87_on;                 /* this block may lower x87 natively (mode allowed it at compile time) */
    int x87_guarded;            /* the guard has been emitted */
    int x87_top;                /* static TOP */
    uint8_t x87_empty;          /* static tag word: bit p = empty */
    uint8_t x87_empty0;         /* ... as it was at the last writeback */
    int x87_v[8];               /* host D register holding physical register p */
    uint8_t x87_loaded, x87_dirty;
    int x87_touched;            /* fsw/tag need writing back at the exit */
    int x87_c1clr;              /* C1 reads 0 at the exit */
    int x87_ixc;                /* a fast-path op since the last fold may have left IXC in FPSR */
    int x87_rc;                 /* rounding mode assumed (x86 encoding) */
    int fp_unit;                /* who owns the flags in FPSR: UNIT_MXCSR (default) or UNIT_X87 */
    uint16_t x87_fcw; uint32_t x87_mxcsr;   /* the control words the block was compiled for */
} jc;

static int greg(jc *j, int g) {
    if (!(j->loaded & (1u << g))) {
        a64_ldr_off(&j->a, 3, HREG[g], R_CPU, OFF(gpr) + 8u * g);
        j->loaded |= 1u << g;
        if (!j->invalidated) j->lazy |= 1u << g;
    }
    return HREG[g];
}
static void gdirty(jc *j, int g) { j->loaded |= 1u << g; j->dirty |= 1u << g; }
/* XMM0-15 live in v16-v31 while a block runs; v0-v3 are temporaries. Both
 * ranges are caller-saved, so a C call clobbers them -- the cache is spilled
 * and invalidated around every call, as for the GPRs. */
#define VREG(x) (16 + (x))
enum { VT0 = 0, VT1 = 1, VT2 = 2, VT3 = 3 };
static int xreg(jc *j, int x) {
    if (!(j->xloaded & (1u << x))) {
        a64_fldst_off(&j->a, 2, 1, VREG(x), R_CPU, OFF(xmm) + 16u * x);
        j->xloaded |= 1u << x;
        if (!j->invalidated) j->lazy |= 0x10000u << x;
    }
    return VREG(x);
}
static void xset(jc *j, int x) { j->xloaded |= 1u << x; j->xdirty |= 1u << x; }   /* about to be fully overwritten */
/* Load the guest registers in `mask` (GPR g in bit g, XMM x in bit 16+x) into
 * their fixed host registers. The block prologue and the link stubs share it,
 * which is what lets a chained edge hand registers over: both sides agree on
 * where a guest register lives, so the only question is who loaded it. */
static void emit_reg_loads(a64 *a, uint32_t mask) {
    for (int g = 0; g < 16; g++) if (mask & (1u << g)) a64_ldr_off(a, 3, HREG[g], R_CPU, OFF(gpr) + 8u * g);
    for (int x = 0; x < 16; x++) if (mask & (0x10000u << x)) a64_fldst_off(a, 2, 1, VREG(x), R_CPU, OFF(xmm) + 16u * x);
}
static int reg_count(uint32_t mask) { int n = 0; for (; mask; mask &= mask - 1) n++; return n; }
static void x87_spill(jc *j);       /* jit_x87.h: dirty ST registers, TOP/tags, FPSR flags -> cpu */
static void x87_flushed(jc *j);
static void x87_invalidate(jc *j);
/* Emit the stores for every dirty register without changing compile state. */
static void spill(jc *j) {
    for (int g = 0; g < 16; g++) if (j->dirty & (1u << g)) a64_str_off(&j->a, 3, HREG[g], R_CPU, OFF(gpr) + 8u * g);
    for (int x = 0; x < 16; x++) if (j->xdirty & (1u << x)) a64_fldst_off(&j->a, 2, 0, VREG(x), R_CPU, OFF(xmm) + 16u * x);
    x87_spill(j);
}
static void flush(jc *j) { spill(j); j->dirty = 0; j->xdirty = 0; x87_flushed(j); }
/* A C call clobbered the caches. Loads after this point are not entry loads:
 * whatever the block needs it will fetch again anyway, so hoisting them into
 * the prologue would only make the cold path and the link stubs longer. */
static void invalidate(jc *j) { j->loaded = j->dirty = 0; j->xloaded = j->xdirty = 0; j->invalidated = 1; x87_invalidate(j); }

/* ---------------------------------------- turning a host fault into a guest one
 *
 * A 32-bit guest's arena is a 4 GB reservation with only the pages the guest
 * has claimed mapped over it, so a guest dereference of a stray pointer is a
 * *host* SIGSEGV, at the very ldr/str this compiler emitted for it. To hand
 * that to the guest as an access violation the runtime needs the guest's
 * register state at that instruction -- and at that instruction the state is
 * in host registers, written back to the cpu struct only at block boundaries.
 *
 * Reconstructing it in C would mean a second implementation of everything
 * spill() knows -- which host register holds which guest register, the x87
 * renaming, the tag word, the pending FPSR fold -- kept in step with the
 * first by hand, in the one place where a disagreement is hardest to notice.
 *
 * So the generated code does it. Every guest memory access gets an
 * out-of-line *recovery stub* holding exactly the spill sequence for that
 * point in the block, and the signal handler moves the host PC to the stub
 * and returns. The stub writes every live register back and leaves through
 * the dispatcher, which returns XC_STOP_FAULT with a cpu struct as consistent
 * as if the block had ended there -- so the runtime can raise a guest
 * exception, and a handler that repairs a register and resumes lands back in
 * a freshly compiled block.
 *
 * The guest RIP is not baked into the stub: the handler already writes
 * cpu->stop and the fault address, and the side table can hand it the RIP as
 * well. That leaves the stub a pure function of the spill state, so every
 * access in a block that has the same registers live shares one -- which in
 * a straight-line block is a handful of stubs rather than one per access.
 *
 * Nothing is added to the fast path. The cost is code size, in blocks that
 * touch memory.
 */
/* Per-block, during compilation: where each access is and the whole compile
 * state at that point, so the stub can be emitted afterwards by the same
 * emitters that would have run there. */
typedef struct {
    uint32_t off;               /* word offset of the faulting instruction */
    uint32_t stub_off;          /* word offset of its recovery stub */
    uint64_t rip;               /* the guest instruction to report and resume at */
    int rsp_fix, rsp_sf;        /* undo a stack adjustment the lowering made early (push) */
    jc snap;                    /* compile state at the access; `a` is zeroed so states compare */
} psite;
static psite g_psite[MAX_BLOCK_SITES];
static int g_npsite;

/* Called immediately before emitting a guest memory access.
 *
 * `rsp_fix` exists for push: the lowering decrements the guest SP and then
 * stores, but x86 reports a faulting push with the SP it had before the
 * instruction, so the stub adds it back. Every other access leaves the guest
 * registers exactly as they were before the instruction, which is what makes
 * resuming at `rip` correct. */
static void fault_site_fix(jc *j, int rsp_fix, int rsp_sf) {
    if (j->probe || g_npsite >= MAX_BLOCK_SITES) return;
    psite *p = &g_psite[g_npsite++];
    p->off = a64_here(&j->a);
    p->rip = j->d ? j->d->rip : 0;
    p->rsp_fix = rsp_fix; p->rsp_sf = rsp_sf;
    p->snap = *j;
    memset(&p->snap.a, 0, sizeof p->snap.a);        /* the buffer is not part of the state */
}
static void fault_site(jc *j) { fault_site_fix(j, 0, 0); }

/* After the block body: one stub per distinct spill state. */
static void emit_fault_stubs(jc *j) {
    if (j->probe || !g_npsite) return;
    jc after = *j;                                  /* state at the end of the body */
    for (int i = 0; i < g_npsite; i++) {
        psite *p = &g_psite[i];
        int share = -1;
        for (int k = 0; k < i && share < 0; k++)
            if (g_psite[k].rsp_fix == p->rsp_fix && g_psite[k].rsp_sf == p->rsp_sf &&
                !memcmp(&g_psite[k].snap, &p->snap, sizeof(jc)))
                share = k;
        if (share >= 0) { p->stub_off = g_psite[share].stub_off; continue; }
        p->stub_off = a64_here(&j->a);
        a64 keep = j->a;
        *j = p->snap; j->a = keep;                  /* emit as if we were back at the access */
        if (p->rsp_fix) a64_add_imm(&j->a, p->rsp_sf, HREG[XC_RSP], HREG[XC_RSP], (uint32_t)p->rsp_fix);
        spill(j);
        a64_br(&j->a, R_DISP);
        keep = j->a; *j = after; j->a = keep;
    }
}

/* Register the block's sites once it has compiled without overflowing. Blocks
 * are emitted forward through one bump-allocated arena, so appending keeps
 * the table sorted by host address and the lookup is a binary search. */
static void register_fault_sites(const jc *j) {
    if (j->probe) return;
    uint64_t base = (uint64_t)(uintptr_t)RX(j->a.buf);
    for (int i = 0; i < g_npsite && g_nfsite < MAX_FAULT_SITES; i++) {
        g_fsite[g_nfsite].at = base + 4u * g_psite[i].off;
        g_fsite[g_nfsite].stub = base + 4u * g_psite[i].stub_off;
        g_fsite[g_nfsite].rip = g_psite[i].rip;
        g_nfsite++;
    }
}

/* Address of a memory operand into `rd`. Returns 1 if the address is a
 * 32-bit quantity (use uxtw addressing), 0 if 64-bit. */
static int emit_ea(jc *j, const xop *op, int rd) {
    int aw = j->d->in.address_width;
    int sf = aw == 64;
    int have = -1;
    if (op->mbase >= 0) have = greg(j, op->mbase);
    if (op->mindex >= 0) {
        int idx = greg(j, op->mindex);
        int sh = op->mscale == 8 ? 3 : op->mscale == 4 ? 2 : op->mscale == 2 ? 1 : 0;
        if (have >= 0) a64_add_shifted(&j->a, sf, rd, have, idx, SH_LSL, sh);
        else a64_lsl_imm(&j->a, sf, rd, idx, sh);
        have = rd;
    }
    if (op->disp != 0 || have < 0) {
        int64_t d = sf ? op->disp : (int64_t)(int32_t)op->disp;
        uint64_t disp = sf ? (uint64_t)d : (uint32_t)d;
        if (have < 0) { a64_mov_imm(&j->a, rd, disp); have = rd; }
        else if (d >= 0 && d < 4096) a64_add_imm(&j->a, sf, rd, have, (uint32_t)d);
        else if (d < 0 && -d < 4096) a64_sub_imm(&j->a, sf, rd, have, (uint32_t)-d);
        else { a64_mov_imm(&j->a, T4 == rd ? T3 : T4, disp); a64_add(&j->a, sf, rd, have, T4 == rd ? T3 : T4); }
        have = rd;
    }
    if (op->mseg) {
        int t = rd == T3 ? T2 : T3;
        a64_ldr_off(&j->a, 3, t, R_CPU, op->mseg == 1 ? OFF(fs_base) : OFF(gs_base));
        a64_add(&j->a, sf, rd, have, t);
        have = rd;
    }
    if (have != rd) a64_mov_reg(&j->a, sf, rd, have);
    return !sf;
}
static int ldst_size(int bits) { return bits == 64 ? 3 : bits == 32 ? 2 : bits == 16 ? 1 : 0; }

static void emit_set_rip_imm(jc *j, uint64_t rip);
/* Bounded arena (smaller than 4 GB): fault the way the interpreter does when
 * the address in T4 is past the end. Full-size arenas need no check -- every
 * 32-bit address lands inside the reservation and unmapped pages fault in
 * the host, which is the runtime's problem to translate. */
static void emit_bounds(jc *j, int bytes) {
    if (!j->bound) return;
    a64_mov_imm(&j->a, T3, j->bound - (uint64_t)bytes);
    a64_cmp(&j->a, 0, T4, T3);
    uint32_t ok = a64_here(&j->a); a64_bcond(&j->a, CC_LS, 0);
    /* out of range: spill, report, leave */
    spill(j);                                                   /* this path does not change compile state */
    a64_mov_reg(&j->a, 1, 0, R_CPU);
    a64_mov_reg(&j->a, 1, 1, T4);
    a64_mov_imm(&j->a, 2, j->d->rip);
    a64_mov_imm(&j->a, R_TMP, (uint64_t)(uintptr_t)jit_fault);
    a64_blr(&j->a, R_TMP);
    a64_br(&j->a, R_DISP);
    a64_patch_bcond(&j->a, ok, a64_here(&j->a));
}

/* Load operand into a register, zero- (or sign-) extended to 64 bits.
 * Returns the register that holds it -- possibly the guest's own host
 * register when no adjustment was needed (do not write to it). */
static int ld_op(jc *j, const xop *op, int rd, int sext) {
    int bits = op->size;
    switch (op->type) {
    case XOP_REG: {
        int h = greg(j, op->ridx);
        if (op->rhi8) { if (sext) a64_sbfx(&j->a, 1, rd, h, 8, 8); else a64_ubfx(&j->a, 1, rd, h, 8, 8); return rd; }
        if (bits == 64 || (bits == 32 && !sext)) return h;        /* 32-bit values are kept zero-extended */
        if (bits == 32) { a64_sxtw(&j->a, rd, h); return rd; }
        if (bits == 16) { if (sext) a64_sxth(&j->a, 1, rd, h); else a64_uxth(&j->a, rd, h); return rd; }
        if (sext) a64_sxtb(&j->a, 1, rd, h); else a64_uxtb(&j->a, rd, h);
        return rd;
    }
    case XOP_MEM: {
        int w = emit_ea(j, op, T4);
        int opt = w ? 2 : 3;
        emit_bounds(j, bits / 8);
        fault_site(j);
        if (sext && bits < 64) a64_ldrs_reg(&j->a, ldst_size(bits), 1, rd, R_BASE, T4, opt);
        else a64_ldr_reg(&j->a, ldst_size(bits), rd, R_BASE, T4, opt);
        return rd;
    }
    case XOP_IMM:
        /* xop immediates are already sign-extended to 64 bits (an imm8 in
         * `and esp, -16` is 0xFFFF...F0); the consumer masks to its width */
        a64_mov_imm(&j->a, rd, op->imm);
        return rd;
    default:
        j->failed = 1; return rd;
    }
}

/* Store the low `bits` of rs into the operand. */
static void st_op(jc *j, const xop *op, int rs) {
    int bits = op->size;
    if (op->type == XOP_REG) {
        int g = op->ridx, h;
        switch (bits) {
        case 64: h = HREG[g]; if (h != rs) a64_mov_reg(&j->a, 1, h, rs); gdirty(j, g); return;
        case 32: h = HREG[g]; a64_mov_reg(&j->a, 0, h, rs); gdirty(j, g); return;   /* zero-extends */
        case 16: h = greg(j, g); a64_bfi(&j->a, 1, h, rs, 0, 16); gdirty(j, g); return;
        default: h = greg(j, g); a64_bfi(&j->a, 1, h, rs, op->rhi8 ? 8 : 0, 8); gdirty(j, g); return;
        }
    }
    if (op->type == XOP_MEM) {
        int w = emit_ea(j, op, T4);
        emit_bounds(j, bits / 8);
        fault_site(j);
        a64_str_reg(&j->a, ldst_size(bits), rs, R_BASE, T4, w ? 2 : 3);
        return;
    }
    j->failed = 1;
}

/* --- exits --- */

static void emit_set_rip_imm(jc *j, uint64_t rip) { a64_mov_imm(&j->a, T0, rip); a64_str_off(&j->a, 3, T0, R_CPU, OFF(rip)); }
/* Exit to a constant address: a chainable link site (see xc_jit_unlink). */
static void emit_exit_imm(jc *j, uint64_t rip) {
    flush(j);
    if (g_chain < 0) { const char *e = getenv("XCORE_JIT_CHAIN"); g_chain = !(e && e[0] == '0'); }
    if (!g_chain) { emit_set_rip_imm(j, rip); a64_br(&j->a, R_DISP); return; }
    uint32_t site = a64_here(&j->a);
    a64_b(&j->a, 2);                                   /* patched to `b <target block>` once known */
    /* Not executed: the registers this exit leaves live, for chain() to read.
     * flush() stored the dirty ones, so a live register and its xc_cpu slot
     * agree -- the target may keep it or reload it, whichever is cheaper. */
    a64_emit(&j->a, (uint32_t)j->loaded | ((uint32_t)j->xloaded << 16));
    emit_set_rip_imm(j, rip);
    a64_adr(&j->a, 1, -(int32_t)((a64_here(&j->a) - site) * 4));   /* x1 = the site, for the dispatcher to patch */
    a64_add_imm(&j->a, 1, R_TMP, R_DISP, 4);           /* dispatcher entry that keeps x1 */
    a64_br(&j->a, R_TMP);
}
/* Exit to a computed address in `r`. flush() may clobber x0-x2 (the x87
 * writeback uses them as scratch), so store the target before flushing --
 * r is a scratch temp holding the value, not a guest register. */
static void emit_exit_reg(jc *j, int r) {
    a64_str_off(&j->a, 3, r, R_CPU, OFF(rip));
    flush(j);
    a64_br(&j->a, R_DISP);
}

/* Call a C helper: fn(cpu, arg1, arg2). Spills and invalidates the register cache. */
static void emit_call(jc *j, void *fn, uint64_t arg1, int has_arg1, int arg2reg) {
    flush(j);
    a64_mov_reg(&j->a, 1, 0, R_CPU);
    if (has_arg1) a64_mov_imm(&j->a, 1, arg1);
    if (arg2reg >= 0 && arg2reg != 2) a64_mov_reg(&j->a, 1, 2, arg2reg);
    a64_mov_imm(&j->a, R_TMP, (uint64_t)(uintptr_t)fn);
    a64_blr(&j->a, R_TMP);
    invalidate(j);
}

/* The instruction goes through the interpreter. */
static void emit_callout(jc *j) {
    emit_set_rip_imm(j, j->d->rip);
    emit_call(j, (void *)jit_callout, (uint64_t)(uintptr_t)j->d, 1, -1);
    /* returned 1: RIP is wherever the interpreter left it; go to the dispatcher */
    a64_cbz(&j->a, 0, 0, 2);
    a64_br(&j->a, R_DISP);
    j->nz = NZ_NONE; j->lz = LZ_VALID;
    j->x87_guarded = 0;                     /* the interpreter may have changed FCW/MXCSR (LDMXCSR, ...) */
}

/* MOVS/STOS through jit_string: like a callout, minus the flag sync (only DF
 * is read, and DF is never lazy). */
static void emit_string(jc *j) {
    emit_set_rip_imm(j, j->d->rip);
    emit_call(j, (void *)jit_string, (uint64_t)(uintptr_t)j->d, 1, -1);
    a64_cbz(&j->a, 0, 0, 2);
    a64_br(&j->a, R_DISP);
    /* the fallback path may have run the interpreter, which leaves rflags exact */
    j->nz = NZ_NONE; if (j->lz != LZ_VALID) j->lz = LZ_UNKNOWN;
}

/* --- lazy flag state --- */

/* Record a lazy flag op. a,b,r are in x5,x6,x7 (masked to `bits`). */
static void emit_lazy(jc *j, int op, int bits) {
    if (!j->flags_live) return;
    a64_mov_imm(&j->a, T0, (uint32_t)op | ((uint32_t)bits << 8));
    a64_str_off(&j->a, 2, T0, R_CPU, OFF(lz_op));
    a64_str_off(&j->a, 3, RA, R_CPU, OFF(lz_a));
    a64_str_off(&j->a, 3, RB, R_CPU, OFF(lz_b));
    a64_str_off(&j->a, 3, RR, R_CPU, OFF(lz_r));
}

/* Get the current CF into w0 (0/1), as cheaply as the compile-time state allows. */
static void emit_get_cf(jc *j) {
    switch (j->lz) {
    case LZ_VALID:  a64_ldr_off(&j->a, 3, T0, R_CPU, OFF(rflags)); a64_ubfx(&j->a, 0, T0, T0, 0, 1); break;
    case LZ_LOGIC0: a64_movz(&j->a, 0, T0, 0, 0); break;
    case LZ_ABR:
        if (j->nz == NZ_SUB) { a64_cmp(&j->a, 1, RA, RB); a64_cset(&j->a, 0, T0, CC_LO); }
        else { a64_cmp(&j->a, 1, RR, RA); a64_cset(&j->a, 0, T0, CC_LO); }
        break;
    case LZ_INCDEC: a64_ldr_off(&j->a, 2, T0, R_CPU, OFF(lz_cf)); break;
    default:
        emit_call(j, (void *)jit_cf, 0, 0, -1);
        j->lz = LZ_VALID;
        break;
    }
}

/* Make rflags exact (needed before partial-flag writers when the state is lazy). */
static void emit_sync(jc *j) {
    if (j->lz == LZ_VALID) return;
    emit_call(j, (void *)jit_sync, 0, 0, -1);
    j->lz = LZ_VALID;
}

#include "jit_sse.h"
#include "jit_x87.h"

/* --- conditions --- */

/* x86 condition code 0..15 for the Jcc/SETcc/CMOVcc mnemonic, or -1 */
static int x86_cc(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO: return 0;
    case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_CMOVNO: return 1;
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: return 2;
    case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: return 3;
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return 4;
    case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return 5;
    case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return 6;
    case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return 7;
    case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return 8;
    case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return 9;
    case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP: return 10;
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_CMOVNP: return 11;
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return 12;
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return 13;
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return 14;
    case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return 15;
    default: return -1;
    }
}

/* Put the truth value of x86 condition `cc` into w0 (0/1), natively when the
 * NZCV state allows, else through the helper. */
static void emit_cond_to_w0(jc *j, int cc) {
    int neg = cc & 1, base = cc >> 1;       /* base: 0 O, 1 B, 2 E, 3 BE, 4 S, 5 P, 6 L, 7 LE */
    int ac = -1;                            /* ARM condition for the un-negated base */
    int constant = -1;                      /* condition is a compile-time constant */
    switch (j->nz) {
    case NZ_SUB:
        switch (base) { case 0: ac = CC_VS; break; case 1: ac = CC_LO; break; case 2: ac = CC_EQ; break; case 3: ac = CC_LS; break;
                        case 4: ac = CC_MI; break; case 6: ac = CC_LT; break; case 7: ac = CC_LE; break; }
        break;
    case NZ_ADD:
        switch (base) { case 0: ac = CC_VS; break; case 1: ac = CC_HS; break; case 2: ac = CC_EQ; break;
                        case 4: ac = CC_MI; break; case 6: ac = CC_LT; break; case 7: ac = CC_LE; break;
                        case 3: /* BE = C || Z ; A = !C && !Z */
                                if (!neg) { a64_cset(&j->a, 0, T0, CC_HS); a64_cset(&j->a, 0, T1, CC_EQ); a64_orr(&j->a, 0, T0, T0, T1); }
                                else      { a64_cset(&j->a, 0, T0, CC_LO); a64_cset(&j->a, 0, T1, CC_NE); a64_and(&j->a, 0, T0, T0, T1); }
                                return; }
        break;
    case NZ_LOGIC:
        switch (base) { case 0: constant = 0; break; case 1: constant = 0; break; case 2: ac = CC_EQ; break; case 3: ac = CC_EQ; break;
                        case 4: ac = CC_MI; break; case 6: ac = CC_MI; break; case 7: ac = CC_LE; break; }
        break;
    case NZ_INC:
        switch (base) { case 0: ac = CC_VS; break; case 2: ac = CC_EQ; break; case 4: ac = CC_MI; break; case 6: ac = CC_LT; break; case 7: ac = CC_LE; break; }
        break;
    case NZ_FCMP:
        /* after fcmp: less N=1; equal Z=1; greater C=1; unordered C=V=1.
         * x86 COMIS: CF = less|unord, ZF = equal|unord, PF = unord */
        switch (base) { case 1: ac = CC_LT; break; case 3: ac = CC_LE; break; case 5: ac = CC_VS; break;
                        case 2: if (!neg) { a64_cset(&j->a, 0, T0, CC_EQ); a64_cset(&j->a, 0, T1, CC_VS); a64_orr(&j->a, 0, T0, T0, T1); }
                                else      { a64_cset(&j->a, 0, T0, CC_NE); a64_cset(&j->a, 0, T1, CC_VC); a64_and(&j->a, 0, T0, T0, T1); }
                                return; }
        break;
    }
    if (constant >= 0) { a64_movz(&j->a, 0, T0, (uint16_t)(constant ^ neg), 0); return; }
    if (ac >= 0) { a64_cset(&j->a, 0, T0, neg ? (ac ^ 1) : ac); return; }
    /* generic: helper evaluates against the true flags */
    emit_call(j, (void *)jit_cc, (uint64_t)cc, 1, -1);
    j->nz = NZ_NONE; j->lz = LZ_VALID;
}

/* --- ALU --- */

/* Narrow operands are shifted to the top of a 32-bit register so ADDS/SUBS
 * produce the right N/Z/C/V for the operand width. */
static void emit_alu(jc *j, ZydisMnemonic m) {
    const xop *dst = &j->ops[0], *src = &j->ops[1];
    int bits = dst->size;
    int sf = bits == 64;
    int ra = ld_op(j, dst, RA, 0), rb = ld_op(j, src, RB, 0);
    if (ra != RA) a64_mov_reg(&j->a, 1, RA, ra);
    if (rb != RB) a64_mov_reg(&j->a, 1, RB, rb);
    /* the source may be a sign-extended immediate: mask it to the width */
    if (src->type == XOP_IMM && bits < 64) { if (bits == 32) a64_mov_reg(&j->a, 0, RB, RB); else a64_ubfx(&j->a, 1, RB, RB, 0, bits); }
    int writes = !(m == ZYDIS_MNEMONIC_CMP || m == ZYDIS_MNEMONIC_TEST);
    int lz;
    if (bits >= 32) {
        switch (m) {
        case ZYDIS_MNEMONIC_ADD: a64_adds(&j->a, sf, RR, RA, RB); lz = XC_LZ_ADD; j->nz = NZ_ADD; break;
        case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: a64_subs(&j->a, sf, RR, RA, RB); lz = XC_LZ_SUB; j->nz = NZ_SUB; break;
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_TEST: a64_ands(&j->a, sf, RR, RA, RB); lz = XC_LZ_LOGIC; j->nz = NZ_LOGIC; break;
        case ZYDIS_MNEMONIC_OR:  a64_orr(&j->a, sf, RR, RA, RB); a64_tst(&j->a, sf, RR, RR); lz = XC_LZ_LOGIC; j->nz = NZ_LOGIC; break;
        default:                 a64_eor(&j->a, sf, RR, RA, RB); a64_tst(&j->a, sf, RR, RR); lz = XC_LZ_LOGIC; j->nz = NZ_LOGIC; break;
        }
    } else {
        int sh = 32 - bits;
        a64_lsl_imm(&j->a, 0, T0, RA, sh);
        a64_lsl_imm(&j->a, 0, T1, RB, sh);
        switch (m) {
        case ZYDIS_MNEMONIC_ADD: a64_adds(&j->a, 0, T2, T0, T1); lz = XC_LZ_ADD; j->nz = NZ_ADD; break;
        case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: a64_subs(&j->a, 0, T2, T0, T1); lz = XC_LZ_SUB; j->nz = NZ_SUB; break;
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_TEST: a64_ands(&j->a, 0, T2, T0, T1); lz = XC_LZ_LOGIC; j->nz = NZ_LOGIC; break;
        case ZYDIS_MNEMONIC_OR:  a64_orr(&j->a, 0, T2, T0, T1); a64_tst(&j->a, 0, T2, T2); lz = XC_LZ_LOGIC; j->nz = NZ_LOGIC; break;
        default:                 a64_eor(&j->a, 0, T2, T0, T1); a64_tst(&j->a, 0, T2, T2); lz = XC_LZ_LOGIC; j->nz = NZ_LOGIC; break;
        }
        a64_lsr_imm(&j->a, 0, RR, T2, sh);
    }
    j->nz_bits = bits;
    emit_lazy(j, lz, bits);
    j->lz = lz == XC_LZ_LOGIC ? LZ_LOGIC0 : LZ_ABR;
    if (writes) st_op(j, dst, RR);
}

static void emit_incdec(jc *j, int dec) {
    const xop *dst = &j->ops[0];
    int bits = dst->size, sf = bits == 64;
    emit_get_cf(j);                                  /* w0 = CF to preserve; may call out */
    a64_str_off(&j->a, 2, T0, R_CPU, OFF(lz_cf));
    int ra = ld_op(j, dst, RA, 0);
    if (ra != RA) a64_mov_reg(&j->a, 1, RA, ra);
    if (bits >= 32) {
        if (dec) a64_subs_imm(&j->a, sf, RR, RA, 1); else a64_adds_imm(&j->a, sf, RR, RA, 1);
    } else {
        int sh = 32 - bits;
        a64_lsl_imm(&j->a, 0, T0, RA, sh);
        a64_mov_imm(&j->a, T1, 1u << sh);
        if (dec) a64_subs(&j->a, 0, T2, T0, T1); else a64_adds(&j->a, 0, T2, T0, T1);
        a64_lsr_imm(&j->a, 0, RR, T2, sh);
    }
    a64_movz(&j->a, 0, RB, 1, 0);
    j->nz = NZ_INC; j->nz_bits = bits;
    emit_lazy(j, dec ? XC_LZ_DEC : XC_LZ_INC, bits);
    j->lz = LZ_INCDEC;
    st_op(j, dst, RR);
}

static void emit_neg_not(jc *j, int neg) {
    const xop *dst = &j->ops[0];
    int bits = dst->size, sf = bits == 64;
    int ra = ld_op(j, dst, RB, 0);
    if (!neg) {
        a64_mvn(&j->a, sf, RR, ra);
        st_op(j, dst, RR);
        return;
    }
    if (ra != RB) a64_mov_reg(&j->a, 1, RB, ra);
    a64_movz(&j->a, 0, RA, 0, 0);
    if (bits >= 32) a64_subs(&j->a, sf, RR, ZR, RB);
    else { int sh = 32 - bits; a64_lsl_imm(&j->a, 0, T1, RB, sh); a64_subs(&j->a, 0, T2, ZR, T1); a64_lsr_imm(&j->a, 0, RR, T2, sh); }
    j->nz = NZ_SUB; j->nz_bits = bits;
    emit_lazy(j, XC_LZ_SUB, bits);
    j->lz = LZ_ABR;
    st_op(j, dst, RR);
}

/* Shifts and rotates by immediate; SHL/SHR/SAR by CL for 32/64-bit. */
static int emit_shift(jc *j, ZydisMnemonic m) {
    const xop *dst = &j->ops[0], *cnt = &j->ops[1];
    int bits = dst->size, sf = bits == 64;
    int rot = m == ZYDIS_MNEMONIC_ROL || m == ZYDIS_MNEMONIC_ROR;
    if (cnt->type != XOP_IMM) {
        if (rot || bits < 32) return 0;
        /* variable count: masked; count 0 leaves flags alone */
        int ra = ld_op(j, dst, RA, 0); if (ra != RA) a64_mov_reg(&j->a, 1, RA, ra);
        int rc = greg(j, XC_RCX);
        a64_ubfx(&j->a, 0, RB, rc, 0, sf ? 6 : 5);
        int op = m == ZYDIS_MNEMONIC_SHL ? 0 : m == ZYDIS_MNEMONIC_SHR ? 1 : 2;
        a64_shiftv(&j->a, sf, op, RR, RA, RB);
        uint32_t skip = a64_here(&j->a); a64_cbz(&j->a, 0, RB, 0);
        emit_lazy(j, m == ZYDIS_MNEMONIC_SHL ? XC_LZ_SHL : m == ZYDIS_MNEMONIC_SHR ? XC_LZ_SHR : XC_LZ_SAR, bits);
        a64_patch_bcond(&j->a, skip, a64_here(&j->a));
        st_op(j, dst, RR);
        j->nz = NZ_NONE; j->lz = LZ_UNKNOWN;
        return 1;
    }
    uint32_t c = (uint32_t)cnt->imm & (sf ? 63 : 31);
    if (c == 0) return 1;                            /* no-op, no flag change */
    if (rot) {
        c %= bits;
        if (j->flags_live) emit_sync(j);              /* ROL/ROR keep the other four flags */
    }
    int ra = ld_op(j, dst, RA, 0); if (ra != RA) a64_mov_reg(&j->a, 1, RA, ra);
    a64_mov_imm(&j->a, RB, c);
    switch (m) {
    case ZYDIS_MNEMONIC_SHL:
        if (bits >= 32) a64_lsl_imm(&j->a, sf, RR, RA, (int)c);
        else if (c >= (uint32_t)bits) a64_movz(&j->a, 0, RR, 0, 0);
        else { a64_lsl_imm(&j->a, 0, RR, RA, (int)c); a64_ubfx(&j->a, 0, RR, RR, 0, bits); }
        emit_lazy(j, XC_LZ_SHL, bits); break;
    case ZYDIS_MNEMONIC_SHR:
        if (bits >= 32) a64_lsr_imm(&j->a, sf, RR, RA, (int)c);
        else if (c >= (uint32_t)bits) a64_movz(&j->a, 0, RR, 0, 0);
        else a64_lsr_imm(&j->a, 0, RR, RA, (int)c);
        emit_lazy(j, XC_LZ_SHR, bits); break;
    case ZYDIS_MNEMONIC_SAR:
        if (bits == 64) a64_asr_imm(&j->a, 1, RR, RA, (int)c);
        else if (bits == 32) a64_asr_imm(&j->a, 0, RR, RA, (int)c);
        else {
            if (bits == 16) a64_sxth(&j->a, 0, T0, RA); else a64_sxtb(&j->a, 0, T0, RA);
            a64_asr_imm(&j->a, 0, T0, T0, c >= (uint32_t)bits ? 31 : (int)c);
            a64_ubfx(&j->a, 0, RR, T0, 0, bits);
        }
        emit_lazy(j, XC_LZ_SAR, bits); break;
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: {
        uint32_t rc = m == ZYDIS_MNEMONIC_ROR ? c : (uint32_t)bits - c;      /* express as ROR */
        if (c == 0) a64_mov_reg(&j->a, 1, RR, RA);                          /* count % width == 0: value unchanged, flags still set from it */
        else if (bits >= 32) a64_ror_imm(&j->a, sf, RR, RA, (int)rc);
        else {
            a64_lsr_imm(&j->a, 0, T0, RA, (int)rc);
            a64_lsl_imm(&j->a, 0, T1, RA, bits - (int)rc);
            a64_orr(&j->a, 0, RR, T0, T1);
            a64_ubfx(&j->a, 0, RR, RR, 0, bits);
        }
        emit_lazy(j, m == ZYDIS_MNEMONIC_ROL ? XC_LZ_ROL : XC_LZ_ROR, bits);
        break;
    }
    default: return 0;
    }
    st_op(j, dst, RR);
    j->nz = NZ_NONE; j->lz = LZ_UNKNOWN;
    return 1;
}

static int emit_imul(jc *j) {
    int n = j->d->in.operand_count_visible;
    if (n < 2) return 0;
    const xop *dst = &j->ops[0], *a = &j->ops[n == 3 ? 1 : 0], *b = &j->ops[n == 3 ? 2 : 1];
    int bits = dst->size;
    if (bits < 16) return 0;
    int ra = ld_op(j, a, T0, 1), rb = ld_op(j, b, T1, 1);      /* sign-extended to 64 */
    if (bits == 64) {
        a64_mul(&j->a, 1, RR, ra, rb);
        a64_smulh(&j->a, T2, ra, rb);
        a64_asr_imm(&j->a, 1, T3, RR, 63);         /* overflow iff the high half is not the low half's sign */
        a64_cmp(&j->a, 1, T2, T3);
        a64_cset(&j->a, 0, RA, CC_NE);
    } else {
        a64_mul(&j->a, 1, T2, ra, rb);             /* 64-bit product of sign-extended inputs is exact for 16/32 */
        if (bits == 32) { a64_sxtw(&j->a, T3, T2); a64_mov_reg(&j->a, 0, RR, T2); }
        else { a64_sxth(&j->a, 1, T3, T2); a64_uxth(&j->a, RR, T2); }
        a64_cmp(&j->a, 1, T3, T2);
        a64_cset(&j->a, 0, RA, CC_NE);
    }
    a64_movz(&j->a, 0, RB, 0, 0);
    emit_lazy(j, XC_LZ_IMUL, bits);
    j->nz = NZ_NONE; j->lz = LZ_UNKNOWN;
    st_op(j, dst, RR);
    return 1;
}

/* --- stack --- */

static void emit_push_reg(jc *j, int rval) {         /* rval holds a stack-width value */
    int sw = j->d->in.stack_width, sf = sw == 64;
    int rsp = greg(j, XC_RSP);
    if (rval == rsp) { a64_mov_reg(&j->a, 1, T0, rsp); rval = T0; }   /* push rsp stores the old value */
    a64_sub_imm(&j->a, sf, rsp, rsp, sw / 8);
    gdirty(j, XC_RSP);
    if (j->bound) { a64_mov_reg(&j->a, 1, T4, rsp); emit_bounds(j, sw / 8); }
    fault_site_fix(j, sw / 8, sf);              /* the SP is already down; x86 reports the old one */
    a64_str_reg(&j->a, sf ? 3 : 2, rval, R_BASE, rsp, sf ? 3 : 2);
}
static void emit_pop_to(jc *j, int rd) {
    int sw = j->d->in.stack_width, sf = sw == 64;
    int rsp = greg(j, XC_RSP);
    if (j->bound) { a64_mov_reg(&j->a, 1, T4, rsp); emit_bounds(j, sw / 8); }
    fault_site(j);
    a64_ldr_reg(&j->a, sf ? 3 : 2, rd, R_BASE, rsp, sf ? 3 : 2);
    a64_add_imm(&j->a, sf, rsp, rsp, sw / 8);
    gdirty(j, XC_RSP);
}

/* ------------------------------------------------------- per instruction */

/* Can this instruction touch guest memory, and so fault?
 *
 * Asked of every operand rather than of the mnemonic, because Zydis decodes
 * the implicit ones too: the stack slot a push writes and a call pushes its
 * return address into are memory operands here, so there is no list of
 * special cases to keep up to date. LEA is the one instruction with a memory
 * operand it never accesses. */
static int insn_can_fault(const ZydisDecodedInstruction *in, const xop *ops) {
    if (in->mnemonic == ZYDIS_MNEMONIC_LEA) return 0;
    for (int k = 0; k < in->operand_count; k++) if (ops[k].type == XOP_MEM) return 1;
    return 0;
}

/* Which flag bits an instruction reads / writes, for the liveness pass. */
enum { FW_ALL = 1, FW_PART = 2, FR = 4 };
static int flag_use(const ZydisDecodedInstruction *in, const xop *ops) {
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_MUL:
        return in->mnemonic == ZYDIS_MNEMONIC_IMUL && in->operand_count_visible < 2 ? FW_ALL : FW_ALL;
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
        if (ops[1].type == XOP_IMM) return (ops[1].imm & (ops[0].size == 64 ? 63 : 31)) ? FW_ALL : 0;
        return FR | FW_PART;
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC:
    case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR:
    case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC: case ZYDIS_MNEMONIC_BT:
    case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC: case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD:
        return FR | FW_PART;
    case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD:
    case ZYDIS_MNEMONIC_LEA: case ZYDIS_MNEMONIC_PUSH: case ZYDIS_MNEMONIC_POP: case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_NOT: case ZYDIS_MNEMONIC_XCHG: case ZYDIS_MNEMONIC_LEAVE: case ZYDIS_MNEMONIC_JMP:
    case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_CBW: case ZYDIS_MNEMONIC_CWDE:
    case ZYDIS_MNEMONIC_CDQE: case ZYDIS_MNEMONIC_CWD: case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CQO:
    case ZYDIS_MNEMONIC_ENDBR64: case ZYDIS_MNEMONIC_ENDBR32:
    case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_MOVQ: case ZYDIS_MNEMONIC_MOVD: case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_MOVSD:
        return in->meta.category == ZYDIS_CATEGORY_STRINGOP ? FR : 0;
    case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_COMISD: case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_UCOMISD:
    case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP:
        return FW_ALL;
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVU:
    case ZYDIS_MNEMONIC_FCMOVNB: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNU:
        return FR;
    default:
        if (in->meta.isa_ext == ZYDIS_ISA_EXT_X87) return 0;     /* the FPU never touches rflags */
        for (int i = 0; i < in->operand_count_visible; i++)
            if (ops[i].type == XOP_REG && ops[i].rcls == XR_XMM && in->mnemonic != ZYDIS_MNEMONIC_PTEST) return 0;   /* SSE data ops leave rflags alone */
        return FR | FW_PART;         /* conservative: callout reads exact flags and may change them */
    }
}

/* Compile one instruction. */
static void emit_insn(jc *j) {
    const ZydisDecodedInstruction *in = &j->d->in;
    const xop *ops = j->ops;
    ZydisMnemonic m = in->mnemonic;
    int cc;

    /* the FPU: its own lowering, or a callout that also closes the block
     * (afterwards TOP and the tags are whatever the interpreter made them) */
    if (in->meta.isa_ext == ZYDIS_ISA_EXT_X87 || m == ZYDIS_MNEMONIC_FXSAVE || m == ZYDIS_MNEMONIC_FXSAVE64 ||
        m == ZYDIS_MNEMONIC_FXRSTOR || m == ZYDIS_MNEMONIC_FXRSTOR64) { emit_x87(j); return; }

    /* anything touching an XMM register goes to the SSE lowering */
    for (int i = 0; i < in->operand_count_visible; i++)
        if (ops[i].type == XOP_REG && ops[i].rcls == XR_XMM) {
            if (j->fp_unit == UNIT_X87) x87_fold_fpsr(j, 1);          /* FPSR flags so far belong to the FPU */
            if (!emit_sse(j)) emit_callout(j);
            return;
        }

    /* operands the native paths cannot describe: segment/other registers,
     * far pointers, anything wider than a GPR */
    for (int i = 0; i < in->operand_count_visible; i++) {
        const xop *o = &ops[i];
        if (m == ZYDIS_MNEMONIC_NOP) break;
        if ((o->type == XOP_REG && o->rcls != XR_GPR) || (o->type != XOP_NONE && o->size != 8 && o->size != 16 && o->size != 32 && o->size != 64)) { emit_callout(j); return; }
    }

    switch (m) {
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_ENDBR64: case ZYDIS_MNEMONIC_ENDBR32: case ZYDIS_MNEMONIC_PAUSE:
        return;

    case ZYDIS_MNEMONIC_MOV: {
        if (ops[0].type == XOP_REG && ops[0].size >= 32 && ops[1].type == XOP_IMM) {
            /* mov r32/r64, imm: straight into the guest register */
            int h = HREG[ops[0].ridx];
            a64_mov_imm(&j->a, h, ops[0].size == 64 ? ops[1].imm : (ops[1].imm & 0xFFFFFFFFu));
            gdirty(j, ops[0].ridx);
            return;
        }
        int r = ld_op(j, &ops[1], T0, 0);
        st_op(j, &ops[0], r);
        return;
    }
    case ZYDIS_MNEMONIC_MOVZX: { int r = ld_op(j, &ops[1], T0, 0); st_op(j, &ops[0], r); return; }
    case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: { int r = ld_op(j, &ops[1], T0, 1); st_op(j, &ops[0], r); return; }
    case ZYDIS_MNEMONIC_LEA: {
        int w = emit_ea(j, &ops[1], T0);
        (void)w;
        st_op(j, &ops[0], T0);                 /* st_op truncates to the destination size */
        return;
    }
    case ZYDIS_MNEMONIC_XCHG:
        if (ops[0].type == XOP_REG && ops[1].type == XOP_REG && ops[0].size >= 32) {
            int h0 = greg(j, ops[0].ridx), h1 = greg(j, ops[1].ridx);
            a64_mov_reg(&j->a, 1, T0, h0);
            st_op(j, &ops[0], h1);
            st_op(j, &ops[1], T0);
            return;
        }
        break;

    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST:
        emit_alu(j, m); return;
    case ZYDIS_MNEMONIC_INC: emit_incdec(j, 0); return;
    case ZYDIS_MNEMONIC_DEC: emit_incdec(j, 1); return;
    case ZYDIS_MNEMONIC_NEG: emit_neg_not(j, 1); return;
    case ZYDIS_MNEMONIC_NOT: emit_neg_not(j, 0); return;
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR:
        if (emit_shift(j, m)) return;
        break;
    case ZYDIS_MNEMONIC_IMUL:
        if (emit_imul(j)) return;
        break;

    case ZYDIS_MNEMONIC_BSWAP: {
        int h = greg(j, ops[0].ridx);
        if (ops[0].size == 64) a64_rev(&j->a, 1, h, h); else a64_rev(&j->a, 0, h, h);   /* 32-bit write zero-extends */
        gdirty(j, ops[0].ridx);
        return;
    }
    case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSQ:
    case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_STOSQ:
        emit_string(j); return;
    case ZYDIS_MNEMONIC_MOVSD:
        if (in->meta.category == ZYDIS_CATEGORY_STRINGOP) { emit_string(j); return; }
        break;

    /* sign extension */
    case ZYDIS_MNEMONIC_CBW:  { int h = greg(j, XC_RAX); a64_sxtb(&j->a, 0, T0, h); a64_bfi(&j->a, 1, h, T0, 0, 16); gdirty(j, XC_RAX); return; }
    case ZYDIS_MNEMONIC_CWDE: { int h = greg(j, XC_RAX); a64_sxth(&j->a, 0, h, h); gdirty(j, XC_RAX); return; }   /* 32-bit write zero-extends */
    case ZYDIS_MNEMONIC_CDQE: { int h = greg(j, XC_RAX); a64_sxtw(&j->a, h, h); gdirty(j, XC_RAX); return; }
    case ZYDIS_MNEMONIC_CWD:  { int a = greg(j, XC_RAX), d = greg(j, XC_RDX); a64_sxth(&j->a, 0, T0, a); a64_asr_imm(&j->a, 0, T0, T0, 31); a64_bfi(&j->a, 1, d, T0, 0, 16); gdirty(j, XC_RDX); return; }
    case ZYDIS_MNEMONIC_CDQ:  { int a = greg(j, XC_RAX), d = HREG[XC_RDX]; a64_asr_imm(&j->a, 0, d, a, 31); gdirty(j, XC_RDX); return; }
    case ZYDIS_MNEMONIC_CQO:  { int a = greg(j, XC_RAX), d = HREG[XC_RDX]; a64_asr_imm(&j->a, 1, d, a, 63); gdirty(j, XC_RDX); return; }

    /* stack */
    case ZYDIS_MNEMONIC_PUSH: {
        if (ops[0].size != in->stack_width && ops[0].type != XOP_IMM) break;
        int r = ld_op(j, &ops[0], T0, ops[0].type == XOP_IMM);
        if (in->stack_width == 32 && ops[0].type == XOP_IMM) { /* imm is sign-extended; store low 32 */ }
        emit_push_reg(j, r);
        return;
    }
    case ZYDIS_MNEMONIC_POP: {
        if (ops[0].size != in->stack_width) break;
        emit_pop_to(j, T0);
        st_op(j, &ops[0], T0);
        return;
    }
    case ZYDIS_MNEMONIC_LEAVE: {
        int sf = in->stack_width == 64;
        int bp = greg(j, XC_RBP), sp = HREG[XC_RSP];
        a64_mov_reg(&j->a, sf, sp, bp); gdirty(j, XC_RSP);
        emit_pop_to(j, T0);
        a64_mov_reg(&j->a, sf, bp, T0); gdirty(j, XC_RBP);
        return;
    }

    /* control flow */
    case ZYDIS_MNEMONIC_JMP:
        if (ops[0].type == XOP_IMM) { emit_exit_imm(j, ops[0].imm); return; }
        { int r = ld_op(j, &ops[0], T0, 0); if (r != T0) a64_mov_reg(&j->a, 1, T0, r); emit_exit_reg(j, T0); return; }
    case ZYDIS_MNEMONIC_CALL: {
        uint64_t next = j->d->rip + in->length;
        int r;
        if (ops[0].type == XOP_IMM) {                  /* direct call: push the return address, chain to the target */
            a64_mov_imm(&j->a, T0, next);
            emit_push_reg(j, T0);
            emit_exit_imm(j, ops[0].imm);
            return;
        }
        r = ld_op(j, &ops[0], T1, 0); if (r != T1) { a64_mov_reg(&j->a, 1, T1, r); r = T1; }
        a64_mov_imm(&j->a, T0, next);
        emit_push_reg(j, T0);
        emit_exit_reg(j, T1);
        return;
    }
    case ZYDIS_MNEMONIC_RET: {
        emit_pop_to(j, T0);
        if (in->operand_count_visible == 1) {
            int sf = in->stack_width == 64, sp = HREG[XC_RSP];
            uint32_t imm = (uint32_t)ops[0].imm;
            if (imm < 4096) a64_add_imm(&j->a, sf, sp, sp, imm); else { a64_mov_imm(&j->a, T1, imm); a64_add(&j->a, sf, sp, sp, T1); }
        }
        emit_exit_reg(j, T0);
        return;
    }
    default: break;
    }

    if ((cc = x86_cc(m)) >= 0) {
        if (in->meta.category == ZYDIS_CATEGORY_COND_BR) {
            uint64_t target = ops[0].imm, next = j->d->rip + in->length;
            /* fast path: a single native condition */
            emit_cond_to_w0(j, cc);
            flush(j);                                  /* both arms share the register spill */
            uint32_t br = a64_here(&j->a); a64_cbnz(&j->a, 0, T0, 0);
            emit_exit_imm(j, next);
            a64_patch_bcond(&j->a, br, a64_here(&j->a));
            emit_exit_imm(j, target);
            return;
        }
        if (in->operand_count_visible == 1) {                             /* SETcc */
            emit_cond_to_w0(j, cc);
            st_op(j, &ops[0], T0);
            return;
        }
        /* CMOVcc: a 32-bit destination is written even when not taken */
        emit_cond_to_w0(j, cc);
        int s = ld_op(j, &ops[1], T1, 0); if (s != T1) a64_mov_reg(&j->a, 1, T1, s);
        int d = ld_op(j, &ops[0], T2, 0);
        a64_cmp_imm(&j->a, 0, T0, 0);
        a64_csel(&j->a, 1, T1, T1, d, CC_NE);
        j->nz = NZ_NONE;                               /* the cmp clobbered NZCV */
        st_op(j, &ops[0], T1);
        return;
    }

    emit_callout(j);
}

/* --------------------------------------------------------- block level */

static void *compile(xc_cpu *c, block *b) {
    size_t room = g_code_cap - g_code_used;
    if (room < 65536) return 0;                       /* caller flushes and retries */
    const dinsn *insns = xc_cache_insns(b);

    /* liveness of flags after each instruction, backwards from "live at exit" */
    static uint8_t live[MAX_BLOCK];
    int l = 1;
    for (int i = (int)b->count - 1; i >= 0; i--) {
        live[i] = (uint8_t)l;
        int u = flag_use(&insns[i].in, xc_cache_ops(&insns[i]));
        if (u & FR) l = 1; else if (u & FW_ALL) l = 0;
        /* An instruction that can fault reads the flags too. A fault leaves
         * the block for a guest exception handler, which is entitled to the
         * architectural flags -- and those are the ones the *previous*
         * instruction left, because a faulting instruction has no effect.
         * Without this, a flag computation that nothing in the block reads
         * gets dropped, and a handler is handed flags the program never had.
         * Caught by test_faultdiff, which is what it is for. */
        if (insn_can_fault(&insns[i].in, xc_cache_ops(&insns[i]))) l = 1;
    }

    code_write_begin();
    /* Two passes over the same buffer. The first is a probe: it emits the
     * block as before, with greg()/xreg() loading registers where they are
     * first used, and records which ones (`lazy`). The second emits the real
     * code with exactly those loaded up front, in a prologue, and nothing
     * lazily after it -- so a chained predecessor that already holds them can
     * jump past the prologue to `warm` and reload nothing.
     *
     * Asking the operands instead of the emitter would be cheaper, but it
     * answers a different question: it names registers the block mentions,
     * not the ones its lowering actually reads, and hoisting a load the block
     * never needed makes both the cold entry and every link stub longer. That
     * cost real time on nbody; this does not. Compiling twice does not --
     * compilation is a fraction of a percent of the time a hot block spends
     * running. */
    jc j;
    uint32_t livein = 0, warm = 0;
    size_t bytes = 0;
    for (int pass = 0; pass < 2; pass++) {
        memset(&j, 0, sizeof j);
        j.a.buf = (uint32_t *)(g_code_rw + g_code_used); j.a.cap = (uint32_t)(room / 4);
        j.mode = c->mode; j.b = b;
        j.bound = (c->mode == XC_MODE_32 && c->mem->size < (1ull << 32)) ? c->mem->size : 0;
        j.probe = pass == 0;

        g_npsite = 0;
        emit_reg_loads(&j.a, livein);            /* nothing on the probe pass */
        warm = a64_here(&j.a);                   /* chained predecessors enter here */
        j.loaded = (uint16_t)livein; j.xloaded = (uint16_t)(livein >> 16);

        /* step budget: a chained entry skips the dispatcher, so the block itself
         * refuses to start once the budget is gone (rip = its own start, as the
         * dispatcher would have reported) */
        a64_ldr_off(&j.a, 3, T0, R_CPU, OFF(steps));
        a64_cmp_imm(&j.a, 1, T0, 0);
        uint32_t ok = a64_here(&j.a); a64_bcond(&j.a, CC_GT, 0);
        emit_set_rip_imm(&j, b->rip);
        a64_br(&j.a, R_DISP);
        a64_patch_bcond(&j.a, ok, a64_here(&j.a));
        a64_sub_imm(&j.a, 1, T0, T0, b->count);
        a64_str_off(&j.a, 3, T0, R_CPU, OFF(steps));

        x87_block_begin(&j, c);
        uint32_t i;
        for (i = 0; i < b->count; i++) {
            j.d = &insns[i]; j.ops = xc_cache_ops(j.d); j.flags_live = live[i];
            emit_insn(&j);
            if (j.failed) { code_write_end(j.a.buf, 0, RX(j.a.buf)); return 0; }
            if (j.ended) break;
        }
        /* fell off the end (block ended at a decode failure or MAX_BLOCK): continue sequentially */
        if (!j.ended) { const dinsn *last = &insns[b->count - 1]; emit_exit_imm(&j, last->rip + last->in.length); }

        /* after the body, and after its exit: the stubs never fall through */
        emit_fault_stubs(&j);

        if (j.a.overflow) { code_write_end(j.a.buf, 0, RX(j.a.buf)); return 0; }
        if (pass == 0) livein = j.lazy;   /* pass 1 preloads them, so its own lazy set is empty */
        bytes = (size_t)j.a.n * 4;
    }
    register_fault_sites(&j);
    void *rx = RX(j.a.buf);
    code_write_end(j.a.buf, bytes, rx);
    if (getenv("XCORE_JIT_DUMP")) {          /* raw code for `objdump -D -b binary -m aarch64` */
        char name[64]; snprintf(name, sizeof name, "%s/blk_%llx.bin", getenv("XCORE_JIT_DUMP"), (unsigned long long)b->rip);
        FILE *f = fopen(name, "wb"); if (f) { fwrite(j.a.buf, 1, bytes, f); fclose(f); }
    }
    g_code_used += (bytes + 15) & ~(size_t)15;
    g_stat_bytes += bytes;
    g_stat_blocks++;
    b->warm = (uint8_t *)rx + warm * 4;
    b->live_in = livein;
    return rx;
}

/* -------------------------------------------------------- dispatcher */

/* The enter/exit stub is itself generated at init: it saves the callee-saved
 * registers, loads x25/x26/x27, and loops { check stop/steps; x0 = lookup;
 * br x0 } until told to leave. Blocks come back with `br x27`. */
typedef xc_stop (*enter_fn)(xc_cpu *);
static enter_fn g_enter;

/* Build the stub that tops a link up: load the registers the target wants and
 * the predecessor is not already holding, then jump to the target's warm
 * entry. Returns the stub's execute address, or NULL if there is no room --
 * in which case the caller falls back to the cold entry, which loads them
 * all. Stubs are ordinary arena code and go away with the next code reset;
 * an unlinked site simply stops branching to one. */
static void *link_stub(uint32_t need, void *warm) {
    uint32_t words = (uint32_t)reg_count(need) + 1;
    if (g_code_cap - g_code_used < (size_t)words * 4 + 64) return 0;
    uint32_t *rw = (uint32_t *)(g_code_rw + g_code_used);
    uint32_t *rx = (uint32_t *)RX(rw);
    a64 a = { rw, 0, words, 0 };
    code_write_begin();
    emit_reg_loads(&a, need);
    intptr_t off = ((uint8_t *)warm - (uint8_t *)(rx + a.n)) / 4;
    if (a.overflow || off < -(1 << 25) || off >= (1 << 25)) { code_write_end(rw, 0, rx); return 0; }
    a64_b(&a, (int32_t)off);
    code_write_end(rw, (size_t)a.n * 4, rx);
    g_code_used += ((size_t)a.n * 4 + 15) & ~(size_t)15;
    g_stat_bytes += (uint64_t)a.n * 4;
    return rx;
}

/* Patch the `b` at a link site to jump into `b`'s code, and remember it. The
 * word after the site says which guest registers the exiting block left live
 * (see emit_exit_imm); the target's cold entry is always a correct
 * destination, and everything here is about arriving somewhere cheaper. */
static void chain(uint32_t *site_rx, block *b) {
    if (!g_links) g_links = malloc(sizeof(link_rec) * MAX_LINKS);
    if (!g_links || g_nlinks + 1 >= MAX_LINKS) return;
    uint32_t *site_rw = (uint32_t *)((uint8_t *)site_rx - g_code_rx + g_code_rw);
    void *dest = b->code;
    if (b->warm) {
        uint32_t need = b->live_in & ~site_rw[1];
        if (!need) { dest = b->warm; g_stat_link_warm++; }
        else if (need != b->live_in) {
            void *stub = link_stub(need, b->warm);
            if (stub) { dest = stub; g_stat_link_stub++; }
        }
    }
    intptr_t off = ((uint8_t *)dest - (uint8_t *)site_rx) / 4;
    if (off < -(1 << 25) || off >= (1 << 25)) return;
    if (g_nlinks == 0) g_nlinks = 1;
    link_rec *l = &g_links[g_nlinks];
    l->site_rw = site_rw; l->site_rx = site_rx; l->next = b->links;
    b->links = g_nlinks++;
    code_write_begin();
    *site_rw = 0x14000000u | ((uint32_t)off & 0x3FFFFFF);
    code_write_end(site_rw, 4, site_rx);
    g_stat_links++;
}

static int g_trace = -1;
/* `site` is the link site of the block that just exited to a constant
 * target (or NULL): once the target has code, the site is chained to it. */
static void *lookup_compile(xc_cpu *c, uint32_t *site) {
    if (g_trace < 0) g_trace = getenv("XCORE_JIT_TRACE") != 0;
    block *b = xc_cache_lookup(c);
    if (!b) return 0;
    if (g_trace) {
        char dis[128]; xc_disasm(c, c->rip, dis, sizeof dis);
        fprintf(stderr, "[jit] rip=%#llx %s%s\n", (unsigned long long)c->rip, dis, b->code ? "" : "  (compile)");
    }
    if (!b->code) {
        b->code = compile(c, b);
        if (!b->code && g_code_cap - g_code_used < 65536) {
            /* The code arena is full. Only the generated code has to go: the
             * decoded blocks are still valid, and re-decoding them is pure
             * waste. On a device whose blessed arena is a megabyte this runs
             * hundreds of times a minute, so the difference is the run. */
            xc_cache_drop_code();
            site = 0;                                           /* the site's memory went with it */
            if (b) b->code = compile(c, b);
        }
        if (!b || !b->code) { if (c->stop == XC_STOP_NONE) c->stop = XC_STOP_UNDEFINED; return 0; }
    }
    if (site) chain(site, b);
    return b->code;
}

static void build_enter(void) {
    uint32_t *buf = (uint32_t *)g_stub_rw, *xbuf = (uint32_t *)g_stub_rx; a64 a = { buf, 0, 1024, 0 };
    code_write_begin();
    a64_stp_pre(&a, 29, 30, SP, -16);
    a64_stp_pre(&a, 19, 20, SP, -16);
    a64_stp_pre(&a, 21, 22, SP, -16);
    a64_stp_pre(&a, 23, 24, SP, -16);
    a64_stp_pre(&a, 25, 26, SP, -16);
    a64_stp_pre(&a, 27, 28, SP, -16);
    a64_fstp_pre(&a, 8, 9, SP, -16);                              /* d8-d15: the x87 registers while a block runs */
    a64_fstp_pre(&a, 10, 11, SP, -16);
    a64_fstp_pre(&a, 12, 13, SP, -16);
    a64_fstp_pre(&a, 14, 15, SP, -16);
    a64_mov_reg(&a, 1, R_CPU, 0);
    a64_ldr_off(&a, 3, R_BASE, R_CPU, OFF(jit_base));
    uint32_t loop = a64_here(&a);
    a64_movz(&a, 1, 1, 0, 0);                                     /* x1 = no link site (plain `br x27` exits) */
    /* loop+4: link-site exits land here with x1 = the site to patch */
    a64_mov_imm4(&a, R_DISP, (uint64_t)(uintptr_t)(xbuf + loop));  /* blocks return here with br x27 */
    /* stop set? steps exhausted? */
    a64_ldr_off(&a, 2, 0, R_CPU, OFF(stop));
    uint32_t b_stop = a64_here(&a); a64_cbnz(&a, 0, 0, 0);
    a64_ldr_off(&a, 3, 0, R_CPU, OFF(steps));
    a64_cmp_imm(&a, 1, 0, 0);
    uint32_t b_steps = a64_here(&a); a64_bcond(&a, CC_LE, 0);
    a64_mov_reg(&a, 1, 0, R_CPU);                                 /* x1 still holds the site */
    a64_mov_imm(&a, R_TMP, (uint64_t)(uintptr_t)lookup_compile);
    a64_blr(&a, R_TMP);
    uint32_t b_null = a64_here(&a); a64_cbz(&a, 1, 0, 0);
    a64_br(&a, 0);
    /* exits */
    uint32_t x_steps = a64_here(&a);
    a64_patch_bcond(&a, b_steps, x_steps);
    a64_movz(&a, 0, 0, XC_STOP_STEPS, 0);
    a64_str_off(&a, 2, 0, R_CPU, OFF(stop));
    uint32_t x_stop = a64_here(&a);
    a64_patch_bcond(&a, b_stop, x_stop);
    a64_patch_bcond(&a, b_null, x_stop);
    a64_ldr_off(&a, 2, 0, R_CPU, OFF(stop));         /* return value: cpu->stop */
    a64_fldp_post(&a, 14, 15, SP, 16);
    a64_fldp_post(&a, 12, 13, SP, 16);
    a64_fldp_post(&a, 10, 11, SP, 16);
    a64_fldp_post(&a, 8, 9, SP, 16);
    a64_ldp_post(&a, 27, 28, SP, 16);
    a64_ldp_post(&a, 25, 26, SP, 16);
    a64_ldp_post(&a, 23, 24, SP, 16);
    a64_ldp_post(&a, 21, 22, SP, 16);
    a64_ldp_post(&a, 19, 20, SP, 16);
    a64_ldp_post(&a, 29, 30, SP, 16);
    a64_ret(&a);
    code_write_end(buf, a.n * 4, xbuf);
    g_enter = (enter_fn)(uintptr_t)xbuf;
}

#endif /* XC_JIT_HOST */

/* xc_run through the JIT. Returns the stop reason (STEPS when the budget ran out). */
xc_stop xc_run_jit(xc_cpu *c, uint64_t max_steps) {
#if XC_JIT_HOST
    if (!g_code_rw && !code_alloc(64u << 20)) return XC_STOP_UNDEFINED;
    if (!g_enter) { build_enter(); g_callout_stats = getenv("XCORE_JIT_CALLOUTS") != 0; }
    c->steps = (int64_t)(max_steps > INT64_MAX ? INT64_MAX : max_steps);
    c->stop = XC_STOP_NONE;
    c->jit_base = c->mem->mode == XC_MODE_64 ? 0 : (uint64_t)(uintptr_t)c->mem->base;
    g_host_fpcr = rd_fpcr();
    fpcr_from_mxcsr(c);
    wr_fpsr(0);
    x87_refresh(c);                         /* the interpreter may have run since: rebuild the double shadow */
    xc_stop st = g_enter(c);
    fold_fpsr(c);
    x87_materialize(c);
    wr_fpcr(g_host_fpcr);
    xc_flags_sync(c);
    if (st == XC_STOP_STEPS) c->stop = XC_STOP_STEPS;
    return st;
#else
    (void)c; (void)max_steps;
    return XC_STOP_UNDEFINED;
#endif
}

