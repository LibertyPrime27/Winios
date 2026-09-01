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
    if (cls == ZYDIS_REGCLASS_SEGMENT) return 0;                 /* flat */
    int hi; int i = gpr_index(r, &hi);
    uint64_t v = c->gpr[i];
    if (hi) v >>= 8;
    return mask_bits(v, reg_bits(c, r));
}

static void reg_write(xc_cpu *c, ZydisRegister r, uint64_t v) {
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

/* ----------------------------------------------------------------- memory */

typedef struct {
    xc_cpu *c;
    const ZydisDecodedInstruction *in;
    const ZydisDecodedOperand *ops;
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

static uint64_t ea(ctx *x, const ZydisDecodedOperand *op) {
    const xc_cpu *c = x->c;
    uint64_t a = 0;
    if (op->mem.base != ZYDIS_REGISTER_NONE) {
        a += (op->mem.base == ZYDIS_REGISTER_RIP || op->mem.base == ZYDIS_REGISTER_EIP)
             ? x->next_rip : reg_read(c, op->mem.base);
    }
    if (op->mem.index != ZYDIS_REGISTER_NONE)
        a += reg_read(c, op->mem.index) * op->mem.scale;
    a += (uint64_t)op->mem.disp.value;
    a = mask_bits(a, x->in->address_width);
    if (c->mode == XC_MODE_64) {
        if (op->mem.segment == ZYDIS_REGISTER_FS) a += c->fs_base;
        else if (op->mem.segment == ZYDIS_REGISTER_GS) a += c->gs_base;
    }
    return a;
}

/* --------------------------------------------------------------- operands */

static int op_read(ctx *x, int i, uint64_t *out) {
    const ZydisDecodedOperand *op = &x->ops[i];
    switch (op->type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        *out = reg_read(x->c, op->reg.value); return 1;
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return mem_read(x, ea(x, op), op->size, out);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        *out = op->imm.is_signed ? (uint64_t)op->imm.value.s : op->imm.value.u; return 1;
    default:
        x->stop = XC_STOP_UNDEFINED; return 0;
    }
}

static int op_write(ctx *x, int i, uint64_t v) {
    const ZydisDecodedOperand *op = &x->ops[i];
    switch (op->type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        reg_write(x->c, op->reg.value, v); return 1;
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return mem_write(x, ea(x, op), op->size, v);
    default:
        x->stop = XC_STOP_UNDEFINED; return 0;
    }
}

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

/* ------------------------------------------------------------------- step */

xc_stop xc_step(xc_cpu *c) {
    static ZydisDecoder dec64, dec32;
    static int inited;
    if (!inited) {
        ZydisDecoderInit(&dec64, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        ZydisDecoderInit(&dec32, ZYDIS_MACHINE_MODE_LONG_COMPAT_32, ZYDIS_STACK_WIDTH_32);
        inited = 1;
    }

    /* Fetch. Up to 15 bytes, but do not read past the arena. */
    uint8_t buf[16]; size_t avail = 15;
    const uint8_t *src = 0;
    for (; avail > 0; --avail) {
        src = (const uint8_t *)xc_mem_ptr(c->mem, c->rip, avail);
        if (src) break;
    }
    if (!src) { c->stop = XC_STOP_FAULT; c->fault_addr = c->rip; return c->stop; }
    memcpy(buf, src, avail);

    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(c->mode == XC_MODE_64 ? &dec64 : &dec32,
                                             buf, avail, &in, ops))) {
        c->stop = XC_STOP_DECODE; return c->stop;
    }

    ctx x = { c, &in, ops, c->rip + in.length, XC_STOP_NONE };
    uint64_t next = x.next_rip;
    int ok = 1;
    ZydisMnemonic m = in.mnemonic;

    switch (m) {
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_ENDBR32:
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
        if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            ZyanU64 t; ZydisCalcAbsoluteAddress(&in, &ops[0], c->rip, &t); target = t;
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
        if (mask_bits(c->gpr[XC_RCX], w) == 0) { ZyanU64 t; ZydisCalcAbsoluteAddress(&in, &ops[0], c->rip, &t); next = t; }
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
    case ZYDIS_MNEMONIC_MOVSD: ok = do_string(&x, 1, 32); break;
    case ZYDIS_MNEMONIC_MOVSQ: ok = do_string(&x, 1, 64); break;
    case ZYDIS_MNEMONIC_STOSB: ok = do_string(&x, 0, 8);  break;
    case ZYDIS_MNEMONIC_STOSW: ok = do_string(&x, 0, 16); break;
    case ZYDIS_MNEMONIC_STOSD: ok = do_string(&x, 0, 32); break;
    case ZYDIS_MNEMONIC_STOSQ: ok = do_string(&x, 0, 64); break;

    default: {
        cc_t cc;
        if ((cc = cc_jcc(m)) != CC_NONE) {
            if (cc_eval(c, cc)) { ZyanU64 t; ZydisCalcAbsoluteAddress(&in, &ops[0], c->rip, &t); next = t; }
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
            c->stop = XC_STOP_UNDEFINED;
            return c->stop;
        }
    }
    }

    if (!ok) { if (x.stop == XC_STOP_NONE) x.stop = XC_STOP_FAULT; c->stop = x.stop; return c->stop; }
    c->rip = next;
    c->stop = XC_STOP_NONE;
    return XC_STOP_NONE;
}

xc_stop xc_run(xc_cpu *c, uint64_t max_steps) {
    while (max_steps--) {
        xc_stop s = xc_step(c);
        if (s != XC_STOP_NONE) return s;
    }
    c->stop = XC_STOP_STEPS;
    return XC_STOP_STEPS;
}

void xc_cpu_init(xc_cpu *c, xc_mode mode, xc_mem *mem) {
    memset(c, 0, sizeof *c);
    c->mode = mode;
    c->mem = mem;
    c->rflags = 0x2;                                   /* reserved bit 1 always set */
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
