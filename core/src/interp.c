/* xcore interpreter.
 *
 * Correctness first. Every instruction here is checked against real silicon
 * by tests/difftest, which runs the same bytes natively on an x86-64 host and
 * compares the full register file and arithmetic flags. Add an instruction,
 * add a difftest case; the CI runner is the oracle.
 *
 * Performance comes later from a dynarec that sits behind the same xc_cpu
 * state; this file stays as the reference semantics and the fallback when JIT
 * is unavailable.
 */
#include "xcore/cpu.h"

#include <Zydis/Zydis.h>
#include "softfloat.h"
#include "xop.h"
#include <fenv.h>
#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ utils */

static inline uint64_t mask_bits(uint64_t v, int bits) {
    return bits >= 64 ? v : (v & ((1ull << bits) - 1));
}
static inline uint64_t sext(uint64_t v, int bits) {
    if (bits >= 64) return v;
    uint64_t m = 1ull << (bits - 1);
    return (mask_bits(v, bits) ^ m) - m;
}
static inline int msb(uint64_t v, int bits) { return (int)((v >> (bits - 1)) & 1); }

static inline int parity_even(uint64_t v) {
    v &= 0xFF;
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}

static inline void set_flag(xc_cpu *c, uint64_t f, int on) {
    if (on) c->rflags |= f; else c->rflags &= ~f;
}
static inline int get_flag(const xc_cpu *c, uint64_t f) { return (c->rflags & f) != 0; }

/* ZF/SF/PF from a result of `bits` width. */
static void flags_zsp(xc_cpu *c, uint64_t res, int bits) {
    res = mask_bits(res, bits);
    set_flag(c, XC_ZF, res == 0);
    set_flag(c, XC_SF, msb(res, bits));
    set_flag(c, XC_PF, parity_even(res));
}

/* res = a + b + cin */
static void flags_add(xc_cpu *c, uint64_t a, uint64_t b, uint64_t cin, uint64_t res, int bits) {
    a = mask_bits(a, bits); b = mask_bits(b, bits); res = mask_bits(res, bits);
    int cf;
    if (bits < 64) cf = ((a + b + cin) >> bits) & 1;
    else {
        uint64_t t; int o1 = __builtin_add_overflow(a, b, &t);
        int o2 = __builtin_add_overflow(t, cin, &t);
        cf = o1 | o2;
    }
    set_flag(c, XC_CF, cf);
    set_flag(c, XC_OF, msb((a ^ res) & (b ^ res), bits));
    set_flag(c, XC_AF, ((a ^ b ^ res) >> 4) & 1);
    flags_zsp(c, res, bits);
}

/* res = a - b - bin */
static void flags_sub(xc_cpu *c, uint64_t a, uint64_t b, uint64_t bin, uint64_t res, int bits) {
    a = mask_bits(a, bits); b = mask_bits(b, bits); res = mask_bits(res, bits);
    int cf;
    if (bits < 64) cf = a < b + bin || (bin && b == mask_bits(~0ull, bits));
    else {
        uint64_t t; int o1 = __builtin_sub_overflow(a, b, &t);
        int o2 = __builtin_sub_overflow(t, bin, &t);
        cf = o1 | o2;
    }
    set_flag(c, XC_CF, cf);
    set_flag(c, XC_OF, msb((a ^ b) & (a ^ res), bits));
    set_flag(c, XC_AF, ((a ^ b ^ res) >> 4) & 1);
    flags_zsp(c, res, bits);
}

static void flags_logic(xc_cpu *c, uint64_t res, int bits) {
    set_flag(c, XC_CF, 0);
    set_flag(c, XC_OF, 0);
    set_flag(c, XC_AF, 0);
    flags_zsp(c, res, bits);
}

/* -------------------------------------------------------------- registers */

/* Map a Zydis GPR to (index, is-high-byte). Zydis GPR8 ids run
 * AL CL DL BL AH CH DH BH SPL BPL SIL DIL R8B..R15B. */
static int gpr_index(ZydisRegister r, int *high8) {
    int id = ZydisRegisterGetId(r);
    *high8 = 0;
    if (ZydisRegisterGetClass(r) == ZYDIS_REGCLASS_GPR8) {
        if (id >= 4 && id <= 7) { *high8 = 1; return id - 4; }
        if (id >= 8) return id - 4;
    }
    return id;
}

static int reg_bits(const xc_cpu *c, ZydisRegister r) {
    return ZydisRegisterGetWidth(c->mode == XC_MODE_64 ? ZYDIS_MACHINE_MODE_LONG_64
                                                       : ZYDIS_MACHINE_MODE_LONG_COMPAT_32, r);
}

static uint64_t reg_read(const xc_cpu *c, ZydisRegister r) {
    ZydisRegisterClass cls = ZydisRegisterGetClass(r);
    if (cls == ZYDIS_REGCLASS_IP) return c->rip;                 /* handled by caller for RIP-rel */
    if (cls == ZYDIS_REGCLASS_SEGMENT) return c->sreg[(unsigned)(r - ZYDIS_REGISTER_ES) % 6];   /* selector value only */
    int hi; int i = gpr_index(r, &hi);
    uint64_t v = c->gpr[i];
    if (hi) v >>= 8;
    return mask_bits(v, reg_bits(c, r));
}

static void reg_write(xc_cpu *c, ZydisRegister r, uint64_t v) {
    if (ZydisRegisterGetClass(r) == ZYDIS_REGCLASS_SEGMENT) {
        /* Flat model: a selector load changes the visible selector and
         * nothing else. FS/GS bases are set by the host (arch_prctl /
         * set_thread_area / the Win32 TEB) rather than through a GDT. */
        c->sreg[(unsigned)(r - ZYDIS_REGISTER_ES) % 6] = (uint16_t)v; return;
    }
    int hi; int i = gpr_index(r, &hi);
    switch (reg_bits(c, r)) {
    case 8:
        if (hi) c->gpr[i] = (c->gpr[i] & ~0xFF00ull) | ((v & 0xFF) << 8);
        else    c->gpr[i] = (c->gpr[i] & ~0xFFull) | (v & 0xFF);
        break;
    case 16: c->gpr[i] = (c->gpr[i] & ~0xFFFFull) | (v & 0xFFFF); break;
    case 32: c->gpr[i] = v & 0xFFFFFFFFull; break;               /* zero-extends */
    default: c->gpr[i] = v; break;
    }
}

/* Operand-form register access: no table lookups, the xop carries the slot. */
static inline uint64_t xreg_read(const xc_cpu *c, const xop *o) {
    switch (o->rcls) {
    case XR_GPR: { uint64_t v = c->gpr[o->ridx]; if (o->rhi8) v >>= 8; return mask_bits(v, o->rbits); }
    case XR_SEG: return c->sreg[o->ridx];
    case XR_IP:  return c->rip;
    default:     return 0;
    }
}
static inline void xreg_write(xc_cpu *c, const xop *o, uint64_t v) {
    if (o->rcls != XR_GPR) { if (o->rcls == XR_SEG) c->sreg[o->ridx] = (uint16_t)v; return; }
    uint64_t *r = &c->gpr[o->ridx];
    switch (o->rbits) {
    case 8:  if (o->rhi8) *r = (*r & ~0xFF00ull) | ((v & 0xFF) << 8); else *r = (*r & ~0xFFull) | (v & 0xFF); break;
    case 16: *r = (*r & ~0xFFFFull) | (v & 0xFFFF); break;
    case 32: *r = v & 0xFFFFFFFFull; break;
    default: *r = v; break;
    }
}

/* ----------------------------------------------------------------- memory */

typedef struct {
    xc_cpu *c;
    const ZydisDecodedInstruction *in;
    const xop *ops;
    uint64_t next_rip;
    xc_stop stop;
} ctx;

static int mem_read(ctx *x, uint64_t ga, int bits, uint64_t *out) {
    void *p = xc_mem_ptr(x->c->mem, ga, (size_t)bits / 8);
    if (!p) { x->stop = XC_STOP_FAULT; x->c->fault_addr = ga; return 0; }
    uint64_t v = 0;
    memcpy(&v, p, (size_t)bits / 8);          /* little-endian host assumed */
    *out = v;
    return 1;
}
static int mem_write(ctx *x, uint64_t ga, int bits, uint64_t v) {
    void *p = xc_mem_ptr(x->c->mem, ga, (size_t)bits / 8);
    if (!p) { x->stop = XC_STOP_FAULT; x->c->fault_addr = ga; return 0; }
    memcpy(p, &v, (size_t)bits / 8);
    return 1;
}

static uint64_t ea(ctx *x, const xop *op) {
    const xc_cpu *c = x->c;
    uint64_t a = (uint64_t)op->disp;                      /* RIP-relative already folded in */
    if (op->mbase >= 0) a += c->gpr[op->mbase];
    if (op->mindex >= 0) a += c->gpr[op->mindex] * op->mscale;
    a = mask_bits(a, x->in->address_width);
    /* Flat segments except FS/GS, whose bases the host sets (TLS, the Win32
     * TEB). In 32-bit mode the sum is still a 32-bit address. */
    if (op->mseg == 1) a += c->fs_base;
    else if (op->mseg == 2) a += c->gs_base;
    return mask_bits(a, c->mode);
}

/* --------------------------------------------------------------- operands */

static int op_read(ctx *x, int i, uint64_t *out) {
    const xop *op = &x->ops[i];
    switch (op->type) {
    case XOP_REG:
        *out = xreg_read(x->c, op); return 1;
    case XOP_MEM:
        return mem_read(x, ea(x, op), op->size, out);
    case XOP_IMM:
        *out = op->imm; return 1;
    default:
        x->stop = XC_STOP_UNDEFINED; return 0;
    }
}

static int op_write(ctx *x, int i, uint64_t v) {
    const xop *op = &x->ops[i];
    switch (op->type) {
    case XOP_REG:
        xreg_write(x->c, op, v); return 1;
    case XOP_MEM:
        return mem_write(x, ea(x, op), op->size, v);
    default:
        x->stop = XC_STOP_UNDEFINED; return 0;
    }
}

/* --------------------------------------------------------- 128-bit operands */

/* Read an operand as up to 128 bits. Registers narrower than 128 bits and
 * memory operands are zero-extended; the caller decides what the top means. */
static int op_read128(ctx *x, int i, xc_u128 *out) {
    const xop *op = &x->ops[i];
    out->lo = out->hi = 0;
    if (op->type == XOP_REG) {
        if (op->rcls == XR_XMM) { *out = x->c->xmm[op->ridx]; return 1; }
        out->lo = xreg_read(x->c, op); return 1;
    }
    if (op->type == XOP_MEM) {
        uint64_t a = ea(x, op);
        if (op->size == 128) return mem_read(x, a, 64, &out->lo) && mem_read(x, a + 8, 64, &out->hi);
        return mem_read(x, a, op->size, &out->lo);
    }
    if (op->type == XOP_IMM) { out->lo = op->imm; return 1; }
    x->stop = XC_STOP_UNDEFINED; return 0;
}

/* Write `bits` (32/64/128) of v to operand i. Writing an XMM register with
 * fewer than 128 bits zeroes the rest (MOVD/MOVQ semantics). */
static int op_write128(ctx *x, int i, xc_u128 v, int bits) {
    const xop *op = &x->ops[i];
    if (op->type == XOP_REG) {
        if (op->rcls == XR_XMM) {
            xc_u128 *d = &x->c->xmm[op->ridx];
            if (bits == 128) *d = v;
            else { d->lo = bits == 64 ? v.lo : (v.lo & 0xFFFFFFFFull); d->hi = 0; }
            return 1;
        }
        xreg_write(x->c, op, v.lo); return 1;
    }
    if (op->type == XOP_MEM) {
        uint64_t a = ea(x, op);
        if (bits == 128) return mem_write(x, a, 64, v.lo) && mem_write(x, a + 8, 64, v.hi);
        return mem_write(x, a, bits, v.lo);
    }
    x->stop = XC_STOP_UNDEFINED; return 0;
}

/* Lane-wise helpers over 16 bytes / 8 words / 4 dwords / 2 qwords. */
typedef union { xc_u128 q; uint8_t b[16]; uint16_t w[8]; uint32_t d[4]; float f[4]; double e[2]; } lanes;

/* ------------------------------------------------------------------ stack */

static int push(ctx *x, uint64_t v) {
    int sw = x->in->stack_width;                       /* 64 or 32 */
    uint64_t sp = mask_bits(x->c->gpr[XC_RSP] - sw / 8, sw);
    if (!mem_write(x, sp, sw, v)) return 0;
    x->c->gpr[XC_RSP] = sw == 64 ? sp : (sp & 0xFFFFFFFFull);
    return 1;
}
static int pop(ctx *x, uint64_t *out) {
    int sw = x->in->stack_width;
    uint64_t sp = mask_bits(x->c->gpr[XC_RSP], sw);
    if (!mem_read(x, sp, sw, out)) return 0;
    sp = mask_bits(sp + sw / 8, sw);
    x->c->gpr[XC_RSP] = sp;
    return 1;
}

/* ------------------------------------------------------------- conditions */

typedef enum { CC_O, CC_NO, CC_B, CC_NB, CC_Z, CC_NZ, CC_BE, CC_NBE,
               CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_NL, CC_LE, CC_NLE, CC_NONE } cc_t;

static int cc_eval(const xc_cpu *c, cc_t cc) {
    int cf = get_flag(c, XC_CF), zf = get_flag(c, XC_ZF), sf = get_flag(c, XC_SF),
        of = get_flag(c, XC_OF), pf = get_flag(c, XC_PF);
    switch (cc) {
    case CC_O:   return of;        case CC_NO:  return !of;
    case CC_B:   return cf;        case CC_NB:  return !cf;
    case CC_Z:   return zf;        case CC_NZ:  return !zf;
    case CC_BE:  return cf || zf;  case CC_NBE: return !cf && !zf;
    case CC_S:   return sf;        case CC_NS:  return !sf;
    case CC_P:   return pf;        case CC_NP:  return !pf;
    case CC_L:   return sf != of;  case CC_NL:  return sf == of;
    case CC_LE:  return zf || sf != of;
    case CC_NLE: return !zf && sf == of;
    default:     return 0;
    }
}

#define CC_FAMILY(PFX) \
    case ZYDIS_MNEMONIC_##PFX##O:   return CC_O;   case ZYDIS_MNEMONIC_##PFX##NO:  return CC_NO;  \
    case ZYDIS_MNEMONIC_##PFX##B:   return CC_B;   case ZYDIS_MNEMONIC_##PFX##NB:  return CC_NB;  \
    case ZYDIS_MNEMONIC_##PFX##Z:   return CC_Z;   case ZYDIS_MNEMONIC_##PFX##NZ:  return CC_NZ;  \
    case ZYDIS_MNEMONIC_##PFX##BE:  return CC_BE;  case ZYDIS_MNEMONIC_##PFX##NBE: return CC_NBE; \
    case ZYDIS_MNEMONIC_##PFX##S:   return CC_S;   case ZYDIS_MNEMONIC_##PFX##NS:  return CC_NS;  \
    case ZYDIS_MNEMONIC_##PFX##P:   return CC_P;   case ZYDIS_MNEMONIC_##PFX##NP:  return CC_NP;  \
    case ZYDIS_MNEMONIC_##PFX##L:   return CC_L;   case ZYDIS_MNEMONIC_##PFX##NL:  return CC_NL;  \
    case ZYDIS_MNEMONIC_##PFX##LE:  return CC_LE;  case ZYDIS_MNEMONIC_##PFX##NLE: return CC_NLE;

static cc_t cc_jcc(ZydisMnemonic m)   { switch (m) { CC_FAMILY(J)    default: return CC_NONE; } }
static cc_t cc_setcc(ZydisMnemonic m) { switch (m) { CC_FAMILY(SET)  default: return CC_NONE; } }
static cc_t cc_cmov(ZydisMnemonic m)  { switch (m) { CC_FAMILY(CMOV) default: return CC_NONE; } }

/* ------------------------------------------------------------- arithmetic */

typedef enum { ALU_ADD, ALU_ADC, ALU_SUB, ALU_SBB, ALU_AND, ALU_OR, ALU_XOR, ALU_CMP, ALU_TEST } alu_t;

static int do_alu(ctx *x, alu_t op) {
    uint64_t a, b;
    if (!op_read(x, 0, &a) || !op_read(x, 1, &b)) return 0;
    int bits = x->ops[0].size;
    uint64_t r, cf = get_flag(x->c, XC_CF);
    switch (op) {
    case ALU_ADD: r = a + b;       flags_add(x->c, a, b, 0, r, bits); break;
    case ALU_ADC: r = a + b + cf;  flags_add(x->c, a, b, cf, r, bits); break;
    case ALU_SUB: case ALU_CMP:
                  r = a - b;       flags_sub(x->c, a, b, 0, r, bits); break;
    case ALU_SBB: r = a - b - cf;  flags_sub(x->c, a, b, cf, r, bits); break;
    case ALU_AND: case ALU_TEST:
                  r = a & b;       flags_logic(x->c, r, bits); break;
    case ALU_OR:  r = a | b;       flags_logic(x->c, r, bits); break;
    case ALU_XOR: r = a ^ b;       flags_logic(x->c, r, bits); break;
    default: return 0;
    }
    if (op == ALU_CMP || op == ALU_TEST) return 1;
    return op_write(x, 0, mask_bits(r, bits));
}

static int do_incdec(ctx *x, int dec) {
    uint64_t a; if (!op_read(x, 0, &a)) return 0;
    int bits = x->ops[0].size, cf = get_flag(x->c, XC_CF);
    uint64_t r = dec ? a - 1 : a + 1;
    if (dec) flags_sub(x->c, a, 1, 0, r, bits); else flags_add(x->c, a, 1, 0, r, bits);
    set_flag(x->c, XC_CF, cf);                          /* INC/DEC preserve CF */
    return op_write(x, 0, mask_bits(r, bits));
}

static int do_shift(ctx *x, ZydisMnemonic m) {
    uint64_t v, cnt;
    if (!op_read(x, 0, &v) || !op_read(x, 1, &cnt)) return 0;
    int bits = x->ops[0].size;
    cnt &= (bits == 64) ? 63 : 31;
    if (cnt == 0) return 1;                             /* no flags change */
    v = mask_bits(v, bits);
    uint64_t r; int cf, of;
    switch (m) {
    case ZYDIS_MNEMONIC_SHL:
        cf = (cnt <= (uint64_t)bits) ? (int)((v >> (bits - cnt)) & 1) : 0;
        r  = mask_bits(v << cnt, bits);
        of = msb(r, bits) ^ cf;
        break;
    case ZYDIS_MNEMONIC_SHR:
        cf = (int)((v >> (cnt - 1)) & 1);
        r  = v >> cnt;
        of = msb(v, bits);
        break;
    case ZYDIS_MNEMONIC_SAR: {
        int64_t s = (int64_t)sext(v, bits);
        cf = (int)((s >> (cnt - 1)) & 1);
        r  = mask_bits((uint64_t)(s >> cnt), bits);
        of = 0;
        break;
    }
    case ZYDIS_MNEMONIC_ROL: {
        cnt %= bits;
        r  = cnt ? mask_bits((v << cnt) | (v >> (bits - cnt)), bits) : v;
        cf = (int)(r & 1);
        of = msb(r, bits) ^ cf;
        set_flag(x->c, XC_CF, cf); set_flag(x->c, XC_OF, of);
        return op_write(x, 0, r);                        /* ROL/ROR: only CF/OF */
    }
    case ZYDIS_MNEMONIC_ROR: {
        cnt %= bits;
        r  = cnt ? mask_bits((v >> cnt) | (v << (bits - cnt)), bits) : v;
        cf = msb(r, bits);
        of = msb(r, bits) ^ (int)((r >> (bits - 2)) & 1);
        set_flag(x->c, XC_CF, cf); set_flag(x->c, XC_OF, of);
        return op_write(x, 0, r);
    }
    default: x->stop = XC_STOP_UNDEFINED; return 0;
    }
    set_flag(x->c, XC_CF, cf);
    set_flag(x->c, XC_OF, of);                          /* architecturally only for cnt==1 */
    set_flag(x->c, XC_AF, 0);
    flags_zsp(x->c, r, bits);
    return op_write(x, 0, r);
}

static int do_imul(ctx *x) {
    int n = x->in->operand_count_visible;
    int bits = x->ops[0].size;
    if (n == 1) {                                        /* RDX:RAX = RAX * src */
        uint64_t s; if (!op_read(x, 0, &s)) return 0;
        __int128 p = (__int128)(int64_t)sext(x->c->gpr[XC_RAX], bits) * (__int128)(int64_t)sext(s, bits);
        uint64_t lo = mask_bits((uint64_t)p, bits), hi = mask_bits((uint64_t)(p >> bits), bits);
        int ovf = p != (__int128)(int64_t)sext(lo, bits);
        if (bits == 8) { x->c->gpr[XC_RAX] = (x->c->gpr[XC_RAX] & ~0xFFFFull) | (uint64_t)((uint16_t)p); }
        else {
            reg_write(x->c, bits == 64 ? ZYDIS_REGISTER_RAX : bits == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_AX, lo);
            reg_write(x->c, bits == 64 ? ZYDIS_REGISTER_RDX : bits == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_DX, hi);
        }
        set_flag(x->c, XC_CF, ovf); set_flag(x->c, XC_OF, ovf);
        flags_zsp(x->c, lo, bits); set_flag(x->c, XC_AF, 0);
        return 1;
    }
    uint64_t a, b;
    if (!op_read(x, n == 3 ? 1 : 0, &a) || !op_read(x, n == 3 ? 2 : 1, &b)) return 0;
    __int128 p = (__int128)(int64_t)sext(a, bits) * (__int128)(int64_t)sext(b, bits);
    uint64_t r = mask_bits((uint64_t)p, bits);
    int ovf = p != (__int128)(int64_t)sext(r, bits);
    set_flag(x->c, XC_CF, ovf); set_flag(x->c, XC_OF, ovf);
    flags_zsp(x->c, r, bits); set_flag(x->c, XC_AF, 0);
    return op_write(x, 0, r);
}

static int do_mul(ctx *x) {
    int bits = x->ops[0].size;
    uint64_t s; if (!op_read(x, 0, &s)) return 0;
    unsigned __int128 p = (unsigned __int128)mask_bits(x->c->gpr[XC_RAX], bits) * (unsigned __int128)mask_bits(s, bits);
    uint64_t lo = mask_bits((uint64_t)p, bits), hi = mask_bits((uint64_t)(p >> bits), bits);
    if (bits == 8) x->c->gpr[XC_RAX] = (x->c->gpr[XC_RAX] & ~0xFFFFull) | (uint64_t)((uint16_t)p);
    else {
        reg_write(x->c, bits == 64 ? ZYDIS_REGISTER_RAX : bits == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_AX, lo);
        reg_write(x->c, bits == 64 ? ZYDIS_REGISTER_RDX : bits == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_DX, hi);
    }
    set_flag(x->c, XC_CF, hi != 0); set_flag(x->c, XC_OF, hi != 0);
    flags_zsp(x->c, lo, bits); set_flag(x->c, XC_AF, 0);
    return 1;
}

static int do_div(ctx *x, int signed_) {
    int bits = x->ops[0].size;
    uint64_t d; if (!op_read(x, 0, &d)) return 0;
    d = mask_bits(d, bits);
    if (d == 0) { x->stop = XC_STOP_FAULT; return 0; }              /* #DE */
    uint64_t lo, hi;
    if (bits == 8) { lo = x->c->gpr[XC_RAX] & 0xFF; hi = (x->c->gpr[XC_RAX] >> 8) & 0xFF; }
    else { lo = mask_bits(x->c->gpr[XC_RAX], bits); hi = mask_bits(x->c->gpr[XC_RDX], bits); }
    uint64_t q, r;
    if (signed_) {
        __int128 n = ((__int128)(int64_t)sext(hi, bits) << bits) | lo;
        __int128 dv = (int64_t)sext(d, bits);
        __int128 qq = n / dv, rr = n % dv;
        if (qq != (__int128)(int64_t)sext((uint64_t)qq, bits)) { x->stop = XC_STOP_FAULT; return 0; }
        q = mask_bits((uint64_t)qq, bits); r = mask_bits((uint64_t)rr, bits);
    } else {
        unsigned __int128 n = ((unsigned __int128)hi << bits) | lo;
        unsigned __int128 qq = n / d, rr = n % d;
        if (qq >> bits) { x->stop = XC_STOP_FAULT; return 0; }
        q = (uint64_t)qq; r = (uint64_t)rr;
    }
    if (bits == 8) x->c->gpr[XC_RAX] = (x->c->gpr[XC_RAX] & ~0xFFFFull) | (r << 8) | q;
    else {
        reg_write(x->c, bits == 64 ? ZYDIS_REGISTER_RAX : bits == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_AX, q);
        reg_write(x->c, bits == 64 ? ZYDIS_REGISTER_RDX : bits == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_DX, r);
    }
    return 1;                                                        /* flags undefined */
}

/* --------------------------------------------------------------- strings */

static int do_string(ctx *x, int is_movs, int bits) {
    xc_cpu *c = x->c;
    int aw = x->in->address_width;
    int rep = (x->in->attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
    int64_t step = get_flag(c, XC_DF) ? -(bits / 8) : (bits / 8);
    for (;;) {
        if (rep && mask_bits(c->gpr[XC_RCX], aw) == 0) break;
        uint64_t v;
        if (is_movs) {
            if (!mem_read(x, mask_bits(c->gpr[XC_RSI], aw), bits, &v)) return 0;
            c->gpr[XC_RSI] = mask_bits(c->gpr[XC_RSI] + step, aw);
        } else v = mask_bits(c->gpr[XC_RAX], bits);
        if (!mem_write(x, mask_bits(c->gpr[XC_RDI], aw), bits, v)) return 0;
        c->gpr[XC_RDI] = mask_bits(c->gpr[XC_RDI] + step, aw);
        if (!rep) break;
        c->gpr[XC_RCX] = mask_bits(c->gpr[XC_RCX] - 1, aw);
    }
    return 1;
}

/* CMPS / SCAS / LODS -- the rest of the string family. REPE/REPNE loop on
 * RCX and ZF; plain REP is not defined for these but decodes as REPE. */
static int do_string_cmp(ctx *x, ZydisMnemonic m, int bits) {
    xc_cpu *c = x->c;
    int aw = x->in->address_width;
    int repe = (x->in->attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE)) != 0;
    int repne = (x->in->attributes & ZYDIS_ATTRIB_HAS_REPNE) != 0;
    int rep = repe || repne;
    int64_t step = get_flag(c, XC_DF) ? -(bits / 8) : (bits / 8);
    for (;;) {
        if (rep && mask_bits(c->gpr[XC_RCX], aw) == 0) break;
        uint64_t a, b;
        switch (m) {
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD: case ZYDIS_MNEMONIC_CMPSQ:
            if (!mem_read(x, mask_bits(c->gpr[XC_RSI], aw), bits, &a)) return 0;
            if (!mem_read(x, mask_bits(c->gpr[XC_RDI], aw), bits, &b)) return 0;
            c->gpr[XC_RSI] = mask_bits(c->gpr[XC_RSI] + step, aw);
            c->gpr[XC_RDI] = mask_bits(c->gpr[XC_RDI] + step, aw);
            flags_sub(c, a, b, 0, a - b, bits);
            break;
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: case ZYDIS_MNEMONIC_SCASQ:
            a = mask_bits(c->gpr[XC_RAX], bits);
            if (!mem_read(x, mask_bits(c->gpr[XC_RDI], aw), bits, &b)) return 0;
            c->gpr[XC_RDI] = mask_bits(c->gpr[XC_RDI] + step, aw);
            flags_sub(c, a, b, 0, a - b, bits);
            break;
        default: /* LODS */
            if (!mem_read(x, mask_bits(c->gpr[XC_RSI], aw), bits, &a)) return 0;
            c->gpr[XC_RSI] = mask_bits(c->gpr[XC_RSI] + step, aw);
            reg_write(c, bits == 64 ? ZYDIS_REGISTER_RAX : bits == 32 ? ZYDIS_REGISTER_EAX : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_AL, a);
            break;
        }
        if (!rep) break;
        c->gpr[XC_RCX] = mask_bits(c->gpr[XC_RCX] - 1, aw);
        if (m != ZYDIS_MNEMONIC_LODSB && m != ZYDIS_MNEMONIC_LODSW && m != ZYDIS_MNEMONIC_LODSD && m != ZYDIS_MNEMONIC_LODSQ) {
            int zf = get_flag(c, XC_ZF);
            if (repe && !zf) break;
            if (repne && zf) break;
        }
    }
    return 1;
}

/* Bit test family: BT/BTS/BTR/BTC. With a memory operand and a register bit
 * offset the address is adjusted by offset/width (signed), as the SDM says. */
static int do_bt(ctx *x, ZydisMnemonic m) {
    int bits = x->ops[0].size;
    uint64_t off;
    if (!op_read(x, 1, &off)) return 0;
    uint64_t v; uint64_t addr = 0; int is_mem = x->ops[0].type == XOP_MEM;
    if (is_mem) {
        addr = ea(x, &x->ops[0]);
        if (x->ops[1].type == XOP_REG) {
            int64_t so = (int64_t)sext(off, x->ops[1].size);
            addr += (uint64_t)((so >> (bits == 64 ? 6 : bits == 32 ? 5 : 4)) * (bits / 8));
        }
        if (!mem_read(x, addr, bits, &v)) return 0;
    } else if (!op_read(x, 0, &v)) return 0;
    off &= (uint64_t)(bits - 1);
    set_flag(x->c, XC_CF, (v >> off) & 1);
    if (m == ZYDIS_MNEMONIC_BT) return 1;
    if (m == ZYDIS_MNEMONIC_BTS) v |= 1ull << off;
    else if (m == ZYDIS_MNEMONIC_BTR) v &= ~(1ull << off);
    else v ^= 1ull << off;
    v = mask_bits(v, bits);
    return is_mem ? mem_write(x, addr, bits, v) : op_write(x, 0, v);
}

/* CPUID: a fixed, deliberately modest x86-64 -- SSE2, CMOV, FXSR, no SSSE3/
 * SSE4/AVX/BMI. glibc and friends pick their baseline code paths from this,
 * which are exactly the instructions implemented here. Extend the two
 * together. */
static void do_cpuid(xc_cpu *c) {
    uint32_t leaf = (uint32_t)c->gpr[XC_RAX], sub = (uint32_t)c->gpr[XC_RCX];
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0: a = 0xD; b = 0x756E6547; d = 0x49656E69; cc = 0x6C65746E; break;   /* "GenuineIntel" */
    case 1:
        a = 0x000306A9;                                   /* family 6, model 0x3A */
        b = 0x00000800;                                   /* 1 logical CPU, CLFLUSH 8*8 */
        cc = (1u << 0)  /* SSE3 */ | (1u << 13) /* CX16 */;
        d  = (1u << 0)  /* FPU */ | (1u << 4) /* TSC */ | (1u << 8) /* CX8 */ | (1u << 15) /* CMOV */
           | (1u << 19) /* CLFSH */ | (1u << 23) /* MMX */ | (1u << 24) /* FXSR */ | (1u << 25) /* SSE */
           | (1u << 26) /* SSE2 */;
        break;
    case 7: a = b = cc = d = 0; break;                    /* no BMI/AVX2/ERMS */
    case 0x80000000: a = 0x80000008; break;
    case 0x80000001: d = (1u << 29) /* LM */ | (1u << 20) /* NX */ | (1u << 11) /* SYSCALL */; cc = (1u << 0) /* LAHF */; break;
    case 0x80000008: a = 0x3028; break;                   /* 48-bit VA, 40-bit PA */
    default: (void)sub; break;
    }
    c->gpr[XC_RAX] = a; c->gpr[XC_RBX] = b; c->gpr[XC_RCX] = cc; c->gpr[XC_RDX] = d;
}

/* ------------------------------------------------------- misc integer ops */

/* CMPXCHG r/m, r: compare the accumulator with the destination; on a match
 * store the source, otherwise load the destination into the accumulator.
 * Flags are those of the compare. */
static int do_cmpxchg(ctx *x) {
    int bits = x->ops[0].size;
    ZydisRegister acc = bits == 64 ? ZYDIS_REGISTER_RAX : bits == 32 ? ZYDIS_REGISTER_EAX
                      : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_AL;
    uint64_t d, s, a = reg_read(x->c, acc);
    if (!op_read(x, 0, &d) || !op_read(x, 1, &s)) return 0;
    flags_sub(x->c, a, d, 0, mask_bits(a - d, bits), bits);
    if (mask_bits(a, bits) == mask_bits(d, bits)) return op_write(x, 0, mask_bits(s, bits));
    /* The destination is still written (with its own value) -- that is what
     * makes the memory form a locked RMW; for registers it is a no-op. */
    reg_write(x->c, acc, d);
    return op_write(x, 0, d);
}

static int do_xadd(ctx *x) {
    int bits = x->ops[0].size;
    uint64_t d, s;
    if (!op_read(x, 0, &d) || !op_read(x, 1, &s)) return 0;
    uint64_t r = mask_bits(d + s, bits);
    flags_add(x->c, d, s, 0, r, bits);
    return op_write(x, 1, d) && op_write(x, 0, r);
}

/* BSF/BSR/TZCNT/LZCNT/POPCNT. BSF/BSR leave the destination alone on a zero
 * source (documented as undefined; every Intel and AMD part preserves it). */
static int do_bitscan(ctx *x, ZydisMnemonic m) {
    int bits = x->ops[0].size;
    uint64_t s;
    if (!op_read(x, 1, &s)) return 0;
    s = mask_bits(s, bits);
    xc_cpu *c = x->c;
    if (m == ZYDIS_MNEMONIC_POPCNT) {
        int n = 0; for (uint64_t t = s; t; t &= t - 1) n++;
        c->rflags &= ~(uint64_t)XC_ARITH_FLAGS;
        set_flag(c, XC_ZF, s == 0);
        return op_write(x, 0, (uint64_t)n);
    }
    if (m == ZYDIS_MNEMONIC_TZCNT || m == ZYDIS_MNEMONIC_LZCNT) {
        int n = bits;
        if (s) { n = 0; if (m == ZYDIS_MNEMONIC_TZCNT) while (!((s >> n) & 1)) n++;
                 else while (!((s >> (bits - 1 - n)) & 1)) n++; }
        set_flag(c, XC_CF, s == 0);
        set_flag(c, XC_ZF, n == 0);
        return op_write(x, 0, (uint64_t)n);
    }
    if (s == 0) { set_flag(c, XC_ZF, 1); return 1; }
    set_flag(c, XC_ZF, 0);
    int n = 0;
    if (m == ZYDIS_MNEMONIC_BSF) while (!((s >> n) & 1)) n++;
    else { n = bits - 1; while (!((s >> n) & 1)) n--; }
    return op_write(x, 0, (uint64_t)n);
}

/* SHLD/SHRD dst, src, count */
static int do_shd(ctx *x, int left) {
    int bits = x->ops[0].size;
    uint64_t d, s, cnt;
    if (!op_read(x, 0, &d) || !op_read(x, 1, &s) || !op_read(x, 2, &cnt)) return 0;
    cnt &= bits == 64 ? 63 : 31;
    if (cnt == 0) return 1;
    if ((int)cnt > bits) return 1;                      /* 16-bit with count > 16: undefined */
    d = mask_bits(d, bits); s = mask_bits(s, bits);
    uint64_t r, cf;
    if (left) {
        r = (d << cnt) | (cnt == (uint64_t)bits ? s : (s >> (bits - cnt)));
        cf = (d >> (bits - cnt)) & 1;
    } else {
        r = (d >> cnt) | (cnt == (uint64_t)bits ? s : (s << (bits - cnt)));
        cf = (d >> (cnt - 1)) & 1;
    }
    r = mask_bits(r, bits);
    flags_zsp(x->c, r, bits);
    set_flag(x->c, XC_CF, (int)cf);
    set_flag(x->c, XC_OF, msb(r, bits) != msb(d, bits));
    return op_write(x, 0, r);
}

/* RCL/RCR: rotate through carry, one bit at a time -- counts are small. */
static int do_rc(ctx *x, int left) {
    int bits = x->ops[0].size;
    uint64_t d, cnt;
    if (!op_read(x, 0, &d) || !op_read(x, 1, &cnt)) return 0;
    cnt &= bits == 64 ? 63 : 31;
    if (bits < 32) cnt %= (uint64_t)(bits + 1);
    if (cnt == 0) return 1;
    d = mask_bits(d, bits);
    uint64_t cf = get_flag(x->c, XC_CF);
    for (uint64_t i = 0; i < cnt; i++) {
        if (left) { uint64_t nc = msb(d, bits); d = mask_bits((d << 1) | cf, bits); cf = nc; }
        else      { uint64_t nc = d & 1; d = (d >> 1) | (cf << (bits - 1)); cf = nc; }
    }
    set_flag(x->c, XC_CF, (int)cf);
    set_flag(x->c, XC_OF, left ? (msb(d, bits) != (int)cf) : (msb(d, bits) != (int)((d >> (bits - 2)) & 1)));
    return op_write(x, 0, d);
}

static int do_loop(ctx *x, ZydisMnemonic m, uint64_t *next) {
    xc_cpu *c = x->c;
    int aw = x->in->address_width;
    c->gpr[XC_RCX] = mask_bits(c->gpr[XC_RCX] - 1, aw);
    int go = mask_bits(c->gpr[XC_RCX], aw) != 0;
    if (m == ZYDIS_MNEMONIC_LOOPE) go = go && get_flag(c, XC_ZF);
    if (m == ZYDIS_MNEMONIC_LOOPNE) go = go && !get_flag(c, XC_ZF);
    if (go) *next = x->ops[0].imm;
    return 1;
}

/* ------------------------------------------------------------- SSE / SSE2 */

static int is_xmm_op(const xop *op) { return op->type == XOP_REG && op->rcls == XR_XMM; }

static inline uint64_t f64_bits(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }
static inline double   bits_f64(uint64_t u) { double v; memcpy(&v, &u, 8); return v; }
static inline uint32_t f32_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
static inline float    bits_f32(uint32_t u) { float v; memcpy(&v, &u, 4); return v; }

/* MXCSR exception flags (sticky, bits 0..5). The host FPU raises the same
 * IEEE conditions we need for IE/ZE/OE/UE/PE, so those are read back through
 * fenv after each operation; DE (denormal input) has no fenv equivalent and
 * is computed from the inputs. `sse_flags` accumulates the manual ones for
 * the instruction in flight -- the interpreter is single-threaded per core. */
enum { MX_IE = 1, MX_DE = 2, MX_ZE = 4, MX_OE = 8, MX_UE = 16, MX_PE = 32 };
static uint32_t sse_flags;

static inline int den64(double v) { uint64_t u = f64_bits(v); return (u & 0x7FF0000000000000ull) == 0 && (u & 0x000FFFFFFFFFFFFFull) != 0; }
static inline int den32(float v)  { uint32_t u = f32_bits(v); return (u & 0x7F800000u) == 0 && (u & 0x007FFFFFu) != 0; }
static inline int snan64(double v) { return isnan(v) && !(f64_bits(v) & (1ull << 51)); }
static inline int snan32(float v)  { return isnan(v) && !(f32_bits(v) & (1u << 22)); }

static void fenv_begin(const xc_cpu *c) {
    sse_flags = 0;
    feclearexcept(FE_ALL_EXCEPT);
    static const int rc[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    if ((c->mxcsr >> 13) & 3) fesetround(rc[(c->mxcsr >> 13) & 3]);
}
static void fenv_end(xc_cpu *c) {
    int e = fetestexcept(FE_ALL_EXCEPT);
    uint32_t f = sse_flags;
    if (e & FE_INVALID)   f |= MX_IE;
    if (e & FE_DIVBYZERO) f |= MX_ZE;
    if (e & FE_OVERFLOW)  f |= MX_OE;
    if (e & FE_UNDERFLOW) f |= MX_UE;
    if (e & FE_INEXACT)   f |= MX_PE;
    c->mxcsr |= f;
    if ((c->mxcsr >> 13) & 3) fesetround(FE_TONEAREST);
}

/* x86 NaN rules for a two-operand SSE op (SDM 4.8.3.5): if either input is a
 * NaN the result is that input quieted (the first operand when both are).
 * An invalid operation on non-NaN inputs yields the x86 default NaN, which is
 * *negative* -- ARM64 hardware would produce a positive one, and the
 * difference would show up in golden replay. */
static double sse_fix64(double r, double a, double b) {
    if (den64(a) || den64(b)) sse_flags |= MX_DE;
    if (isnan(a)) return bits_f64(f64_bits(a) | (1ull << 51));
    if (isnan(b)) return bits_f64(f64_bits(b) | (1ull << 51));
    if (isnan(r)) return bits_f64(0xFFF8000000000000ull);
    return r;
}
static float sse_fix32(float r, float a, float b) {
    if (den32(a) || den32(b)) sse_flags |= MX_DE;
    if (isnan(a)) return bits_f32(f32_bits(a) | (1u << 22));
    if (isnan(b)) return bits_f32(f32_bits(b) | (1u << 22));
    if (isnan(r)) return bits_f32(0xFFC00000u);
    return r;
}
/* MIN/MAX: if either is NaN (IE, even for QNaN), or both are zero, the second
 * operand wins. */
static double sse_min64(double a, double b) { if (den64(a) || den64(b)) sse_flags |= MX_DE; if (isnan(a) || isnan(b)) { sse_flags |= MX_IE; return b; } return a == b ? b : (a < b ? a : b); }
static double sse_max64(double a, double b) { if (den64(a) || den64(b)) sse_flags |= MX_DE; if (isnan(a) || isnan(b)) { sse_flags |= MX_IE; return b; } return a == b ? b : (a > b ? a : b); }
static float  sse_min32(float a, float b)   { if (den32(a) || den32(b)) sse_flags |= MX_DE; if (isnan(a) || isnan(b)) { sse_flags |= MX_IE; return b; } return a == b ? b : (a < b ? a : b); }
static float  sse_max32(float a, float b)   { if (den32(a) || den32(b)) sse_flags |= MX_DE; if (isnan(a) || isnan(b)) { sse_flags |= MX_IE; return b; } return a == b ? b : (a > b ? a : b); }

/* sqrt of a negative: default NaN + IE */
static double sse_sqrt64(double a) {
    if (den64(a)) sse_flags |= MX_DE;
    if (isnan(a)) { if (snan64(a)) sse_flags |= MX_IE; return bits_f64(f64_bits(a) | (1ull << 51)); }
    if (a < 0) { sse_flags |= MX_IE; return bits_f64(0xFFF8000000000000ull); }
    return sqrt(a);
}
static float sse_sqrt32(float a) {
    if (den32(a)) sse_flags |= MX_DE;
    if (isnan(a)) { if (snan32(a)) sse_flags |= MX_IE; return bits_f32(f32_bits(a) | (1u << 22)); }
    if (a < 0) { sse_flags |= MX_IE; return bits_f32(0xFFC00000u); }
    return sqrtf(a);
}

/* Widening/narrowing conversions: SNaN -> IE, denormal -> DE, otherwise the
 * host cast (which raises OE/UE/PE for narrowing exactly as x86 does). */
static float sse_f64_to_f32(double v) {
    if (den64(v)) sse_flags |= MX_DE;
    if (isnan(v)) { if (snan64(v)) sse_flags |= MX_IE; return bits_f32((uint32_t)((f64_bits(v) >> 29) & 0x807FFFFFu) | 0x7FC00000u); }
    return (float)v;
}
static double sse_f32_to_f64(float v) {
    if (den32(v)) sse_flags |= MX_DE;
    if (isnan(v)) { if (snan32(v)) sse_flags |= MX_IE; return bits_f64(((uint64_t)(f32_bits(v) & 0x807FFFFFu) << 29) | 0x7FF8000000000000ull); }
    return (double)v;
}

/* Convert to integer with x86 semantics: out of range or NaN gives the
 * "integer indefinite" 0x8000... and IE; an inexact conversion sets PE.
 * `trunc` selects CVTT*; otherwise the current rounding mode. */
static int64_t sse_to_int(double v, int bits, int trunc) {
    int64_t indef = bits == 64 ? (int64_t)0x8000000000000000ull : (int64_t)(int32_t)0x80000000;
    if (isnan(v)) { sse_flags |= MX_IE; return indef; }
    double r = trunc ? __builtin_trunc(v) : nearbyint(v);
    int64_t out;
    if (bits == 64) {
        if (r < -9223372036854775808.0 || r >= 9223372036854775808.0) { sse_flags |= MX_IE; return indef; }
        out = (int64_t)r;
    } else {
        if (r < -2147483648.0 || r >= 2147483648.0) { sse_flags |= MX_IE; return indef; }
        out = (int64_t)(int32_t)(int64_t)r;
    }
    if (r != v) sse_flags |= MX_PE;
    return out;
}

/* COMISx / UCOMISx: ZF,PF,CF; OF,AF,SF cleared. COMISx signals IE on any NaN,
 * UCOMIS* only on an SNaN. */
static void fcmp_flags(xc_cpu *c, int unord, int lt, int eq) {
    c->rflags &= ~(uint64_t)XC_ARITH_FLAGS;
    if (unord) c->rflags |= XC_ZF | XC_PF | XC_CF;
    else if (eq) c->rflags |= XC_ZF;
    else if (lt) c->rflags |= XC_CF;
}
static void fcmp64(xc_cpu *c, double a, double b, int ordered) {
    if (den64(a) || den64(b)) sse_flags |= MX_DE;
    int un = isnan(a) || isnan(b);
    if (un && (ordered || snan64(a) || snan64(b))) sse_flags |= MX_IE;
    fcmp_flags(c, un, !un && a < b, !un && a == b);
}
static void fcmp32(xc_cpu *c, float a, float b, int ordered) {
    if (den32(a) || den32(b)) sse_flags |= MX_DE;
    int un = isnan(a) || isnan(b);
    if (un && (ordered || snan32(a) || snan32(b))) sse_flags |= MX_IE;
    fcmp_flags(c, un, !un && a < b, !un && a == b);
}

/* CMPPS/CMPPD/CMPSS/CMPSD predicate 0..7 */
static int fpred(int p, double a, double b) {
    int un = isnan(a) || isnan(b);
    if (den64(a) || den64(b)) sse_flags |= MX_DE;
    if (un && (((p & 3) == 1 || (p & 3) == 2) || snan64(a) || snan64(b))) sse_flags |= MX_IE;
    switch (p & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}

static int fpred32(int p, float a, float b) {
    if (den32(a) || den32(b)) sse_flags |= MX_DE;
    return fpred(p, isnan(a) ? (double)a : (double)a, (double)b);   /* exact widening */
}

static inline uint8_t  sat_u8(int32_t v)  { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static inline int8_t   sat_s8(int32_t v)  { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }
static inline uint16_t sat_u16(int32_t v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }
static inline int16_t  sat_s16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

/* One SSE instruction with an XMM destination. Returns 1 if handled (ok or
 * fault -- check x->stop), 0 if the mnemonic is not an SSE op we know. */
static int do_sse(ctx *x, ZydisMnemonic m, int *ok) {
    const xop *ops = x->ops;
    xc_cpu *c = x->c;
    lanes a, b, r;
    memset(&r, 0, sizeof r);
    /* Only floating-point cases touch the host FP environment (FENV/WRF); the
     * integer and move cases skip it -- feclearexcept/fetestexcept are libm
     * calls and would otherwise dominate PXOR. */
#define FENV() fenv_begin(c)
#define RD(i, dst) do { if (!op_read128(x, i, &(dst).q)) { *ok = 0; return 1; } } while (0)
#define WR(v, bits) do { *ok = op_write128(x, 0, (v).q, bits); return 1; } while (0)
#define WRF(v, bits) do { fenv_end(c); *ok = op_write128(x, 0, (v).q, bits); return 1; } while (0)
#define IMM(i) ((int)ops[i].imm)
    switch (m) {
    /* ---- moves ---- */
    case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPD: case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_MOVNTDQ: case ZYDIS_MNEMONIC_MOVNTPS: case ZYDIS_MNEMONIC_MOVNTPD:
    case ZYDIS_MNEMONIC_LDDQU:
        RD(1, a); WR(a, 128);
    case ZYDIS_MNEMONIC_MOVQ: RD(1, a); WR(a, 64);
    case ZYDIS_MNEMONIC_MOVD: RD(1, a); WR(a, 32);
    case ZYDIS_MNEMONIC_MOVSD: case ZYDIS_MNEMONIC_MOVSS: {
        int bits = m == ZYDIS_MNEMONIC_MOVSD ? 64 : 32;
        RD(1, a);
        if (is_xmm_op(&ops[0]) && is_xmm_op(&ops[1])) {          /* merge into the low lane */
            r.q = c->xmm[ops[0].ridx];
            if (bits == 64) r.q.lo = a.q.lo; else r.d[0] = a.d[0];
            WR(r, 128);
        }
        WR(a, bits);                                             /* load zero-extends, store writes low */
    }
    case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD:
        RD(1, a);
        if (is_xmm_op(&ops[0])) { r.q = c->xmm[ops[0].ridx]; r.q.lo = a.q.lo; WR(r, 128); }
        WR(a, 64);
    case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVHPD:
        RD(1, a);
        if (is_xmm_op(&ops[0])) { r.q = c->xmm[ops[0].ridx]; r.q.hi = a.q.lo; WR(r, 128); }
        r.q.lo = a.q.hi; WR(r, 64);
    case ZYDIS_MNEMONIC_MOVHLPS: RD(0, r); RD(1, a); r.q.lo = a.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_MOVLHPS: RD(0, r); RD(1, a); r.q.hi = a.q.lo; WR(r, 128);
    case ZYDIS_MNEMONIC_MOVDDUP: RD(1, a); r.q.lo = r.q.hi = a.q.lo; WR(r, 128);
    case ZYDIS_MNEMONIC_MOVMSKPS: RD(1, a); r.q.lo = (a.d[0] >> 31) | ((a.d[1] >> 31) << 1) | ((a.d[2] >> 31) << 2) | ((a.d[3] >> 31) << 3); WR(r, 32);
    case ZYDIS_MNEMONIC_MOVMSKPD: RD(1, a); r.q.lo = (a.q.lo >> 63) | ((a.q.hi >> 63) << 1); WR(r, 32);
    case ZYDIS_MNEMONIC_PMOVMSKB: RD(1, a); for (int i = 0; i < 16; i++) r.q.lo |= (uint64_t)(a.b[i] >> 7) << i; WR(r, 32);
    case ZYDIS_MNEMONIC_PEXTRW: RD(1, a); r.q.lo = a.w[IMM(2) & 7]; WR(r, 32);
    case ZYDIS_MNEMONIC_PINSRW: RD(0, r); RD(1, a); r.w[IMM(2) & 7] = (uint16_t)a.q.lo; WR(r, 128);

    /* ---- bitwise ---- */
    case ZYDIS_MNEMONIC_PXOR: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD:
        RD(0, a); RD(1, b); r.q.lo = a.q.lo ^ b.q.lo; r.q.hi = a.q.hi ^ b.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_POR: case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD:
        RD(0, a); RD(1, b); r.q.lo = a.q.lo | b.q.lo; r.q.hi = a.q.hi | b.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_PAND: case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD:
        RD(0, a); RD(1, b); r.q.lo = a.q.lo & b.q.lo; r.q.hi = a.q.hi & b.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_PANDN: case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_ANDNPD:
        RD(0, a); RD(1, b); r.q.lo = ~a.q.lo & b.q.lo; r.q.hi = ~a.q.hi & b.q.hi; WR(r, 128);

    /* ---- integer add/sub ---- */
    case ZYDIS_MNEMONIC_PADDB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)(a.b[i] + b.b[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PADDW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(a.w[i] + b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PADDD: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.d[i] = a.d[i] + b.d[i]; WR(r, 128);
    case ZYDIS_MNEMONIC_PADDQ: RD(0, a); RD(1, b); r.q.lo = a.q.lo + b.q.lo; r.q.hi = a.q.hi + b.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)(a.b[i] - b.b[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(a.w[i] - b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBD: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.d[i] = a.d[i] - b.d[i]; WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBQ: RD(0, a); RD(1, b); r.q.lo = a.q.lo - b.q.lo; r.q.hi = a.q.hi - b.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_PADDUSB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = sat_u8(a.b[i] + b.b[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PADDUSW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = sat_u16(a.w[i] + b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PADDSB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)sat_s8((int8_t)a.b[i] + (int8_t)b.b[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PADDSW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)sat_s16((int16_t)a.w[i] + (int16_t)b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBUSB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = sat_u8(a.b[i] - b.b[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBUSW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = sat_u16(a.w[i] - b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBSB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)sat_s8((int8_t)a.b[i] - (int8_t)b.b[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PSUBSW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)sat_s16((int16_t)a.w[i] - (int16_t)b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PAVGB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)((a.b[i] + b.b[i] + 1) >> 1); WR(r, 128);
    case ZYDIS_MNEMONIC_PAVGW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((a.w[i] + b.w[i] + 1) >> 1); WR(r, 128);
    case ZYDIS_MNEMONIC_PMINUB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; WR(r, 128);
    case ZYDIS_MNEMONIC_PMAXUB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; WR(r, 128);
    case ZYDIS_MNEMONIC_PMINSW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (int16_t)a.w[i] < (int16_t)b.w[i] ? a.w[i] : b.w[i]; WR(r, 128);
    case ZYDIS_MNEMONIC_PMAXSW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (int16_t)a.w[i] > (int16_t)b.w[i] ? a.w[i] : b.w[i]; WR(r, 128);
    case ZYDIS_MNEMONIC_PSADBW: {
        RD(0, a); RD(1, b);
        uint32_t s0 = 0, s1 = 0;
        for (int i = 0; i < 8; i++) { s0 += (uint32_t)abs(a.b[i] - b.b[i]); s1 += (uint32_t)abs(a.b[8 + i] - b.b[8 + i]); }
        r.q.lo = s0; r.q.hi = s1; WR(r, 128);
    }

    /* ---- compare ---- */
    case ZYDIS_MNEMONIC_PCMPEQB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = a.b[i] == b.b[i] ? 0xFF : 0; WR(r, 128);
    case ZYDIS_MNEMONIC_PCMPEQW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = a.w[i] == b.w[i] ? 0xFFFF : 0; WR(r, 128);
    case ZYDIS_MNEMONIC_PCMPEQD: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.d[i] = a.d[i] == b.d[i] ? 0xFFFFFFFFu : 0; WR(r, 128);
    case ZYDIS_MNEMONIC_PCMPGTB: RD(0, a); RD(1, b); for (int i = 0; i < 16; i++) r.b[i] = (int8_t)a.b[i] > (int8_t)b.b[i] ? 0xFF : 0; WR(r, 128);
    case ZYDIS_MNEMONIC_PCMPGTW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (int16_t)a.w[i] > (int16_t)b.w[i] ? 0xFFFF : 0; WR(r, 128);
    case ZYDIS_MNEMONIC_PCMPGTD: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.d[i] = (int32_t)a.d[i] > (int32_t)b.d[i] ? 0xFFFFFFFFu : 0; WR(r, 128);

    /* ---- multiply ---- */
    case ZYDIS_MNEMONIC_PMULLW:  RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((int32_t)(int16_t)a.w[i] * (int16_t)b.w[i]); WR(r, 128);
    case ZYDIS_MNEMONIC_PMULHW:  RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(((int32_t)(int16_t)a.w[i] * (int16_t)b.w[i]) >> 16); WR(r, 128);
    case ZYDIS_MNEMONIC_PMULHUW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16); WR(r, 128);
    case ZYDIS_MNEMONIC_PMULUDQ: RD(0, a); RD(1, b); r.q.lo = (uint64_t)a.d[0] * b.d[0]; r.q.hi = (uint64_t)a.d[2] * b.d[2]; WR(r, 128);
    case ZYDIS_MNEMONIC_PMADDWD:
        RD(0, a); RD(1, b);
        for (int i = 0; i < 4; i++)
            r.d[i] = (uint32_t)((int32_t)(int16_t)a.w[2 * i] * (int16_t)b.w[2 * i] + (int32_t)(int16_t)a.w[2 * i + 1] * (int16_t)b.w[2 * i + 1]);
        WR(r, 128);

    /* ---- shifts: count from imm8 or from the low 64 bits of an xmm/m128 ---- */
    case ZYDIS_MNEMONIC_PSLLW: case ZYDIS_MNEMONIC_PSLLD: case ZYDIS_MNEMONIC_PSLLQ:
    case ZYDIS_MNEMONIC_PSRLW: case ZYDIS_MNEMONIC_PSRLD: case ZYDIS_MNEMONIC_PSRLQ:
    case ZYDIS_MNEMONIC_PSRAW: case ZYDIS_MNEMONIC_PSRAD: {
        RD(0, a);
        uint64_t n;
        if (ops[1].type == XOP_IMM) n = ops[1].imm; else { RD(1, b); n = b.q.lo; }
        int left = m == ZYDIS_MNEMONIC_PSLLW || m == ZYDIS_MNEMONIC_PSLLD || m == ZYDIS_MNEMONIC_PSLLQ;
        int arith = m == ZYDIS_MNEMONIC_PSRAW || m == ZYDIS_MNEMONIC_PSRAD;
        if (m == ZYDIS_MNEMONIC_PSLLW || m == ZYDIS_MNEMONIC_PSRLW || m == ZYDIS_MNEMONIC_PSRAW) {
            for (int i = 0; i < 8; i++)
                r.w[i] = n > 15 ? (arith ? (uint16_t)((int16_t)a.w[i] >> 15) : 0)
                       : left ? (uint16_t)(a.w[i] << n) : arith ? (uint16_t)((int16_t)a.w[i] >> n) : (uint16_t)(a.w[i] >> n);
        } else if (m == ZYDIS_MNEMONIC_PSLLD || m == ZYDIS_MNEMONIC_PSRLD || m == ZYDIS_MNEMONIC_PSRAD) {
            for (int i = 0; i < 4; i++)
                r.d[i] = n > 31 ? (arith ? (uint32_t)((int32_t)a.d[i] >> 31) : 0)
                       : left ? a.d[i] << n : arith ? (uint32_t)((int32_t)a.d[i] >> n) : a.d[i] >> n;
        } else {
            r.q.lo = n > 63 ? 0 : left ? a.q.lo << n : a.q.lo >> n;
            r.q.hi = n > 63 ? 0 : left ? a.q.hi << n : a.q.hi >> n;
        }
        WR(r, 128);
    }
    case ZYDIS_MNEMONIC_PSLLDQ: case ZYDIS_MNEMONIC_PSRLDQ: {
        RD(0, a);
        int n = IMM(1); if (n > 16) n = 16;
        for (int i = 0; i < 16; i++) {
            int src = m == ZYDIS_MNEMONIC_PSLLDQ ? i - n : i + n;
            r.b[i] = (src >= 0 && src < 16) ? a.b[src] : 0;
        }
        WR(r, 128);
    }

    /* ---- shuffles / unpacks ---- */
    case ZYDIS_MNEMONIC_PSHUFD: { RD(1, a); int im = IMM(2); for (int i = 0; i < 4; i++) r.d[i] = a.d[(im >> (2 * i)) & 3]; WR(r, 128); }
    case ZYDIS_MNEMONIC_PSHUFLW: { RD(1, a); int im = IMM(2); for (int i = 0; i < 4; i++) r.w[i] = a.w[(im >> (2 * i)) & 3]; r.q.hi = a.q.hi; WR(r, 128); }
    case ZYDIS_MNEMONIC_PSHUFHW: { RD(1, a); int im = IMM(2); for (int i = 0; i < 4; i++) r.w[4 + i] = a.w[4 + ((im >> (2 * i)) & 3)]; r.q.lo = a.q.lo; WR(r, 128); }
    case ZYDIS_MNEMONIC_SHUFPS: {
        RD(0, a); RD(1, b); int im = IMM(2);
        r.d[0] = a.d[im & 3]; r.d[1] = a.d[(im >> 2) & 3]; r.d[2] = b.d[(im >> 4) & 3]; r.d[3] = b.d[(im >> 6) & 3];
        WR(r, 128);
    }
    case ZYDIS_MNEMONIC_SHUFPD: { RD(0, a); RD(1, b); int im = IMM(2); r.q.lo = (im & 1) ? a.q.hi : a.q.lo; r.q.hi = (im & 2) ? b.q.hi : b.q.lo; WR(r, 128); }
    case ZYDIS_MNEMONIC_PUNPCKLBW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; } WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKHBW: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[8 + i]; r.b[2 * i + 1] = b.b[8 + i]; } WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKLWD: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; } WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKHWD: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[4 + i]; r.w[2 * i + 1] = b.w[4 + i]; } WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKLDQ: case ZYDIS_MNEMONIC_UNPCKLPS:
        RD(0, a); RD(1, b); r.d[0] = a.d[0]; r.d[1] = b.d[0]; r.d[2] = a.d[1]; r.d[3] = b.d[1]; WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKHDQ: case ZYDIS_MNEMONIC_UNPCKHPS:
        RD(0, a); RD(1, b); r.d[0] = a.d[2]; r.d[1] = b.d[2]; r.d[2] = a.d[3]; r.d[3] = b.d[3]; WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKLQDQ: case ZYDIS_MNEMONIC_UNPCKLPD:
        RD(0, a); RD(1, b); r.q.lo = a.q.lo; r.q.hi = b.q.lo; WR(r, 128);
    case ZYDIS_MNEMONIC_PUNPCKHQDQ: case ZYDIS_MNEMONIC_UNPCKHPD:
        RD(0, a); RD(1, b); r.q.lo = a.q.hi; r.q.hi = b.q.hi; WR(r, 128);
    case ZYDIS_MNEMONIC_PACKUSWB: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) { r.b[i] = sat_u8((int16_t)a.w[i]); r.b[8 + i] = sat_u8((int16_t)b.w[i]); } WR(r, 128);
    case ZYDIS_MNEMONIC_PACKSSWB: RD(0, a); RD(1, b); for (int i = 0; i < 8; i++) { r.b[i] = (uint8_t)sat_s8((int16_t)a.w[i]); r.b[8 + i] = (uint8_t)sat_s8((int16_t)b.w[i]); } WR(r, 128);
    case ZYDIS_MNEMONIC_PACKSSDW: RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) { r.w[i] = (uint16_t)sat_s16((int32_t)a.d[i]); r.w[4 + i] = (uint16_t)sat_s16((int32_t)b.d[i]); } WR(r, 128);

    /* ---- scalar double ---- */
    case ZYDIS_MNEMONIC_ADDSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_fix64(r.e[0] + b.e[0], r.e[0], b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SUBSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_fix64(r.e[0] - b.e[0], r.e[0], b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MULSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_fix64(r.e[0] * b.e[0], r.e[0], b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_DIVSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_fix64(r.e[0] / b.e[0], r.e[0], b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MINSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_min64(r.e[0], b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MAXSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_max64(r.e[0], b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SQRTSD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_sqrt64(b.e[0]); WRF(r, 128);
    /* ---- scalar single ---- */
    case ZYDIS_MNEMONIC_ADDSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_fix32(r.f[0] + b.f[0], r.f[0], b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SUBSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_fix32(r.f[0] - b.f[0], r.f[0], b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MULSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_fix32(r.f[0] * b.f[0], r.f[0], b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_DIVSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_fix32(r.f[0] / b.f[0], r.f[0], b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MINSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_min32(r.f[0], b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MAXSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_max32(r.f[0], b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SQRTSS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_sqrt32(b.f[0]); WRF(r, 128);
    /* ---- packed double ---- */
    case ZYDIS_MNEMONIC_ADDPD: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_fix64(a.e[i] + b.e[i], a.e[i], b.e[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SUBPD: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_fix64(a.e[i] - b.e[i], a.e[i], b.e[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MULPD: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_fix64(a.e[i] * b.e[i], a.e[i], b.e[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_DIVPD: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_fix64(a.e[i] / b.e[i], a.e[i], b.e[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MINPD: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_min64(a.e[i], b.e[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MAXPD: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_max64(a.e[i], b.e[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SQRTPD: FENV(); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_sqrt64(b.e[i]); WRF(r, 128);
    /* ---- packed single ---- */
    case ZYDIS_MNEMONIC_ADDPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_fix32(a.f[i] + b.f[i], a.f[i], b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SUBPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_fix32(a.f[i] - b.f[i], a.f[i], b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MULPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_fix32(a.f[i] * b.f[i], a.f[i], b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_DIVPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_fix32(a.f[i] / b.f[i], a.f[i], b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MINPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_min32(a.f[i], b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_MAXPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_max32(a.f[i], b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_SQRTPS: FENV(); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = sse_sqrt32(b.f[i]); WRF(r, 128);

    /* ---- FP compares ---- */
    case ZYDIS_MNEMONIC_COMISD: case ZYDIS_MNEMONIC_UCOMISD:
        FENV(); RD(0, a); RD(1, b); fcmp64(c, a.e[0], b.e[0], m == ZYDIS_MNEMONIC_COMISD); *ok = 1; fenv_end(c); return 1;
    case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS:
        FENV(); RD(0, a); RD(1, b); fcmp32(c, a.f[0], b.f[0], m == ZYDIS_MNEMONIC_COMISS); *ok = 1; fenv_end(c); return 1;
    case ZYDIS_MNEMONIC_CMPSD: FENV(); RD(0, r); RD(1, b); r.q.lo = fpred(IMM(2), r.e[0], b.e[0]) ? ~0ull : 0; WRF(r, 128);
    case ZYDIS_MNEMONIC_CMPSS: FENV(); RD(0, r); RD(1, b); r.d[0] = fpred32(IMM(2), r.f[0], b.f[0]) ? ~0u : 0; WRF(r, 128);
    case ZYDIS_MNEMONIC_CMPPD: FENV(); RD(0, a); RD(1, b); r.q.lo = fpred(IMM(2), a.e[0], b.e[0]) ? ~0ull : 0; r.q.hi = fpred(IMM(2), a.e[1], b.e[1]) ? ~0ull : 0; WRF(r, 128);
    case ZYDIS_MNEMONIC_CMPPS: FENV(); RD(0, a); RD(1, b); for (int i = 0; i < 4; i++) r.d[i] = fpred32(IMM(2), a.f[i], b.f[i]) ? ~0u : 0; WRF(r, 128);

    /* ---- conversions ---- */
    case ZYDIS_MNEMONIC_CVTSI2SD: FENV(); RD(0, r); RD(1, b); r.e[0] = ops[1].size == 64 ? (double)(int64_t)b.q.lo : (double)(int32_t)b.d[0]; WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTSI2SS: FENV(); RD(0, r); RD(1, b); r.f[0] = ops[1].size == 64 ? (float)(int64_t)b.q.lo : (float)(int32_t)b.d[0]; WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_CVTSD2SI:
        FENV(); RD(1, b); r.q.lo = (uint64_t)sse_to_int(b.e[0], ops[0].size, m == ZYDIS_MNEMONIC_CVTTSD2SI); WRF(r, ops[0].size);
    case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI:
        FENV(); RD(1, b); r.q.lo = (uint64_t)sse_to_int((double)b.f[0], ops[0].size, m == ZYDIS_MNEMONIC_CVTTSS2SI); WRF(r, ops[0].size);
    case ZYDIS_MNEMONIC_CVTSD2SS: FENV(); RD(0, r); RD(1, b); r.f[0] = sse_f64_to_f32(b.e[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTSS2SD: FENV(); RD(0, r); RD(1, b); r.e[0] = sse_f32_to_f64(b.f[0]); WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTDQ2PS: FENV(); RD(1, b); for (int i = 0; i < 4; i++) r.f[i] = (float)(int32_t)b.d[i]; WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTDQ2PD: FENV(); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = (double)(int32_t)b.d[i]; WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTPS2DQ: case ZYDIS_MNEMONIC_CVTTPS2DQ:
        FENV(); RD(1, b); for (int i = 0; i < 4; i++) r.d[i] = (uint32_t)sse_to_int((double)b.f[i], 32, m == ZYDIS_MNEMONIC_CVTTPS2DQ); WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTPD2DQ: case ZYDIS_MNEMONIC_CVTTPD2DQ:
        FENV(); RD(1, b); for (int i = 0; i < 2; i++) r.d[i] = (uint32_t)sse_to_int(b.e[i], 32, m == ZYDIS_MNEMONIC_CVTTPD2DQ); WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTPS2PD: FENV(); RD(1, b); for (int i = 0; i < 2; i++) r.e[i] = sse_f32_to_f64(b.f[i]); WRF(r, 128);
    case ZYDIS_MNEMONIC_CVTPD2PS: FENV(); RD(1, b); for (int i = 0; i < 2; i++) r.f[i] = sse_f64_to_f32(b.e[i]); WRF(r, 128);

    /* ---- control ---- */
    case ZYDIS_MNEMONIC_LDMXCSR: { uint64_t v; *ok = op_read(x, 0, &v); if (*ok) c->mxcsr = (uint32_t)v; return 1; }
    case ZYDIS_MNEMONIC_STMXCSR: *ok = op_write(x, 0, c->mxcsr); return 1;

    default: return 0;
    }
#undef RD
#undef WR
#undef WRF
#undef FENV
#undef IMM
}


/* ------------------------------------------------- 32-bit-era leftovers */

/* The BCD adjust family. Nobody emits these on purpose any more, but 32-bit
 * Windows binaries carry them (compilers of the era, hand-written asm,
 * obfuscators), and each is a couple of lines against the SDM. */
static void do_daa_das(xc_cpu *c, int sub) {
    uint64_t al = c->gpr[XC_RAX] & 0xFF, old_al = al;
    int cf = get_flag(c, XC_CF), af = get_flag(c, XC_AF), ncf = 0;
    if ((al & 0xF) > 9 || af) {
        al = sub ? al - 6 : al + 6;
        ncf = cf || (sub ? old_al < 6 : (old_al + 6) > 0xFF);
        set_flag(c, XC_AF, 1);
    } else set_flag(c, XC_AF, 0);
    if (old_al > 0x99 || cf) { al = sub ? al - 0x60 : al + 0x60; ncf = 1; }
    al &= 0xFF;
    reg_write(c, ZYDIS_REGISTER_AL, al);
    set_flag(c, XC_CF, ncf);
    flags_zsp(c, al, 8);
}
static void do_aaa_aas(xc_cpu *c, int sub) {
    uint64_t ax = c->gpr[XC_RAX] & 0xFFFF;
    if ((ax & 0xF) > 9 || get_flag(c, XC_AF)) {
        ax = sub ? ax - 6 : ax + 6;
        ax = sub ? ax - 0x100 : ax + 0x100;
        set_flag(c, XC_AF, 1); set_flag(c, XC_CF, 1);
    } else { set_flag(c, XC_AF, 0); set_flag(c, XC_CF, 0); }
    ax &= 0xFF0F;
    reg_write(c, ZYDIS_REGISTER_AX, ax);
}
static int do_aam(ctx *x) {
    uint64_t base; op_read(x, 0, &base); base &= 0xFF;
    if (base == 0) { x->stop = XC_STOP_FAULT; return 0; }                 /* #DE */
    uint64_t al = x->c->gpr[XC_RAX] & 0xFF;
    uint64_t ax = ((al / base) << 8) | (al % base);
    reg_write(x->c, ZYDIS_REGISTER_AX, ax);
    flags_zsp(x->c, ax & 0xFF, 8);
    return 1;
}
static void do_aad(ctx *x) {
    uint64_t base; op_read(x, 0, &base); base &= 0xFF;
    uint64_t ax = x->c->gpr[XC_RAX] & 0xFFFF;
    uint64_t al = ((ax & 0xFF) + ((ax >> 8) & 0xFF) * base) & 0xFF;
    reg_write(x->c, ZYDIS_REGISTER_AX, al);
    flags_zsp(x->c, al, 8);
}

/* PUSHF/POPF at the instruction's operand size. POPF in user mode cannot
 * change IOPL/IF/VM etc.; we keep the arithmetic flags, DF, TF-free. */
static int do_pushf(ctx *x) {
    int w = x->in->operand_width;
    return push(x, mask_bits(x->c->rflags & 0x00FCFFFFu, w));           /* RF, VM read as 0 */
}
static int do_popf(ctx *x) {
    uint64_t v; if (!pop(x, &v)) return 0;
    const uint64_t m = XC_CF | XC_PF | XC_AF | XC_ZF | XC_SF | XC_DF | XC_OF;   /* what user mode may change */
    x->c->rflags = (x->c->rflags & ~m) | (mask_bits(v, x->in->operand_width) & m) | 0x2;
    return 1;
}

/* PUSHA/POPA (32-bit mode only): the eight registers in encoding order,
 * with the pushed ESP being its value before the instruction, and the
 * popped ESP slot discarded. */
static int do_pusha(ctx *x) {
    xc_cpu *c = x->c;
    uint64_t sp0 = mask_bits(c->gpr[XC_RSP], x->in->stack_width);
    static const int order[8] = { XC_RAX, XC_RCX, XC_RDX, XC_RBX, XC_RSP, XC_RBP, XC_RSI, XC_RDI };
    for (int i = 0; i < 8; i++) {
        uint64_t v = order[i] == XC_RSP ? sp0 : c->gpr[order[i]];
        if (!push(x, mask_bits(v, x->in->operand_width))) return 0;
    }
    return 1;
}
static int do_popa(ctx *x) {
    xc_cpu *c = x->c;
    static const int order[8] = { XC_RDI, XC_RSI, XC_RBP, XC_RSP, XC_RBX, XC_RDX, XC_RCX, XC_RAX };
    int w = x->in->operand_width;
    for (int i = 0; i < 8; i++) {
        uint64_t v; if (!pop(x, &v)) return 0;
        if (order[i] == XC_RSP) continue;
        if (w == 16) c->gpr[order[i]] = (c->gpr[order[i]] & ~0xFFFFull) | (v & 0xFFFF);
        else c->gpr[order[i]] = v & 0xFFFFFFFFull;
    }
    return 1;
}

/* ENTER imm16, imm8 -- the frame builder MSVC emitted for nested scopes. */
static int do_enter(ctx *x) {
    xc_cpu *c = x->c;
    uint64_t size, level; op_read(x, 0, &size); op_read(x, 1, &level);
    level &= 31;
    int sw = x->in->stack_width;
    if (!push(x, mask_bits(c->gpr[XC_RBP], sw))) return 0;
    uint64_t frame = mask_bits(c->gpr[XC_RSP], sw);
    for (uint64_t i = 1; i < level; i++) {
        uint64_t bp = mask_bits(c->gpr[XC_RBP] - i * (sw / 8), sw), v;
        if (!mem_read(x, bp, sw, &v) || !push(x, v)) return 0;
    }
    if (level > 0 && !push(x, frame)) return 0;
    c->gpr[XC_RBP] = sw == 64 ? frame : (frame & 0xFFFFFFFFull);
    c->gpr[XC_RSP] = mask_bits(c->gpr[XC_RSP] - size, sw);
    if (sw == 32) c->gpr[XC_RSP] &= 0xFFFFFFFFull;
    return 1;
}

/* CMPXCHG8B / CMPXCHG16B: compare EDX:EAX (RDX:RAX) with m64 (m128). */
static int do_cmpxchg8b(ctx *x, int bits) {
    xc_cpu *c = x->c;
    uint64_t a = ea(x, &x->ops[0]);
    int half = bits / 2;
    uint64_t lo, hi;
    if (!mem_read(x, a, half, &lo) || !mem_read(x, a + half / 8, half, &hi)) return 0;
    if (lo == mask_bits(c->gpr[XC_RAX], half) && hi == mask_bits(c->gpr[XC_RDX], half)) {
        set_flag(c, XC_ZF, 1);
        return mem_write(x, a, half, mask_bits(c->gpr[XC_RBX], half)) && mem_write(x, a + half / 8, half, mask_bits(c->gpr[XC_RCX], half));
    }
    set_flag(c, XC_ZF, 0);
    if (half == 32) { c->gpr[XC_RAX] = lo; c->gpr[XC_RDX] = hi; }          /* zero-extended, as on hardware */
    else { c->gpr[XC_RAX] = lo; c->gpr[XC_RDX] = hi; }
    return 1;
}

#include "interp_x87.h"

/* ------------------------------------------------------------------- step */

int xc_decode_at(xc_cpu *c, uint64_t rip, ZydisDecodedInstruction *in, ZydisDecodedOperand *ops);
xc_stop xc_exec_decoded(xc_cpu *c, const ZydisDecodedInstruction *in_, const xop *ops);

/* ------------------------------------------------------------------- step */

/* Fetch and decode the instruction at `rip`. Shared by xc_step and the block
 * cache. Returns 0 and sets c->stop on a fetch fault or undecodable bytes. */
int xc_decode_at(xc_cpu *c, uint64_t rip, ZydisDecodedInstruction *in, ZydisDecodedOperand *ops) {
    static ZydisDecoder dec64, dec32;
    static int inited;
    if (!inited) {
        ZydisDecoderInit(&dec64, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        ZydisDecoderInit(&dec32, ZYDIS_MACHINE_MODE_LONG_COMPAT_32, ZYDIS_STACK_WIDTH_32);
        inited = 1;
    }
    /* Up to 15 bytes, but do not read past the arena. */
    uint8_t buf[16]; size_t avail = 15;
    const uint8_t *src = 0;
    for (; avail > 0; --avail) {
        src = (const uint8_t *)xc_mem_ptr(c->mem, rip, avail);
        if (src) break;
    }
    if (!src) { c->stop = XC_STOP_FAULT; c->fault_addr = rip; return 0; }
    memcpy(buf, src, avail);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(c->mode == XC_MODE_64 ? &dec64 : &dec32, buf, avail, in, ops))) {
        c->stop = XC_STOP_DECODE; return 0;
    }
    return 1;
}

xc_stop xc_step(xc_cpu *c) {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand zops[ZYDIS_MAX_OPERAND_COUNT];
    xop ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!xc_decode_at(c, c->rip, &in, zops)) return c->stop;
    xop_convert(&in, zops, c->rip, c->mode, ops);
    return xc_exec_decoded(c, &in, ops);
}

/* Execute one already-decoded instruction at c->rip. */
xc_stop xc_exec_decoded(xc_cpu *c, const ZydisDecodedInstruction *in_, const xop *ops) {
#define in (*in_)
    ctx x = { c, in_, ops, c->rip + in.length, XC_STOP_NONE };
    uint64_t next = x.next_rip;
    int ok = 1;
    ZydisMnemonic m = in.mnemonic;

    switch (m) {
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_ENDBR32:
    /* CET shadow-stack ops are NOPs on hardware without CET enabled, which is
     * what CPUID here describes. glibc's setjmp/longjmp run them anyway. */
    case ZYDIS_MNEMONIC_RDSSPD: case ZYDIS_MNEMONIC_RDSSPQ: case ZYDIS_MNEMONIC_INCSSPD:
    case ZYDIS_MNEMONIC_INCSSPQ: case ZYDIS_MNEMONIC_SAVEPREVSSP: case ZYDIS_MNEMONIC_RSTORSSP:
    case ZYDIS_MNEMONIC_CLRSSBSY: case ZYDIS_MNEMONIC_SETSSBSY:
        break;

    case ZYDIS_MNEMONIC_MOV: {
        uint64_t v; ok = op_read(&x, 1, &v) && op_write(&x, 0, mask_bits(v, ops[0].size)); break;
    }
    case ZYDIS_MNEMONIC_MOVZX: {
        uint64_t v; ok = op_read(&x, 1, &v) && op_write(&x, 0, mask_bits(v, ops[1].size)); break;
    }
    case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: {
        uint64_t v; ok = op_read(&x, 1, &v) && op_write(&x, 0, mask_bits(sext(v, ops[1].size), ops[0].size)); break;
    }
    case ZYDIS_MNEMONIC_LEA:
        ok = op_write(&x, 0, mask_bits(ea(&x, &ops[1]), ops[0].size)); break;

    case ZYDIS_MNEMONIC_XCHG: {
        uint64_t a, b;
        ok = op_read(&x, 0, &a) && op_read(&x, 1, &b) && op_write(&x, 0, b) && op_write(&x, 1, a);
        break;
    }

    case ZYDIS_MNEMONIC_ADD:  ok = do_alu(&x, ALU_ADD);  break;
    case ZYDIS_MNEMONIC_ADC:  ok = do_alu(&x, ALU_ADC);  break;
    case ZYDIS_MNEMONIC_SUB:  ok = do_alu(&x, ALU_SUB);  break;
    case ZYDIS_MNEMONIC_SBB:  ok = do_alu(&x, ALU_SBB);  break;
    case ZYDIS_MNEMONIC_AND:  ok = do_alu(&x, ALU_AND);  break;
    case ZYDIS_MNEMONIC_OR:   ok = do_alu(&x, ALU_OR);   break;
    case ZYDIS_MNEMONIC_XOR:  ok = do_alu(&x, ALU_XOR);  break;
    case ZYDIS_MNEMONIC_CMP:  ok = do_alu(&x, ALU_CMP);  break;
    case ZYDIS_MNEMONIC_TEST: ok = do_alu(&x, ALU_TEST); break;
    case ZYDIS_MNEMONIC_INC:  ok = do_incdec(&x, 0); break;
    case ZYDIS_MNEMONIC_DEC:  ok = do_incdec(&x, 1); break;

    case ZYDIS_MNEMONIC_NEG: {
        uint64_t a; int bits = ops[0].size;
        if ((ok = op_read(&x, 0, &a))) {
            uint64_t r = mask_bits(0 - a, bits);
            flags_sub(c, 0, a, 0, r, bits);
            set_flag(c, XC_CF, mask_bits(a, bits) != 0);
            ok = op_write(&x, 0, r);
        }
        break;
    }
    case ZYDIS_MNEMONIC_NOT: {
        uint64_t a; ok = op_read(&x, 0, &a) && op_write(&x, 0, mask_bits(~a, ops[0].size)); break;
    }

    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR:
        ok = do_shift(&x, m); break;

    case ZYDIS_MNEMONIC_IMUL: ok = do_imul(&x); break;
    case ZYDIS_MNEMONIC_MUL:  ok = do_mul(&x);  break;
    case ZYDIS_MNEMONIC_DIV:  ok = do_div(&x, 0); break;
    case ZYDIS_MNEMONIC_IDIV: ok = do_div(&x, 1); break;

    /* sign-extension family */
    case ZYDIS_MNEMONIC_CBW:  reg_write(c, ZYDIS_REGISTER_AX,  sext(c->gpr[XC_RAX], 8));  break;
    case ZYDIS_MNEMONIC_CWDE: reg_write(c, ZYDIS_REGISTER_EAX, sext(c->gpr[XC_RAX], 16)); break;
    case ZYDIS_MNEMONIC_CDQE: reg_write(c, ZYDIS_REGISTER_RAX, sext(c->gpr[XC_RAX], 32)); break;
    case ZYDIS_MNEMONIC_CWD:  reg_write(c, ZYDIS_REGISTER_DX,  (c->gpr[XC_RAX] & 0x8000) ? 0xFFFF : 0); break;
    case ZYDIS_MNEMONIC_CDQ:  reg_write(c, ZYDIS_REGISTER_EDX, (c->gpr[XC_RAX] & 0x80000000u) ? 0xFFFFFFFFu : 0); break;
    case ZYDIS_MNEMONIC_CQO:  reg_write(c, ZYDIS_REGISTER_RDX, (c->gpr[XC_RAX] >> 63) ? ~0ull : 0); break;

    /* stack */
    case ZYDIS_MNEMONIC_PUSH: {
        uint64_t v; ok = op_read(&x, 0, &v) && push(&x, mask_bits(sext(v, ops[0].size), in.stack_width)); break;
    }
    case ZYDIS_MNEMONIC_POP: {
        uint64_t v; ok = pop(&x, &v) && op_write(&x, 0, v); break;
    }
    case ZYDIS_MNEMONIC_LEAVE: {
        int sw = in.stack_width;
        c->gpr[XC_RSP] = mask_bits(c->gpr[XC_RBP], sw);
        uint64_t v; if ((ok = pop(&x, &v))) c->gpr[XC_RBP] = sw == 64 ? v : (v & 0xFFFFFFFFull);
        break;
    }

    /* control flow */
    case ZYDIS_MNEMONIC_JMP: case ZYDIS_MNEMONIC_CALL: {
        uint64_t target;
        if (ops[0].type == XOP_IMM) {
            target = ops[0].imm;
        } else if (!(ok = op_read(&x, 0, &target))) break;
        if (m == ZYDIS_MNEMONIC_CALL && !(ok = push(&x, next))) break;
        next = mask_bits(target, c->mode);
        break;
    }
    case ZYDIS_MNEMONIC_RET: {
        uint64_t v; if ((ok = pop(&x, &v))) {
            next = mask_bits(v, c->mode);
            if (in.operand_count_visible == 1) {
                uint64_t imm; op_read(&x, 0, &imm);
                c->gpr[XC_RSP] = mask_bits(c->gpr[XC_RSP] + imm, in.stack_width);
            }
        }
        break;
    }
    case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JCXZ: {
        int w = m == ZYDIS_MNEMONIC_JRCXZ ? 64 : m == ZYDIS_MNEMONIC_JECXZ ? 32 : 16;
        if (mask_bits(c->gpr[XC_RCX], w) == 0) { next = ops[0].imm; }
        break;
    }

    case ZYDIS_MNEMONIC_HLT:  c->stop = XC_STOP_HLT; c->rip = next; return c->stop;
    case ZYDIS_MNEMONIC_INT3: c->stop = XC_STOP_BREAKPOINT; c->rip = next; return c->stop;
    case ZYDIS_MNEMONIC_UD2:  c->stop = XC_STOP_UNDEFINED; return c->stop;
    case ZYDIS_MNEMONIC_SYSCALL:
        c->syscall_vector = -1; c->stop = XC_STOP_SYSCALL; c->rip = next; return c->stop;
    case ZYDIS_MNEMONIC_INT: {
        uint64_t v; op_read(&x, 0, &v);
        c->syscall_vector = (int)v; c->stop = XC_STOP_SYSCALL; c->rip = next; return c->stop;
    }

    /* strings */
    case ZYDIS_MNEMONIC_MOVSB: ok = do_string(&x, 1, 8);  break;
    case ZYDIS_MNEMONIC_MOVSW: ok = do_string(&x, 1, 16); break;
    case ZYDIS_MNEMONIC_MOVSD:                         /* string form; the SSE form is below */
        if (in.meta.category != ZYDIS_CATEGORY_STRINGOP) goto sse;
        ok = do_string(&x, 1, 32); break;
    case ZYDIS_MNEMONIC_MOVSQ: ok = do_string(&x, 1, 64); break;
    case ZYDIS_MNEMONIC_STOSB: ok = do_string(&x, 0, 8);  break;
    case ZYDIS_MNEMONIC_STOSW: ok = do_string(&x, 0, 16); break;
    case ZYDIS_MNEMONIC_STOSD: ok = do_string(&x, 0, 32); break;
    case ZYDIS_MNEMONIC_STOSQ: ok = do_string(&x, 0, 64); break;

    case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSQ:
    case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: case ZYDIS_MNEMONIC_SCASQ:
    case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD: case ZYDIS_MNEMONIC_LODSQ:
        ok = do_string_cmp(&x, m, m == ZYDIS_MNEMONIC_CMPSB || m == ZYDIS_MNEMONIC_SCASB || m == ZYDIS_MNEMONIC_LODSB ? 8
                               : m == ZYDIS_MNEMONIC_CMPSW || m == ZYDIS_MNEMONIC_SCASW || m == ZYDIS_MNEMONIC_LODSW ? 16
                               : m == ZYDIS_MNEMONIC_SCASD || m == ZYDIS_MNEMONIC_LODSD ? 32 : 64);
        break;
    case ZYDIS_MNEMONIC_CMPSD:                         /* string form; the SSE form is below */
        if (in.meta.category != ZYDIS_CATEGORY_STRINGOP) goto sse;
        ok = do_string_cmp(&x, m, 32); break;

    /* 32-bit-era instructions (also legal in 64-bit mode where noted) */
    case ZYDIS_MNEMONIC_PUSHF: case ZYDIS_MNEMONIC_PUSHFD: case ZYDIS_MNEMONIC_PUSHFQ: ok = do_pushf(&x); break;
    case ZYDIS_MNEMONIC_POPF: case ZYDIS_MNEMONIC_POPFD: case ZYDIS_MNEMONIC_POPFQ: ok = do_popf(&x); break;
    case ZYDIS_MNEMONIC_PUSHA: case ZYDIS_MNEMONIC_PUSHAD: ok = do_pusha(&x); break;
    case ZYDIS_MNEMONIC_POPA: case ZYDIS_MNEMONIC_POPAD: ok = do_popa(&x); break;
    case ZYDIS_MNEMONIC_DAA: do_daa_das(c, 0); break;
    case ZYDIS_MNEMONIC_DAS: do_daa_das(c, 1); break;
    case ZYDIS_MNEMONIC_AAA: do_aaa_aas(c, 0); break;
    case ZYDIS_MNEMONIC_AAS: do_aaa_aas(c, 1); break;
    case ZYDIS_MNEMONIC_AAM: ok = do_aam(&x); break;
    case ZYDIS_MNEMONIC_AAD: do_aad(&x); break;
    case ZYDIS_MNEMONIC_XLAT: {
        uint64_t a = mask_bits(c->gpr[XC_RBX] + (c->gpr[XC_RAX] & 0xFF), in.address_width), v;
        if ((ok = mem_read(&x, a, 8, &v))) reg_write(c, ZYDIS_REGISTER_AL, v);
        break;
    }
    case ZYDIS_MNEMONIC_SALC: reg_write(c, ZYDIS_REGISTER_AL, get_flag(c, XC_CF) ? 0xFF : 0); break;
    case ZYDIS_MNEMONIC_ENTER: ok = do_enter(&x); break;
    case ZYDIS_MNEMONIC_CMPXCHG8B: ok = do_cmpxchg8b(&x, 64); break;
    case ZYDIS_MNEMONIC_CMPXCHG16B: ok = do_cmpxchg8b(&x, 128); break;
    case ZYDIS_MNEMONIC_SYSENTER:
        c->syscall_vector = -2; c->stop = XC_STOP_SYSCALL; c->rip = next; return c->stop;

    /* flags */
    case ZYDIS_MNEMONIC_CLC: set_flag(c, XC_CF, 0); break;
    case ZYDIS_MNEMONIC_STC: set_flag(c, XC_CF, 1); break;
    case ZYDIS_MNEMONIC_CMC: c->rflags ^= XC_CF; break;
    case ZYDIS_MNEMONIC_CLD: set_flag(c, XC_DF, 0); break;
    case ZYDIS_MNEMONIC_STD: set_flag(c, XC_DF, 1); break;
    case ZYDIS_MNEMONIC_LAHF: reg_write(c, ZYDIS_REGISTER_AH, (c->rflags & 0xD5) | 0x02); break;
    case ZYDIS_MNEMONIC_SAHF: c->rflags = (c->rflags & ~0xD5ull) | (reg_read(c, ZYDIS_REGISTER_AH) & 0xD5); break;

    /* bits */
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC:
        ok = do_bt(&x, m); break;
    case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_TZCNT: case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_POPCNT:
        ok = do_bitscan(&x, m); break;
    case ZYDIS_MNEMONIC_BSWAP: {
        uint64_t v = xreg_read(c, &ops[0]);
        if (ops[0].size == 64) v = __builtin_bswap64(v); else v = __builtin_bswap32((uint32_t)v);
        xreg_write(c, &ops[0], v); break;
    }
    case ZYDIS_MNEMONIC_SHLD: ok = do_shd(&x, 1); break;
    case ZYDIS_MNEMONIC_SHRD: ok = do_shd(&x, 0); break;
    case ZYDIS_MNEMONIC_RCL:  ok = do_rc(&x, 1); break;
    case ZYDIS_MNEMONIC_RCR:  ok = do_rc(&x, 0); break;

    /* atomics -- a single core, so plain read-modify-write is atomic enough */
    case ZYDIS_MNEMONIC_CMPXCHG: ok = do_cmpxchg(&x); break;
    case ZYDIS_MNEMONIC_XADD:    ok = do_xadd(&x); break;

    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
        ok = do_loop(&x, m, &next); break;

    /* system-ish */
    case ZYDIS_MNEMONIC_CPUID: do_cpuid(c); break;
    case ZYDIS_MNEMONIC_RDTSC: {
        uint64_t t = ++c->tsc * 64;                   /* monotonic, deterministic */
        c->gpr[XC_RAX] = (uint32_t)t; c->gpr[XC_RDX] = t >> 32; break;
    }
    case ZYDIS_MNEMONIC_MOVNTI: { uint64_t v; ok = op_read(&x, 1, &v) && op_write(&x, 0, v); break; }
    case ZYDIS_MNEMONIC_PREFETCHT0: case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHW: case ZYDIS_MNEMONIC_PREFETCH:
    case ZYDIS_MNEMONIC_SFENCE: case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_MFENCE:
    case ZYDIS_MNEMONIC_CLFLUSH: case ZYDIS_MNEMONIC_FWAIT:
        break;

    default: {
        cc_t cc;
        if ((cc = cc_jcc(m)) != CC_NONE) {
            if (cc_eval(c, cc)) { next = ops[0].imm; }
        } else if ((cc = cc_setcc(m)) != CC_NONE) {
            ok = op_write(&x, 0, cc_eval(c, cc));
        } else if ((cc = cc_cmov(m)) != CC_NONE) {
            /* A 32-bit CMOV zero-extends the destination even when the move
             * does not happen -- writing the destination back to itself
             * reproduces that. */
            uint64_t s, d;
            if ((ok = op_read(&x, 1, &s) && op_read(&x, 0, &d)))
                ok = op_write(&x, 0, cc_eval(c, cc) ? s : d);
        } else {
        sse:
            if (!do_sse(&x, m, &ok) && !do_x87(&x, m, &ok)) {
                c->stop = XC_STOP_UNDEFINED;
                return c->stop;
            }
        }
    }
    }

    if (!ok) { if (x.stop == XC_STOP_NONE) x.stop = XC_STOP_FAULT; c->stop = x.stop; return c->stop; }
    c->rip = next;
    c->stop = XC_STOP_NONE;
    return XC_STOP_NONE;
#undef in
}

void xc_cpu_init(xc_cpu *c, xc_mode mode, xc_mem *mem) {
    memset(c, 0, sizeof *c);
    c->mode = mode;
    c->mem = mem;
    c->rflags = 0x2;                                   /* reserved bit 1 always set */
    c->mxcsr = 0x1F80;                                 /* all exceptions masked, round to nearest */
    x87_init(c);
}

const char *xc_stop_name(xc_stop s) {
    switch (s) {
    case XC_STOP_NONE: return "none";           case XC_STOP_STEPS: return "steps";
    case XC_STOP_HLT: return "hlt";             case XC_STOP_SYSCALL: return "syscall";
    case XC_STOP_BREAKPOINT: return "int3";     case XC_STOP_UNDEFINED: return "undefined";
    case XC_STOP_DECODE: return "decode";       case XC_STOP_FAULT: return "fault";
    }
    return "?";
}

int xc_disasm(const xc_cpu *c, uint64_t rip, char *buf, size_t buflen) {
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, c->mode == XC_MODE_64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LONG_COMPAT_32,
                     c->mode == XC_MODE_64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
    uint8_t code[16]; size_t avail = 15;
    const uint8_t *src = 0;
    for (; avail > 0; --avail) if ((src = (const uint8_t *)xc_mem_ptr(c->mem, rip, avail))) break;
    if (!src) { snprintf(buf, buflen, "<unmapped>"); return 0; }
    memcpy(code, src, avail);
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code, avail, &in, ops))) { snprintf(buf, buflen, "<bad opcode>"); return 0; }
    ZydisFormatter f;
    ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterFormatInstruction(&f, &in, ops, in.operand_count_visible, buf, buflen, rip, ZYAN_NULL);
    return (int)in.length;
}
