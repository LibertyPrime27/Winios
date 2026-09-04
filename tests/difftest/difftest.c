/* Differential test: run each snippet on the real CPU and in xcore, compare.
 *
 * The CI runner is x86-64, so the CPU it runs on is the specification. Every
 * instruction the interpreter implements gets a case here; a wrong flag or a
 * missed zero-extension shows up as a diff against silicon, not as a game
 * crashing three layers up.
 *
 * Linux x86-64 only. On other hosts this target is not built.
 */
#define _GNU_SOURCE
#include "xcore/cpu.h"

#include <Zydis/Zydis.h>
#include "softfloat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Does this snippet reference memory at all -- read, write, or via the stack?
 *
 * Detected by decoding rather than by observing writes: a load like
 * `mov rax,[rdi+8]` changes no memory and moves no stack pointer, yet its
 * recorded RDI is a host address that means nothing on another machine. An
 * earlier write-based check missed exactly those and the replay faulted.
 * Hidden operands are included, which is what catches PUSH/POP/CALL/RET. */
static int snippet_touches_memory(const uint8_t *code, size_t len) {
    ZydisDecoder d;
    ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t off = 0;
    while (off < len) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d, code + off, len - off, &in, ops)))
            return 1;                     /* undecodable: assume the worst */
        for (int i = 0; i < in.operand_count; i++)
            if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) return 1;
        off += in.length;
    }
    return 0;
}

typedef struct {
    uint64_t gpr[16];
    uint64_t rflags;
    uint64_t code;
    uint64_t saved_rsp;
    uint8_t  pad[8];
    uint8_t  fx[512];        /* FXSAVE image at offset 160: x87, MXCSR, XMM */
} __attribute__((aligned(16))) nstate;

/* FXSAVE layout */
enum { FX_FCW = 0, FX_FSW = 2, FX_FTW = 4, FX_MXCSR = 24, FX_MXCSR_MASK = 28, FX_ST = 32, FX_XMM = 160 };

/* The x87 state a case starts from and ends with, in ST order like FXSAVE. */
typedef struct {
    uint16_t fcw, fsw;
    uint8_t  ftw;            /* abridged: bit i = physical register i valid */
    xc_f80   st[8];          /* ST(0)..ST(7); only tagged-valid ones matter */
} x87state;

static int x87_top_of(uint16_t fsw) { return (fsw >> 11) & 7; }

static void x87_to_fx(const x87state *s, uint8_t *fx) {
    memcpy(fx + FX_FCW, &s->fcw, 2); memcpy(fx + FX_FSW, &s->fsw, 2); fx[FX_FTW] = s->ftw;
    for (int i = 0; i < 8; i++) { memcpy(fx + FX_ST + 16 * i, &s->st[i].mant, 8); memcpy(fx + FX_ST + 16 * i + 8, &s->st[i].se, 2); }
}
static void x87_from_fx(const uint8_t *fx, x87state *s) {
    memcpy(&s->fcw, fx + FX_FCW, 2); memcpy(&s->fsw, fx + FX_FSW, 2); s->ftw = fx[FX_FTW];
    for (int i = 0; i < 8; i++) { memcpy(&s->st[i].mant, fx + FX_ST + 16 * i, 8); memcpy(&s->st[i].se, fx + FX_ST + 16 * i + 8, 2); }
}
static void x87_to_cpu(const x87state *s, xc_cpu *c) {
    c->fcw = s->fcw; c->fsw = s->fsw; c->ftag_empty = (uint8_t)~s->ftw;
    int top = x87_top_of(s->fsw);
    for (int i = 0; i < 8; i++) c->fpr[(top + i) & 7] = s->st[i];
}
static void x87_from_cpu(const xc_cpu *c, x87state *s) {
    s->fcw = c->fcw; s->fsw = c->fsw; s->ftw = (uint8_t)~c->ftag_empty;
    int top = x87_top_of(c->fsw);
    for (int i = 0; i < 8; i++) s->st[i] = c->fpr[(top + i) & 7];
}
static int st_valid(const x87state *s, int i) { return (s->ftw >> ((x87_top_of(s->fsw) + i) & 7)) & 1; }

/* For the transcendental group: equal to within a few units of double precision. */
static int f80_close(xc_f80 a, xc_f80 b) {
    if (a.mant == b.mant && a.se == b.se) return 1;
    long double x, y; memset(&x, 0, sizeof x); memset(&y, 0, sizeof y);
    memcpy(&x, &a, 10); memcpy(&y, &b, 10);
    if (x != x || y != y) return (x != x) && (y != y);
    long double d = x - y; if (d < 0) d = -d;
    long double m = x < 0 ? -x : x; if (m < 1e-300L) m = 1e-300L;
    return d / m < 1e-15L;
}

extern void native_run(nstate *s);

enum { STACK_SZ = 64 * 1024, DATA_SZ = 4096, CODE_SZ = 4096 };
static uint8_t *g_code, *g_stack, *g_data;
static const uint64_t SENTINEL = 0x5E17E0000ull;      /* unmapped; emulator stops here */

typedef void (*setup_fn)(uint64_t gpr[16], uint64_t *flags);

/* Does the snippet name an XMM register? Such cases record the XMM file. */
static int snippet_uses_xmm(const uint8_t *code, size_t len) {
    ZydisDecoder d;
    ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t off = 0;
    while (off < len) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d, code + off, len - off, &in, ops))) return 1;
        for (int i = 0; i < in.operand_count; i++)
            if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ZydisRegisterGetClass(ops[i].reg.value) == ZYDIS_REGCLASS_XMM) return 1;
        if (in.mnemonic == ZYDIS_MNEMONIC_LDMXCSR || in.mnemonic == ZYDIS_MNEMONIC_STMXCSR) return 1;
        off += in.length;
    }
    return 0;
}

typedef struct {
    const char    *name;
    const uint8_t *code;
    size_t         len;
    uint64_t       flag_mask;   /* arithmetic flags that are architecturally defined afterwards */
    setup_fn       setup;
    unsigned       fsw_mask;    /* x87 status bits compared; 0 = case is not x87 */
    int            fuzzy;       /* transcendental: compare ST values with tolerance */
} tcase;

/* deterministic register soup so preserved-flag and upper-bit bugs surface */
static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static void seed_regs(uint64_t gpr[16], uint64_t *flags, uint64_t seed, int needs_mem) {
    uint64_t s = seed | 1;
    for (int i = 0; i < 16; i++) gpr[i] = xs(&s);
    gpr[XC_RSP] = (uint64_t)(g_stack + STACK_SZ - 256);
    /* Only point RDI/RSI at the data buffer when the snippet dereferences
     * them. For everything else they are ordinary values, and using host
     * pointers would bake this machine's ASLR layout into the recording. */
    if (needs_mem) {
        gpr[XC_RDI] = (uint64_t)g_data;
        gpr[XC_RSI] = (uint64_t)g_data + 64;
    }
    *flags = 0x202 | (xs(&s) & XC_ARITH_FLAGS);
}

/* A partly filled x87 stack: 3..6 valid registers holding modest doubles,
 * ST(2) occasionally a full 64-bit-significand value so precision control has
 * something to round. Everything else empty. */
static void seed_x87(x87state *s, uint64_t seed) {
    uint64_t r = seed ^ 0x3C3C3C3C3C3C3C3Cull;
    int n = 3 + (int)((xs(&r) >> 8) % 4);
    memset(s, 0, sizeof *s);
    s->fcw = 0x037F;
    s->fsw = (uint16_t)((8 - n) << 11);
    for (int i = 0; i < n; i++) {
        int phys = (8 - n + i) & 7;
        s->ftw |= (uint8_t)(1u << phys);
        double d = (double)(int64_t)(xs(&r) % 40001) / 32.0 - 625.0;
        float64_t f; memcpy(&f, &d, 8);
        extFloat80_t e = f64_to_extF80(f);
        memcpy(&s->st[i], &e, 10);
        if (i == 2 && (seed & 0x100)) { s->st[i].mant = xs(&r) | (1ull << 63); s->st[i].se = (uint16_t)(0x3FFF + (int)(xs(&r) % 20) - 10); }
    }
}

/* xmm0-7: bit soup (so lane boundaries, sign bits and the odd NaN get
 * exercised); xmm8-15: modest doubles in both halves and modest floats in
 * all four lanes, so arithmetic cases produce exact, meaningful results. */
static void seed_xmm(xc_u128 xmm[16], uint64_t seed) {
    uint64_t s = seed ^ 0xA5A5A5A5A5A5A5A5ull;
    for (int i = 0; i < 8; i++) { xmm[i].lo = xs(&s); xmm[i].hi = xs(&s); }
    for (int i = 8; i < 16; i++) {
        if (i & 1) {
            double a = (double)(int64_t)(xs(&s) % 20001) / 8.0 - 1250.0;
            double b = (double)(int64_t)(xs(&s) % 20001) / 16.0 - 625.0;
            memcpy(&xmm[i].lo, &a, 8); memcpy(&xmm[i].hi, &b, 8);
        } else {
            float f[4];
            for (int k = 0; k < 4; k++) f[k] = (float)(int64_t)(xs(&s) % 4001) / 4.0f - 500.0f;
            memcpy(&xmm[i], f, 16);
        }
    }
}

static FILE *g_golden;          /* non-NULL when emitting golden vectors */

static int run_case(const tcase *t, uint64_t seed) {
    /* code: snippet + ret */
    memset(g_code, 0xCC, CODE_SZ);
    memcpy(g_code, t->code, t->len);
    g_code[t->len] = 0xC3;

    const int needs_mem = snippet_touches_memory(t->code, t->len);
    const int uses_xmm = snippet_uses_xmm(t->code, t->len);
    uint64_t gpr[16], flags;
    xc_u128 xmm[16];
    x87state x87in;
    seed_regs(gpr, &flags, seed, needs_mem);
    seed_xmm(xmm, seed);
    seed_x87(&x87in, seed);
    if (t->setup) t->setup(gpr, &flags);

    /* fill data + stack deterministically, snapshot */
    uint64_t s = seed * 7 + 3;
    for (size_t i = 0; i < DATA_SZ; i += 8) { uint64_t v = xs(&s); memcpy(g_data + i, &v, 8); }
    for (size_t i = 0; i < STACK_SZ; i += 8) { uint64_t v = xs(&s); memcpy(g_stack + i, &v, 8); }
    uint8_t *snap_data = malloc(DATA_SZ), *snap_stack = malloc(STACK_SZ);
    memcpy(snap_data, g_data, DATA_SZ); memcpy(snap_stack, g_stack, STACK_SZ);

    /* --- native --- */
    nstate n; memset(&n, 0, sizeof n);
    memcpy(n.gpr, gpr, sizeof gpr); n.rflags = flags; n.code = (uint64_t)g_code;
    x87_to_fx(&x87in, n.fx);
    { uint32_t m = 0x1F80, mm = 0xFFFF; memcpy(n.fx + FX_MXCSR, &m, 4); memcpy(n.fx + FX_MXCSR_MASK, &mm, 4); }
    memcpy(n.fx + FX_XMM, xmm, sizeof xmm);
    native_run(&n);
    x87state x87nat; x87_from_fx(n.fx, &x87nat);
    xc_u128 xmmnat[16]; memcpy(xmmnat, n.fx + FX_XMM, sizeof xmmnat);
    uint32_t mxcsrnat; memcpy(&mxcsrnat, n.fx + FX_MXCSR, 4);
    uint8_t *nat_data = malloc(DATA_SZ), *nat_stack = malloc(STACK_SZ);
    memcpy(nat_data, g_data, DATA_SZ); memcpy(nat_stack, g_stack, STACK_SZ);

    /* --- emulated --- */
    memcpy(g_data, snap_data, DATA_SZ); memcpy(g_stack, snap_stack, STACK_SZ);
    xc_mem mem; xc_mem_init_identity(&mem);
    xc_cpu c; xc_cpu_init(&c, XC_MODE_64, &mem);
    memcpy(c.gpr, gpr, sizeof gpr);
    memcpy(c.xmm, xmm, sizeof xmm);
    x87_to_cpu(&x87in, &c);
    c.rflags = flags;
    c.rip = (uint64_t)g_code;
    /* the native call pushed a return address; emulate that slot with a sentinel */
    c.gpr[XC_RSP] -= 8;
    memcpy((void *)c.gpr[XC_RSP], &SENTINEL, 8);
    /* native's return address lives in that slot too; exclude it from the compare */
    size_t ret_slot = (size_t)(c.gpr[XC_RSP] - (uint64_t)g_stack);

    int steps = 0; xc_stop st = XC_STOP_NONE;
    while (c.rip != SENTINEL && steps++ < 100000) {
        st = xc_step(&c);
        if (st != XC_STOP_NONE) break;
    }

    int bad = 0;
    if (c.rip != SENTINEL) {
        printf("  [%s] emulator stopped: %s at rip=%#llx (fault_addr=%#llx)\n",
               t->name, xc_stop_name(st), (unsigned long long)c.rip, (unsigned long long)c.fault_addr);
        bad = 1;
    }
    static const char *rn[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                 "r8","r9","r10","r11","r12","r13","r14","r15"};
    for (int i = 0; i < 16; i++) if (n.gpr[i] != c.gpr[i]) {
        printf("  [%s] %s: native %#018llx  emu %#018llx\n", t->name, rn[i],
               (unsigned long long)n.gpr[i], (unsigned long long)c.gpr[i]);
        bad = 1;
    }
    for (int i = 0; i < 16; i++) if (xmmnat[i].lo != c.xmm[i].lo || xmmnat[i].hi != c.xmm[i].hi) {
        printf("  [%s] xmm%d: native %016llx_%016llx  emu %016llx_%016llx\n", t->name, i,
               (unsigned long long)xmmnat[i].hi, (unsigned long long)xmmnat[i].lo,
               (unsigned long long)c.xmm[i].hi, (unsigned long long)c.xmm[i].lo);
        bad = 1;
    }
    if (mxcsrnat != c.mxcsr) {
        printf("  [%s] mxcsr: native %#x  emu %#x\n", t->name, mxcsrnat, c.mxcsr);
        bad = 1;
    }
    x87state x87emu; x87_from_cpu(&c, &x87emu);
    if (t->fsw_mask && getenv("XDBG")) {
        printf("  [%s] seed %llx in: fcw %x fsw %x ftw %x", t->name, (unsigned long long)seed, x87in.fcw, x87in.fsw, x87in.ftw);
        for (int i = 0; i < 8; i++) if (st_valid(&x87in, i)) printf(" st%d=%04x_%016llx", i, x87in.st[i].se, (unsigned long long)x87in.st[i].mant);
        printf("\n");
    }
    if (t->fsw_mask) {
        if (x87nat.fcw != x87emu.fcw) { printf("  [%s] fcw: native %#x  emu %#x\n", t->name, x87nat.fcw, x87emu.fcw); bad = 1; }
        if ((x87nat.fsw & t->fsw_mask) != (x87emu.fsw & t->fsw_mask)) {
            printf("  [%s] fsw: native %#06x  emu %#06x  (mask %#x)\n", t->name, x87nat.fsw, x87emu.fsw, t->fsw_mask); bad = 1;
        }
        if (x87nat.ftw != x87emu.ftw) { printf("  [%s] ftw: native %#04x  emu %#04x\n", t->name, x87nat.ftw, x87emu.ftw); bad = 1; }
        else for (int i = 0; i < 8; i++) if (st_valid(&x87nat, i)) {
            int same = t->fuzzy ? f80_close(x87nat.st[i], x87emu.st[i])
                                : (x87nat.st[i].mant == x87emu.st[i].mant && x87nat.st[i].se == x87emu.st[i].se);
            if (!same) {
                printf("  [%s] st(%d): native %04x_%016llx  emu %04x_%016llx\n", t->name, i,
                       x87nat.st[i].se, (unsigned long long)x87nat.st[i].mant, x87emu.st[i].se, (unsigned long long)x87emu.st[i].mant);
                bad = 1;
            }
        }
    }
    uint64_t fm = t->flag_mask;
    if ((n.rflags & fm) != (c.rflags & fm)) {
        printf("  [%s] flags: native %#06llx  emu %#06llx  (mask %#06llx)\n", t->name,
               (unsigned long long)(n.rflags & XC_ARITH_FLAGS),
               (unsigned long long)(c.rflags & XC_ARITH_FLAGS), (unsigned long long)fm);
        bad = 1;
    }
    if (memcmp(nat_data, g_data, DATA_SZ)) {
        printf("  [%s] data buffer differs\n", t->name); bad = 1;
        if (getenv("XDBG")) for (size_t i = 0; i < DATA_SZ; i += 8) if (memcmp(nat_data + i, g_data + i, 8)) {
            uint64_t a, b; memcpy(&a, nat_data + i, 8); memcpy(&b, g_data + i, 8);
            printf("    +%zu: native %016llx emu %016llx\n", i, (unsigned long long)a, (unsigned long long)b);
        }
    }
    memcpy(nat_stack + ret_slot, g_stack + ret_slot, 8);   /* ignore the return-address slot */
    if (memcmp(nat_stack, g_stack, STACK_SZ)) { printf("  [%s] stack differs\n", t->name); bad = 1; }

    /* Emit a golden vector: the native (silicon) post-state, so the same case
     * can be replayed on ARM64 where no native oracle exists.
     *
     * Whether the case touched memory is recorded here, where it is known for
     * certain, rather than inferred at replay time from register values. */
    if (g_golden) {
        const int touched = needs_mem;
        /* Memory cases are skipped on replay, and their registers hold host
         * addresses, so their state is recorded as zero. Non-memory cases
         * record everything except RSP, which the trampoline owns. Both rules
         * exist so the file is byte-identical on any machine -- otherwise CI
         * cannot check it for staleness. */
        fprintf(g_golden, "  { \"%s\", 0x%llxull, { ", t->name, (unsigned long long)seed);
        for (int i = 0; i < 16; i++)
            fprintf(g_golden, "0x%llxull,", (unsigned long long)(touched || i == XC_RSP ? 0 : gpr[i]));
        fprintf(g_golden, " }, 0x%llxull, { ",
                (unsigned long long)(touched ? 0 : (flags & XC_ARITH_FLAGS)));
        for (int i = 0; i < 16; i++)
            fprintf(g_golden, "0x%llxull,", (unsigned long long)(touched || i == XC_RSP ? 0 : n.gpr[i]));
        /* Record only the bits this case declares defined. Masking to
         * XC_ARITH_FLAGS was not enough: it kept architecturally-undefined
         * bits such as AF after a shift, which real CPUs genuinely differ on,
         * so the file still varied between runners. Anything outside
         * flag_mask is not compared at replay either, so recording it can only
         * cause false mismatches. */
        fprintf(g_golden, " }, 0x%llxull, 0x%llxull, %d,\n    (const uint8_t[]){",
                (unsigned long long)(touched ? 0 : (n.rflags & t->flag_mask)),
                (unsigned long long)t->flag_mask, touched);
        for (size_t i = 0; i < t->len; i++) fprintf(g_golden, "0x%02x,", t->code[i]);
        fprintf(g_golden, "}, %zu,\n    ", t->len);
        /* XMM state only for cases that name an XMM register (and never for
         * memory cases, which are not replayed). */
        if (uses_xmm && !touched) {
            fprintf(g_golden, "(const uint64_t[]){");
            for (int i = 0; i < 16; i++) fprintf(g_golden, "0x%llxull,0x%llxull,", (unsigned long long)xmm[i].lo, (unsigned long long)xmm[i].hi);
            fprintf(g_golden, "},\n    (const uint64_t[]){");
            for (int i = 0; i < 16; i++) fprintf(g_golden, "0x%llxull,0x%llxull,", (unsigned long long)xmmnat[i].lo, (unsigned long long)xmmnat[i].hi);
            fprintf(g_golden, "},\n    ");
        } else fprintf(g_golden, "0, 0,\n    ");
        if (t->fsw_mask && !touched) {
            fprintf(g_golden, "(const uint64_t[]){%u,%u,%u,", x87in.fcw, x87in.fsw, x87in.ftw);
            for (int i = 0; i < 8; i++) fprintf(g_golden, "0x%llxull,%u,", (unsigned long long)x87in.st[i].mant, x87in.st[i].se);
            /* Transcendentals: record what *this* code produced, not silicon.
             * Intel and AMD differ in the last bits of FSIN and friends, so a
             * silicon recording would make the file depend on the runner;
             * difftest has already checked the emulator against silicon with
             * a tolerance, and the replay checks ARM64 against x86 hosts. */
            const x87state *rec = t->fuzzy ? &x87emu : &x87nat;
            fprintf(g_golden, "},\n    (const uint64_t[]){%u,%u,%u,", rec->fcw, rec->fsw, rec->ftw);
            for (int i = 0; i < 8; i++) {
                uint64_t mant = rec->st[i].mant; unsigned se = rec->st[i].se;
                if (t->fuzzy && st_valid(rec, i) && (se & 0x7FFF) != 0x7FFF) {
                    /* Even our own transcendental results differ in the last
                     * bit between libm versions; keep 32 significant bits so
                     * the file is stable and the replay compares loosely. */
                    uint64_t r = mant + 0x80000000ull;
                    if (r < mant) { r = 1ull << 63; se++; }
                    mant = r & ~0xFFFFFFFFull;
                }
                fprintf(g_golden, "0x%llxull,%u,", (unsigned long long)mant, se);
            }
            fprintf(g_golden, "}, %#x, %d },\n", t->fsw_mask, t->fuzzy);
        } else fprintf(g_golden, "0, 0, 0, 0 },\n");
    }

    free(snap_data); free(snap_stack); free(nat_data); free(nat_stack);
    return bad;
}

/* ------------------------------------------------------------------ cases */

#define ALL  XC_ARITH_FLAGS
#define NO_OF (XC_ARITH_FLAGS & ~XC_OF)
#define CO   (XC_CF | XC_OF)                       /* MUL/IMUL: only CF/OF defined */
#define NONE 0
/* Shifts: the SDM leaves AF undefined, and real CPUs disagree about it --
 * this was caught by two CI runners producing different recordings. OF is
 * defined only for a count of exactly 1. */
#define SH1  (XC_ARITH_FLAGS & ~XC_AF)
#define SHN  (XC_ARITH_FLAGS & ~(XC_AF | XC_OF))
#define ZF   XC_ZF                                 /* BSF/BSR: only ZF defined */
#define CZ   (XC_CF | XC_ZF)                       /* TZCNT/LZCNT */
#define CF   XC_CF                                 /* BT family */

static void s_div(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RDX] = 0; g[XC_RCX] |= 1; g[XC_RAX] &= 0xFFFFFFFF; }
static void s_idiv32(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 0x1234567 | 1; g[XC_RAX] &= 0x7FFFFFFF; }
static void s_cl0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] &= ~0xFFull; }
static void s_cl1(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFull) | 1; }
static void s_cl7(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFull) | 7; }
static void s_rcx4(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 4; }
static void s_rcx16(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 16; }
static void s_zf(uint64_t g[16], uint64_t *f) { (void)g; *f |= XC_ZF; }
static void s_nzf(uint64_t g[16], uint64_t *f) { (void)g; *f &= ~(uint64_t)XC_ZF; }
static void s_eq(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RBX] = g[XC_RAX]; }
static void s_idx(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 3; }
static void s_eqc(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = g[XC_RAX]; }
static void s_eqc32(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFFFFFFFull) | (g[XC_RAX] & 0xFFFFFFFF); }
static void s_rbx0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RBX] = 0; }
static void s_rcx100(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 100; }

/* x87 status word: C1 is masked everywhere (it reports round-up, which the
 * software FPU does not track); C0/C2/C3 are undefined after arithmetic. */
#define FSW_ALL  0xFDFFu                 /* everything but C1 */
#define FSW_NOCC (FSW_ALL & ~0x4500u)    /* also drop C0, C2, C3 */
#define FSW_TOP  0x3800u                 /* transcendentals: stack shape only */

#define B(...) ((const uint8_t[]){__VA_ARGS__})
#define T(nm, mask, setup, ...) { nm, B(__VA_ARGS__), sizeof(B(__VA_ARGS__)), mask, setup, 0, 0 }
#define TX(nm, mask, fsw, fuzzy, setup, ...) { nm, B(__VA_ARGS__), sizeof(B(__VA_ARGS__)), mask, setup, fsw, fuzzy }

static const tcase cases[] = {
    /* moves */
    T("mov rax,rbx",            ALL, 0, 0x48,0x89,0xD8),
    T("mov eax,imm32",          ALL, 0, 0xB8,0x78,0x56,0x34,0x12),
    T("mov rax,imm64",          ALL, 0, 0x48,0xB8,0xEF,0xCD,0xAB,0x89,0x67,0x45,0x23,0x01),
    T("mov ah,imm8",            ALL, 0, 0xB4,0x7F),
    T("mov bh,imm8",            ALL, 0, 0xB7,0x33),
    T("mov r8d,imm32",          ALL, 0, 0x41,0xB8,0x05,0x00,0x00,0x00),
    T("mov [rdi],rax",          ALL, 0, 0x48,0x89,0x07),
    T("mov rax,[rdi+8]",        ALL, 0, 0x48,0x8B,0x47,0x08),
    T("mov [rdi+rcx*4],edx",    ALL, s_idx, 0x89,0x14,0x8F),
    T("movzx eax,byte[rdi]",    ALL, 0, 0x0F,0xB6,0x07),
    T("movsx rax,word[rdi+2]",  ALL, 0, 0x48,0x0F,0xBF,0x47,0x02),
    T("movsxd rax,ecx",         ALL, 0, 0x48,0x63,0xC1),
    T("lea rax,[rdi+rcx*8+16]", ALL, 0, 0x48,0x8D,0x44,0xCF,0x10),
    T("xchg rax,rbx",           ALL, 0, 0x48,0x87,0xD8),

    /* alu */
    T("add rax,rbx",            ALL, 0, 0x48,0x01,0xD8),
    T("add al,5",               ALL, 0, 0x04,0x05),
    T("add ah,bh",              ALL, 0, 0x00,0xFC),
    T("add r8b,cl",             ALL, 0, 0x41,0x00,0xC8),
    T("sub ecx,edx",            ALL, 0, 0x29,0xD1),
    T("sub sil,dil",            ALL, 0, 0x40,0x28,0xFE),
    T("adc rax,rcx",            ALL, 0, 0x48,0x11,0xC8),
    T("sbb r8,r9",              ALL, 0, 0x4D,0x19,0xC8),
    T("and rax,0x0F0F",         ALL, 0, 0x48,0x25,0x0F,0x0F,0x00,0x00),
    T("or edx,0x80000000",      ALL, 0, 0x81,0xCA,0x00,0x00,0x00,0x80),
    T("xor r10,r11",            ALL, 0, 0x4D,0x31,0xDA),
    T("xor eax,eax",            ALL, 0, 0x31,0xC0),
    T("cmp rax,rbx",            ALL, 0, 0x48,0x39,0xD8),
    T("cmp rax,rbx (equal)",    ALL, s_eq, 0x48,0x39,0xD8),
    T("test ecx,edx",           ALL, 0, 0x85,0xD1),
    T("inc rax",                ALL, 0, 0x48,0xFF,0xC0),
    T("dec ecx",                ALL, 0, 0xFF,0xC9),
    T("neg rdx",                ALL, 0, 0x48,0xF7,0xDA),
    T("not r9",                 ALL, 0, 0x49,0xF7,0xD1),
    T("add [rdi],rax",          ALL, 0, 0x48,0x01,0x07),
    T("add dword[rdi+4],7",     ALL, 0, 0x83,0x47,0x04,0x07),

    /* shifts -- OF is defined only for a count of 1 */
    T("shl rax,1",              SH1,   0, 0x48,0xD1,0xE0),
    T("shl rax,3",              SHN,   0, 0x48,0xC1,0xE0,0x03),
    T("shr ecx,5",              SHN,   0, 0xC1,0xE9,0x05),
    T("sar rdx,7",              SHN,   0, 0x48,0xC1,0xFA,0x07),
    T("shl rax,cl (cl=0)",      ALL,   s_cl0, 0x48,0xD3,0xE0),
    T("shl rax,cl (cl=1)",      SH1,   s_cl1, 0x48,0xD3,0xE0),
    T("shr rax,cl (cl=7)",      SHN,   s_cl7, 0x48,0xD3,0xE8),
    T("rol eax,9",              NO_OF, 0, 0xC1,0xC0,0x09),
    T("ror rcx,13",             NO_OF, 0, 0x48,0xC1,0xC9,0x0D),
    T("rol rax,1",              ALL,   0, 0x48,0xD1,0xC0),

    /* multiply / divide */
    T("imul rax,rbx",           CO, 0, 0x48,0x0F,0xAF,0xC3),
    T("imul ecx,edx,100",       CO, 0, 0x6B,0xCA,0x64),
    T("imul rbx (1-op)",        CO, 0, 0x48,0xF7,0xEB),
    T("mul rcx",                CO, 0, 0x48,0xF7,0xE1),
    T("mul ecx",                CO, 0, 0xF7,0xE1),
    T("div rcx",                NONE, s_div, 0x48,0xF7,0xF1),
    T("cdq; idiv ecx",          NONE, s_idiv32, 0x99,0xF7,0xF9),

    /* sign extension */
    T("cbw",  ALL, 0, 0x66,0x98),
    T("cwde", ALL, 0, 0x98),
    T("cdqe", ALL, 0, 0x48,0x98),
    T("cdq",  ALL, 0, 0x99),
    T("cqo",  ALL, 0, 0x48,0x99),

    /* stack */
    T("push rbx; pop rcx",      ALL, 0, 0x53,0x59),
    T("push imm8; pop rax",     ALL, 0, 0x6A,0x12,0x58),
    T("push imm32(neg); pop rdx", ALL, 0, 0x68,0x00,0x00,0x00,0x80,0x5A),
    T("push rbp;mov rbp,rsp;sub rsp,32;leave", ALL, 0, 0x55,0x48,0x89,0xE5,0x48,0x83,0xEC,0x20,0xC9),

    /* conditionals */
    T("cmovz eax,ecx (ZF)",     ALL, s_zf,  0x0F,0x44,0xC1),
    T("cmovz eax,ecx (!ZF)",    ALL, s_nzf, 0x0F,0x44,0xC1),
    T("cmovnz rax,rcx",         ALL, 0,     0x48,0x0F,0x45,0xC1),
    T("setb al",                ALL, 0, 0x0F,0x92,0xC0),
    T("setnle dl",              ALL, 0, 0x0F,0x9F,0xC2),
    T("cmp;jz;mov;jmp;mov",     ALL, 0, 0x48,0x39,0xD8, 0x74,0x07, 0xB9,0x01,0x00,0x00,0x00, 0xEB,0x05, 0xB9,0x02,0x00,0x00,0x00),
    T("cmp;jz (taken)",         ALL, s_eq, 0x48,0x39,0xD8, 0x74,0x07, 0xB9,0x01,0x00,0x00,0x00, 0xEB,0x05, 0xB9,0x02,0x00,0x00,0x00),
    T("call/ret",               ALL, 0, 0xE8,0x06,0x00,0x00,0x00, 0x48,0x83,0xC0,0x01, 0xEB,0x05, 0x48,0x83,0xC1,0x07, 0xC3),

    /* strings */
    T("rep stosq",              ALL, s_rcx4,  0xF3,0x48,0xAB),
    T("rep movsb",              ALL, s_rcx16, 0xF3,0xA4),
    T("stosd",                  ALL, 0,       0xAB),

    /* everything below is assembled by cases_gen.py */
#include "cases_gen.inc"
};

int main(int argc, char **argv) {
    const char *emit = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--emit-golden") && i + 1 < argc) emit = argv[++i];

    g_code  = mmap(0, CODE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_stack = mmap(0, STACK_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_data  = mmap(0, DATA_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_code == MAP_FAILED || g_stack == MAP_FAILED || g_data == MAP_FAILED) {
        perror("mmap"); return 2;
    }

    if (emit) {
        g_golden = fopen(emit, "w");
        if (!g_golden) { perror("fopen"); return 2; }
        fprintf(g_golden,
            "/* GENERATED by difftest --emit-golden. Do not edit.\n"
            " *\n"
            " * Each entry is a snippet plus the post-state a real x86-64 CPU produced\n"
            " * for it. On ARM64 there is no native oracle, so the on-device self-test\n"
            " * replays these and compares -- which is how we know the interpreter\n"
            " * behaves identically on the target CPU, not just on the CI runner.\n"
            " */\n"
            "#include \"xcore/golden.h\"\n\n"
            "const golden_vec xc_golden[] = {\n");
    }

    int failed = 0, total = 0;
    const int n = (int)(sizeof cases / sizeof cases[0]);
    for (int i = 0; i < n; i++) {
        /* several seeds per case: flags-in and register soup vary */
        for (uint64_t seed = 1; seed <= 6; seed++) {
            total++;
            if (run_case(&cases[i], seed * 0x9E3779B97F4A7C15ull)) { failed++; if (!g_golden) break; }
        }
    }
    if (g_golden) {
        fprintf(g_golden, "};\nconst unsigned xc_golden_count = sizeof xc_golden / sizeof xc_golden[0];\n");
        fclose(g_golden);
        printf("difftest: wrote golden vectors to %s\n", emit);
    }
    printf("difftest: %d cases, %d runs, %d failed\n", n, total, failed);
    return failed ? 1 : 0;
}
