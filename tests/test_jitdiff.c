/* JIT-vs-interpreter differential test.
 *
 * The golden vectors prove both paths against silicon, but only for the
 * register-only cases the on-device replay can reproduce. This test takes
 * every 64-bit case of the differential suite (tests/difftest/cases_gen.inc,
 * memory operands included), seeds registers, memory and XMM state, runs the
 * snippet once through the interpreter and once through the dynarec, and
 * requires the two final states to be identical. The interpreter is the
 * oracle here; it is itself held to x86 hardware by difftest on the x86 CI
 * runner, so this closes the loop on ARM64 hosts where no x86 is available.
 *
 * Extra seeds inject the values the SSE lowering has to get right but real
 * code rarely produces: NaNs of both kinds and signs, infinities, signed
 * zeros, denormals, and integers at the conversion boundaries. Some seeds
 * also change the MXCSR rounding mode, which the JIT mirrors into FPCR.
 *
 * On hosts without a JIT (x86) the test passes trivially.
 */
#include "xcore/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*setup_fn)(uint64_t g[16], uint64_t *f);
typedef struct {
    const char *name; const uint8_t *code; size_t len; uint64_t flag_mask; setup_fn setup;
    unsigned fsw_mask; int fuzzy; int mode; int x87;
} tcase;

#define ALL  XC_ARITH_FLAGS
#define NO_OF (XC_ARITH_FLAGS & ~XC_OF)
#define CO   (XC_CF | XC_OF)
#define NONE 0
#define SH1  (XC_ARITH_FLAGS & ~XC_AF)
#define SHN  (XC_ARITH_FLAGS & ~(XC_AF | XC_OF))
#define ZF   XC_ZF
#define CZ   (XC_CF | XC_ZF)
#define CF   XC_CF
#define AC   (XC_AF | XC_CF)
#define SZP  (XC_SF | XC_ZF | XC_PF)
#define FSW_ALL 0xFFFF
#define FSW_NOCC 0
#define FSW_TOP 0

static uint8_t g_data[4096] __attribute__((aligned(64)));
static uint8_t g_stack[65536] __attribute__((aligned(64)));
static uint8_t g_code[128] __attribute__((aligned(64)));

static void s_div(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RDX] = 0; g[XC_RCX] |= 1; g[XC_RAX] &= 0xFFFFFFFF; }
static void s_idiv32(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 0x1234567 | 1; g[XC_RAX] &= 0x7FFFFFFF; }
static void s_cl0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] &= ~0xFFull; }
static void s_cl1(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFull) | 1; }
static void s_cl7(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFull) | 7; }
static void s_rcx4(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 4; }
static void s_rcx16(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 16; }
static void s_len(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RAX] = 5; g[XC_RDX] = 9; }   /* PCMPESTR*: short explicit lengths */
static void s_zf(uint64_t g[16], uint64_t *f) { (void)g; *f |= XC_ZF; }
static void s_nzf(uint64_t g[16], uint64_t *f) { (void)g; *f &= ~(uint64_t)XC_ZF; }
static void s_eq(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RBX] = g[XC_RAX]; }
static void s_idx(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 3; }
static void s_eqc(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = g[XC_RAX]; }
static void s_eqc32(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFFFFFFFull) | (g[XC_RAX] & 0xFFFFFFFF); }
static void s_rbx0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RBX] = 0; }
static void s_rcx100(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 100; }
static void s_div32(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RDX] = 0; g[XC_RCX] |= 1; g[XC_RAX] &= 0x7FFFFFFF; }
static void s_div8(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] |= 0x80; g[XC_RAX] &= 0x7FFF; }
static void s_ecx0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 0; }
static void s_eax0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RAX] = 0; }

#define B(...) ((const uint8_t[]){ __VA_ARGS__ })
#define T(nm, mask, setup, ...) { nm, B(__VA_ARGS__), sizeof(B(__VA_ARGS__)), mask, setup, 0, 0, 64, 0 }
#define TX(nm, mask, fsw, fuzzy, setup, ...) { nm, B(__VA_ARGS__), sizeof(B(__VA_ARGS__)), mask, setup, fsw, fuzzy, 64, 1 }
static const tcase CASES[] = {
#include "difftest/cases_gen.inc"
};
#define NCASES (sizeof CASES / sizeof CASES[0])

static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

/* special FP values, as bit patterns */
static const uint64_t SPECIAL64[] = {
    0x7FF8000000000000ull, 0xFFF8000000000000ull,   /* +/- QNaN */
    0x7FF0000000000001ull, 0xFFF0000000000001ull,   /* +/- SNaN */
    0x7FF0000000000000ull, 0xFFF0000000000000ull,   /* +/- inf */
    0x0000000000000000ull, 0x8000000000000000ull,   /* +/- 0 */
    0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,   /* denormals */
    0x41DFFFFFFFC00000ull,                          /* 2147483647.0 */
    0x41E0000000000000ull,                          /* 2^31 */
    0xC1E0000000000000ull,                          /* -2^31 */
    0xC1E0000000200000ull,                          /* -2^31 - 1 */
    0x41DFFFFFFFE00000ull,                          /* 2147483647.5 */
    0x43E0000000000000ull,                          /* 2^63 */
    0x3FF8000000000000ull, 0xBFF8000000000000ull,   /* +/- 1.5 */
    0x4059000000000000ull,                          /* 100.0 */
};
static const uint32_t SPECIAL32[] = {
    0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u, 0x7F800000u, 0xFF800000u,
    0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x4F000000u, 0xCF000000u,
    0x4EFFFFFFu, 0x3FC00000u, 0xBFC00000u, 0x42C80000u, 0x5F000000u,
};

/* f64 -> f80, exact (FLD m64) */
static xc_f80 f80_of_double(double d) {
    uint64_t bits; memcpy(&bits, &d, 8);
    xc_f80 r; uint16_t sign = (uint16_t)((bits >> 63) << 15);
    unsigned e = (unsigned)((bits >> 52) & 0x7FF); uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    if (e == 0) {
        if (!frac) { r.mant = 0; r.se = sign; return r; }
        int sh = 0; while (!(frac >> 52)) { frac <<= 1; sh++; }
        r.mant = frac << 11; r.se = (uint16_t)(sign | (0x3FFF - 1022 - sh)); return r;
    }
    if (e == 0x7FF) { r.mant = (1ull << 63) | (frac << 11); r.se = (uint16_t)(sign | 0x7FFF); return r; }
    r.mant = (1ull << 63) | (frac << 11); r.se = (uint16_t)(sign | (e - 1023 + 0x3FFF));
    return r;
}
/* doubles at the edges the x87 lowering has to hand back to the interpreter */
static const uint64_t X87EDGE[] = {
    0x7FE0000000000000ull, 0xFFE0000000000000ull,   /* +/- 2^1023: products overflow a double */
    0x0010000000000000ull, 0x8010000000000000ull,   /* +/- DBL_MIN */
    0x0020000000000000ull,                          /* 2 * DBL_MIN */
    0x1000000000000000ull,                          /* 2^-767: products underflow a double */
    0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,   /* denormals */
    0x7FF8000000000000ull, 0xFFF0000000000001ull,   /* QNaN, -SNaN */
    0x7FF0000000000000ull,                          /* inf */
    0x43F0000000000000ull, 0xC3E0000000000000ull,   /* 2^64, -2^63: FIST overflow */
    0x41DFFFFFFFC00000ull, 0x41E0000000000000ull,   /* 2^31 - 1, 2^31 */
    0x40D0000000000000ull, 0xC0E0000000000000ull,   /* 16384, -32768 */
    0x0000000000000000ull, 0x8000000000000000ull,   /* +/- 0 */
    0x3FF0000000000000ull, 0x3FF0000000000001ull,   /* 1, 1 + ulp */
};
/* An x87 stack of 3..6 registers, mostly modest doubles, under the Windows
 * control word (PC=53) -- the mode the dynarec lowers natively -- with some
 * seeds in 64-bit precision, other rounding modes, or FPU/SSE modes that
 * disagree, and edge values or 64-bit significands sprinkled in. */
static void seed_x87(xc_cpu *c, uint64_t seed) {
    uint64_t r = seed ^ 0x3C3C3C3C3C3C3C3Cull;
    int n = 3 + (int)((xs(&r) >> 8) % 4);
    c->fcw = (seed % 8 == 7) ? 0x037F : 0x027F;
    if (seed % 16 == 5) { unsigned rc = (unsigned)(seed >> 4) & 3; c->fcw = (uint16_t)((c->fcw & ~0x0C00u) | (rc << 10)); c->mxcsr = 0x1F80 | (rc << 13); }
    if (seed % 32 == 13) c->fcw ^= 0x0400;                          /* FPU and SSE rounding differ */
    c->fsw = (uint16_t)((8 - n) << 11);
    c->ftag_empty = 0xFF;
    for (int i = 0; i < n; i++) {
        int phys = (8 - n + i) & 7;
        c->ftag_empty &= (uint8_t)~(1u << phys);
        double d = (double)(int64_t)(xs(&r) % 40001) / 32.0 - 625.0;
        c->fpr[phys] = f80_of_double(d);
        if (seed % 3 == 1 && (xs(&r) & 1)) { uint64_t b = X87EDGE[xs(&r) % (sizeof X87EDGE / sizeof X87EDGE[0])]; double e; memcpy(&e, &b, 8); c->fpr[phys] = f80_of_double(e); }
        if (seed % 5 == 2 && i == 2) { c->fpr[phys].mant = xs(&r) | (1ull << 63); c->fpr[phys].se = (uint16_t)(0x3FFF + (int)(xs(&r) % 20) - 10); }   /* not a double */
    }
    for (int i = 8 - n; i < 8; i++) { (void)i; }
    /* memory the [rdi] cases read: doubles, ints, edge values */
    for (int i = 0; i < 8; i++) {
        double d = (double)(int64_t)(xs(&r) % 20001) / 16.0 - 625.0;
        if (seed % 3 == 1 && (xs(&r) & 1)) { uint64_t b = X87EDGE[xs(&r) % (sizeof X87EDGE / sizeof X87EDGE[0])]; memcpy(g_data + 8 * i, &b, 8); }
        else if (i & 1) { int64_t v = (int64_t)(xs(&r) % 200001) - 100000; if (seed % 7 == 3) v = (int64_t)xs(&r); memcpy(g_data + 8 * i, &v, 8); }
        else memcpy(g_data + 8 * i, &d, 8);
    }
}

static void seed_all(xc_cpu *c, const tcase *t, uint64_t seed) {
    uint64_t s = seed | 1;
    for (int i = 0; i < 16; i++) c->gpr[i] = xs(&s);
    c->gpr[XC_RSP] = (uint64_t)(uintptr_t)(g_stack + sizeof g_stack - 256);
    c->gpr[XC_RDI] = (uint64_t)(uintptr_t)g_data;
    c->gpr[XC_RSI] = (uint64_t)(uintptr_t)g_data + 64;
    c->rflags = 0x202 | (xs(&s) & XC_ARITH_FLAGS);
    uint64_t x = seed ^ 0xA5A5A5A5A5A5A5A5ull;
    for (int i = 0; i < 8; i++) { c->xmm[i].lo = xs(&x); c->xmm[i].hi = xs(&x); }
    for (int i = 8; i < 16; i++) {
        if (i & 1) {
            double a = (double)(int64_t)(xs(&x) % 20001) / 8.0 - 1250.0;
            double b = (double)(int64_t)(xs(&x) % 20001) / 16.0 - 625.0;
            memcpy(&c->xmm[i].lo, &a, 8); memcpy(&c->xmm[i].hi, &b, 8);
        } else {
            float f[4];
            for (int k = 0; k < 4; k++) f[k] = (float)(int64_t)(xs(&x) % 4001) / 4.0f - 500.0f;
            memcpy(&c->xmm[i], f, 16);
        }
    }
    /* every third seed: sprinkle special values over the registers the
     * cases use as inputs (both the soup and the "modest" ones) */
    if (seed % 3 == 1) {
        for (int i = 0; i < 16; i++) {
            if (xs(&x) & 1) {
                uint64_t *q = (xs(&x) & 1) ? &c->xmm[i].lo : &c->xmm[i].hi;
                *q = SPECIAL64[xs(&x) % (sizeof SPECIAL64 / sizeof SPECIAL64[0])];
            }
            if (xs(&x) & 1) {
                uint32_t w[4]; memcpy(w, &c->xmm[i], 16);
                w[xs(&x) & 3] = SPECIAL32[xs(&x) % (sizeof SPECIAL32 / sizeof SPECIAL32[0])];
                memcpy(&c->xmm[i], w, 16);
            }
        }
        /* and memory, which the [rdi] cases read */
        for (int i = 0; i < 8; i++) {
            uint64_t v = SPECIAL64[xs(&x) % (sizeof SPECIAL64 / sizeof SPECIAL64[0])];
            memcpy(g_data + 8 * i, &v, 8);
        }
    }
    if (seed % 4 == 3) c->mxcsr = 0x1F80 | (uint32_t)((seed >> 2) & 3) << 13;
    if (t->x87) seed_x87(c, seed);
    if (t->setup) t->setup(c->gpr, &c->rflags);
}

typedef struct { uint64_t gpr[16], rflags; xc_u128 xmm[16]; uint32_t mxcsr; xc_f80 fpr[8]; uint16_t fcw, fsw; uint8_t ftag; uint64_t rip; xc_stop st;
                 uint8_t data[256], stack[512]; } result;

static void run(const tcase *t, uint64_t seed, int jit, result *r) {
    /* memory: deterministic fill, then the case-specific seeding */
    uint64_t s = seed * 7 + 3;
    for (size_t i = 0; i < sizeof g_data; i += 8) { uint64_t v = xs(&s); memcpy(g_data + i, &v, 8); }
    for (size_t i = 0; i < sizeof g_stack; i += 8) { uint64_t v = xs(&s); memcpy(g_stack + i, &v, 8); }
    memset(g_code, 0xCC, sizeof g_code);
    memcpy(g_code, t->code, t->len);

    xc_mem mem; xc_mem_init_identity(&mem);
    xc_cpu c; xc_cpu_init(&c, XC_MODE_64, &mem);
    seed_all(&c, t, seed);
    c.rip = (uint64_t)(uintptr_t)g_code;
    xc_jit_enable(jit);
    r->st = xc_run(&c, 1000);
    r->rip = c.rip;
    memcpy(r->gpr, c.gpr, sizeof r->gpr); r->rflags = c.rflags;
    memcpy(r->xmm, c.xmm, sizeof r->xmm); r->mxcsr = c.mxcsr;
    memcpy(r->fpr, c.fpr, sizeof r->fpr); r->fcw = c.fcw; r->fsw = c.fsw; r->ftag = c.ftag_empty;
    memcpy(r->data, g_data, sizeof r->data);
    memcpy(r->stack, g_stack + sizeof g_stack - sizeof r->stack, sizeof r->stack);
}

int main(int argc, char **argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 12;
    if (!xc_jit_available()) { puts("test_jitdiff: no JIT on this host, nothing to compare"); return 0; }
    int bad = 0, runs = 0;
    for (size_t i = 0; i < NCASES; i++) {
        const tcase *t = &CASES[i];
        for (int sd = 0; sd < seeds; sd++) {
            result a, b;
            run(t, (uint64_t)sd * 1000003u + i, 0, &a);
            run(t, (uint64_t)sd * 1000003u + i, 1, &b);
            runs++;
            int fail = 0;
            uint64_t fm = t->flag_mask | ~(uint64_t)XC_ARITH_FLAGS;
            if (a.st != b.st || a.rip != b.rip) { printf("  [%s] seed %d: stop %s@%#llx vs jit %s@%#llx\n", t->name, sd, xc_stop_name(a.st), (unsigned long long)a.rip, xc_stop_name(b.st), (unsigned long long)b.rip); fail = 1; }
            for (int g = 0; g < 16; g++) if (a.gpr[g] != b.gpr[g]) { printf("  [%s] seed %d: gpr%d %#llx vs jit %#llx\n", t->name, sd, g, (unsigned long long)a.gpr[g], (unsigned long long)b.gpr[g]); fail = 1; }
            if ((a.rflags & fm) != (b.rflags & fm)) { printf("  [%s] seed %d: rflags %#llx vs jit %#llx\n", t->name, sd, (unsigned long long)a.rflags, (unsigned long long)b.rflags); fail = 1; }
            for (int x = 0; x < 16; x++) if (a.xmm[x].lo != b.xmm[x].lo || a.xmm[x].hi != b.xmm[x].hi)
                { printf("  [%s] seed %d: xmm%d %016llx%016llx vs jit %016llx%016llx\n", t->name, sd, x, (unsigned long long)a.xmm[x].hi, (unsigned long long)a.xmm[x].lo, (unsigned long long)b.xmm[x].hi, (unsigned long long)b.xmm[x].lo); fail = 1; }
            /* DE (denormal operand) is the one MXCSR bit the native path does not track */
            if ((a.mxcsr & ~2u) != (b.mxcsr & ~2u)) { printf("  [%s] seed %d: mxcsr %#x vs jit %#x\n", t->name, sd, a.mxcsr, b.mxcsr); fail = 1; }
            /* DE (denormal operand) is not tracked by the dynarec, for either unit */
            int x87diff = a.fcw != b.fcw || (a.fsw & ~2u) != (b.fsw & ~2u) || a.ftag != b.ftag;
            for (int k = 0; k < 8; k++) if (a.fpr[k].mant != b.fpr[k].mant || a.fpr[k].se != b.fpr[k].se) x87diff = 1;   /* not memcmp: padding */
            if (x87diff) {
                printf("  [%s] seed %d: x87 state differs (fsw %#x/%#x ftag %#x/%#x)\n", t->name, sd, a.fsw, b.fsw, a.ftag, b.ftag);
                for (int k = 0; k < 8; k++) if (a.fpr[k].mant != b.fpr[k].mant || a.fpr[k].se != b.fpr[k].se)
                    printf("      R%d %04x.%016llx vs jit %04x.%016llx\n", k, a.fpr[k].se, (unsigned long long)a.fpr[k].mant, b.fpr[k].se, (unsigned long long)b.fpr[k].mant);
                fail = 1;
            }
            if (memcmp(a.data, b.data, sizeof a.data)) { printf("  [%s] seed %d: data memory differs\n", t->name, sd); fail = 1; }
            if (memcmp(a.stack, b.stack, sizeof a.stack)) { printf("  [%s] seed %d: stack memory differs\n", t->name, sd); fail = 1; }
            bad += fail;
        }
    }
    uint64_t blocks, callouts, bytes; xc_jit_stats(&blocks, &callouts, &bytes);
    printf("test_jitdiff: %zu cases x %d seeds = %d runs, %d failed; jit compiled %llu blocks, %llu callouts\n",
           NCASES, seeds, runs, bad, (unsigned long long)blocks, (unsigned long long)callouts);
    return bad ? 1 : 0;
}
