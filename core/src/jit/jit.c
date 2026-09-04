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
 * unmapped memory is a host fault today; the signal-to-guest-fault bridge is
 * the runtime's job later.
 *
 * Verification
 * ------------
 * The self-test replays the golden vectors through this path, so the JIT is
 * held to the same silicon recordings as the interpreter; the block cache's
 * byte check makes self-modifying code safe here too.
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
static int g_enabled = -1;

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
void xc_jit_code_reset(void) { g_code_used = 0; }

void xc_jit_stats(uint64_t *blocks, uint64_t *callouts, uint64_t *bytes) {
    if (blocks) *blocks = g_stat_blocks;
    if (callouts) *callouts = g_stat_callouts;
    if (bytes) *bytes = g_code_used;
}

#if XC_JIT_HOST

/* ----------------------------------------------------------- C helpers */
/* Called from generated code. All of them may clobber x0-x17, so the
 * compiler spills the guest register cache around every call. */

/* One instruction through the interpreter. Returns 1 if the block must exit
 * afterwards (stop condition, or RIP left the straight line), else 0. */
static int jit_callout(xc_cpu *c, const dinsn *d) {
    g_stat_callouts++;
    xc_flags_sync(c);
    xc_stop st = xc_exec_decoded(c, &d->in, xc_cache_ops(d));
    if (st != XC_STOP_NONE) return 1;
    return c->rip != d->rip + d->in.length;
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
    uint16_t loaded, dirty;
    int nz, nz_bits;
    int lz;
    const block *b;
    const dinsn *d;             /* current instruction */
    const xop *ops;
    int flags_live;             /* after the current instruction */
    int failed;
} jc;

static int greg(jc *j, int g) {
    if (!(j->loaded & (1u << g))) {
        a64_ldr_off(&j->a, 3, HREG[g], R_CPU, OFF(gpr) + 8u * g);
        j->loaded |= 1u << g;
    }
    return HREG[g];
}
static void gdirty(jc *j, int g) { j->loaded |= 1u << g; j->dirty |= 1u << g; }
static void flush(jc *j) {
    for (int g = 0; g < 16; g++) if (j->dirty & (1u << g)) a64_str_off(&j->a, 3, HREG[g], R_CPU, OFF(gpr) + 8u * g);
    j->dirty = 0;
}
static void invalidate(jc *j) { j->loaded = j->dirty = 0; }

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
        a64_str_reg(&j->a, ldst_size(bits), rs, R_BASE, T4, w ? 2 : 3);
        return;
    }
    j->failed = 1;
}

/* --- exits --- */

static void emit_set_rip_imm(jc *j, uint64_t rip) { a64_mov_imm(&j->a, T0, rip); a64_str_off(&j->a, 3, T0, R_CPU, OFF(rip)); }
static void emit_exit_imm(jc *j, uint64_t rip) { flush(j); emit_set_rip_imm(j, rip); a64_br(&j->a, R_DISP); }
static void emit_exit_reg(jc *j, int r) {     /* r must not be a guest register that flush() rewrites... it only stores, fine */
    flush(j);
    a64_str_off(&j->a, 3, r, R_CPU, OFF(rip));
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
    a64_str_reg(&j->a, sf ? 3 : 2, rval, R_BASE, rsp, sf ? 3 : 2);
}
static void emit_pop_to(jc *j, int rd) {
    int sw = j->d->in.stack_width, sf = sw == 64;
    int rsp = greg(j, XC_RSP);
    a64_ldr_reg(&j->a, sf ? 3 : 2, rd, R_BASE, rsp, sf ? 3 : 2);
    a64_add_imm(&j->a, sf, rsp, rsp, sw / 8);
    gdirty(j, XC_RSP);
}

/* ------------------------------------------------------- per instruction */

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
    default:
        return FR | FW_PART;         /* conservative: callout reads exact flags and may change them */
    }
}

/* Compile one instruction. */
static void emit_insn(jc *j) {
    const ZydisDecodedInstruction *in = &j->d->in;
    const xop *ops = j->ops;
    ZydisMnemonic m = in->mnemonic;
    int cc;

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
        if (ops[0].type == XOP_IMM) { a64_mov_imm(&j->a, T1, ops[0].imm); r = T1; }
        else { r = ld_op(j, &ops[0], T1, 0); if (r != T1) { a64_mov_reg(&j->a, 1, T1, r); r = T1; } }
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
    jc j; memset(&j, 0, sizeof j);
    j.a.buf = (uint32_t *)(g_code_rw + g_code_used); j.a.cap = (uint32_t)(room / 4);
    j.mode = c->mode; j.b = b;
    const dinsn *insns = xc_cache_insns(b);

    /* liveness of flags after each instruction, backwards from "live at exit" */
    static uint8_t live[MAX_BLOCK];
    int l = 1;
    for (int i = (int)b->count - 1; i >= 0; i--) {
        live[i] = (uint8_t)l;
        int u = flag_use(&insns[i].in, xc_cache_ops(&insns[i]));
        if (u & FR) l = 1; else if (u & FW_ALL) l = 0;
    }

    code_write_begin();
    /* step budget */
    a64_ldr_off(&j.a, 3, T0, R_CPU, OFF(steps));
    a64_sub_imm(&j.a, 1, T0, T0, b->count);
    a64_str_off(&j.a, 3, T0, R_CPU, OFF(steps));

    for (uint32_t i = 0; i < b->count; i++) {
        j.d = &insns[i]; j.ops = xc_cache_ops(j.d); j.flags_live = live[i];
        emit_insn(&j);
        if (j.failed) { code_write_end(j.a.buf, 0, RX(j.a.buf)); return 0; }
    }
    /* fell off the end (block ended at a decode failure or MAX_BLOCK): continue sequentially */
    const dinsn *last = &insns[b->count - 1];
    emit_exit_imm(&j, last->rip + last->in.length);

    if (j.a.overflow) { code_write_end(j.a.buf, 0, RX(j.a.buf)); return 0; }
    size_t bytes = (size_t)j.a.n * 4;
    void *rx = RX(j.a.buf);
    code_write_end(j.a.buf, bytes, rx);
    if (getenv("XCORE_JIT_DUMP")) {          /* raw code for `objdump -D -b binary -m aarch64` */
        char name[64]; snprintf(name, sizeof name, "%s/blk_%llx.bin", getenv("XCORE_JIT_DUMP"), (unsigned long long)b->rip);
        FILE *f = fopen(name, "wb"); if (f) { fwrite(j.a.buf, 1, bytes, f); fclose(f); }
    }
    g_code_used += (bytes + 15) & ~(size_t)15;
    g_stat_blocks++;
    return rx;
}

/* -------------------------------------------------------- dispatcher */

/* The enter/exit stub is itself generated at init: it saves the callee-saved
 * registers, loads x25/x26/x27, and loops { check stop/steps; x0 = lookup;
 * br x0 } until told to leave. Blocks come back with `br x27`. */
typedef xc_stop (*enter_fn)(xc_cpu *);
static enter_fn g_enter;

static int g_trace = -1;
static void *lookup_compile(xc_cpu *c) {
    if (g_trace < 0) g_trace = getenv("XCORE_JIT_TRACE") != 0;
    block *b = xc_cache_lookup(c);
    if (!b) return 0;
    if (g_trace) {
        char dis[128]; xc_disasm(c, c->rip, dis, sizeof dis);
        fprintf(stderr, "[jit] rip=%#llx %s%s\n", (unsigned long long)c->rip, dis, b->code ? "" : "  (compile)");
    }
    if (!b->code) {
        b->code = compile(c, b);
        if (!b->code && g_code_cap - g_code_used < 65536) {   /* code memory full: flush everything, once */
            xc_cache_flush();
            b = xc_cache_lookup(c);
            if (b) b->code = compile(c, b);
        }
        if (!b || !b->code) { if (c->stop == XC_STOP_NONE) c->stop = XC_STOP_UNDEFINED; return 0; }
    }
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
    a64_mov_reg(&a, 1, R_CPU, 0);
    a64_ldr_off(&a, 3, R_BASE, R_CPU, OFF(jit_base));
    uint32_t loop = a64_here(&a);
    a64_mov_imm4(&a, R_DISP, (uint64_t)(uintptr_t)(xbuf + loop));  /* blocks return here with br x27 */
    /* stop set? steps exhausted? */
    a64_ldr_off(&a, 2, 0, R_CPU, OFF(stop));
    uint32_t b_stop = a64_here(&a); a64_cbnz(&a, 0, 0, 0);
    a64_ldr_off(&a, 3, 0, R_CPU, OFF(steps));
    a64_cmp_imm(&a, 1, 0, 0);
    uint32_t b_steps = a64_here(&a); a64_bcond(&a, CC_LE, 0);
    a64_mov_reg(&a, 1, 0, R_CPU);
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
    if (!g_enter) build_enter();
    c->steps = (int64_t)(max_steps > INT64_MAX ? INT64_MAX : max_steps);
    c->stop = XC_STOP_NONE;
    c->jit_base = c->mem->mode == XC_MODE_64 ? 0 : (uint64_t)(uintptr_t)c->mem->base;
    xc_stop st = g_enter(c);
    xc_flags_sync(c);
    if (st == XC_STOP_STEPS) c->stop = XC_STOP_STEPS;
    return st;
#else
    (void)c; (void)max_steps;
    return XC_STOP_UNDEFINED;
#endif
}

