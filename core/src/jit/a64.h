/* a64 -- a small AArch64 instruction encoder.
 *
 * Just the instructions the dynarec emits, as functions appending 32-bit
 * words to a buffer. Register numbers are 0-30; 31 is SP or ZR by context.
 * `sf` selects the 64-bit form (1) or 32-bit (0). Nothing here is clever:
 * each function is one encoding table row from the ARM ARM, and
 * tests/test_a64.c checks them against the system disassembler.
 */
#ifndef XCORE_A64_H
#define XCORE_A64_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t *buf;
    uint32_t  n, cap;
    int       overflow;
} a64;

static inline void a64_emit(a64 *a, uint32_t w) {
    if (a->n < a->cap) a->buf[a->n] = w; else a->overflow = 1;
    a->n++;
}
static inline uint32_t a64_here(const a64 *a) { return a->n; }

enum { ZR = 31, SP = 31 };
/* condition codes */
enum { CC_EQ = 0, CC_NE, CC_HS, CC_LO, CC_MI, CC_PL, CC_VS, CC_VC, CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL, CC_NV };
enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3 };
enum { EXT_UXTB = 0, EXT_UXTH, EXT_UXTW, EXT_UXTX, EXT_SXTB, EXT_SXTH, EXT_SXTW, EXT_SXTX };

/* ---- moves / immediates ---- */
static inline void a64_movz(a64 *a, int sf, int rd, uint16_t imm, int shift16) { a64_emit(a, (sf << 31) | 0x52800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | rd); }
static inline void a64_movk(a64 *a, int sf, int rd, uint16_t imm, int shift16) { a64_emit(a, (sf << 31) | 0x72800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | rd); }
static inline void a64_movn(a64 *a, int sf, int rd, uint16_t imm, int shift16) { a64_emit(a, (sf << 31) | 0x12800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | rd); }

/* Load a 64-bit constant with the fewest movz/movk (or movn). */
static inline void a64_mov_imm(a64 *a, int rd, uint64_t v) {
    if ((v >> 32) == 0) {
        a64_movz(a, 0, rd, (uint16_t)v, 0);
        if (v >> 16) a64_movk(a, 0, rd, (uint16_t)(v >> 16), 1);
        return;
    }
    if (~v >> 32 == 0 && (uint32_t)~v >> 16 == 0) { a64_movn(a, 1, rd, (uint16_t)~v, 0); return; }
    int first = 1;
    for (int i = 0; i < 4; i++) {
        uint16_t h = (uint16_t)(v >> (16 * i));
        if (!h && !(first && i == 3)) continue;
        if (first) { a64_movz(a, 1, rd, h, i); first = 0; } else a64_movk(a, 1, rd, h, i);
    }
}

/* Always four words (patchable in place). */
static inline void a64_mov_imm4(a64 *a, int rd, uint64_t v) {
    a64_movz(a, 1, rd, (uint16_t)v, 0);
    a64_movk(a, 1, rd, (uint16_t)(v >> 16), 1);
    a64_movk(a, 1, rd, (uint16_t)(v >> 32), 2);
    a64_movk(a, 1, rd, (uint16_t)(v >> 48), 3);
}

/* ---- arithmetic (shifted register) ----  op: 0 add, 1 adds, 2 sub, 3 subs */
static inline void a64_addsub_reg(a64 *a, int sf, int op, int rd, int rn, int rm, int shift, int amount) {
    a64_emit(a, (sf << 31) | ((op & 2) << 29) | ((op & 1) << 29) | 0x0B000000u | (shift << 22) | (rm << 16) | (amount << 10) | (rn << 5) | rd);
}
static inline void a64_add(a64 *a, int sf, int rd, int rn, int rm)  { a64_addsub_reg(a, sf, 0, rd, rn, rm, 0, 0); }
static inline void a64_adds(a64 *a, int sf, int rd, int rn, int rm) { a64_addsub_reg(a, sf, 1, rd, rn, rm, 0, 0); }
static inline void a64_sub(a64 *a, int sf, int rd, int rn, int rm)  { a64_addsub_reg(a, sf, 2, rd, rn, rm, 0, 0); }
static inline void a64_subs(a64 *a, int sf, int rd, int rn, int rm) { a64_addsub_reg(a, sf, 3, rd, rn, rm, 0, 0); }
static inline void a64_cmp(a64 *a, int sf, int rn, int rm) { a64_subs(a, sf, ZR, rn, rm); }
static inline void a64_add_shifted(a64 *a, int sf, int rd, int rn, int rm, int shift, int amt) { a64_addsub_reg(a, sf, 0, rd, rn, rm, shift, amt); }
/* extended register: add xd, xn, wm, uxtw  (option 010) */
static inline void a64_add_ext(a64 *a, int sf, int rd, int rn, int rm, int ext, int shift) {
    a64_emit(a, (sf << 31) | 0x0B200000u | (rm << 16) | (ext << 13) | (shift << 10) | (rn << 5) | rd);
}
/* immediate (0..4095, optionally <<12). op as above */
static inline void a64_addsub_imm(a64 *a, int sf, int op, int rd, int rn, uint32_t imm12, int sh12) {
    a64_emit(a, (sf << 31) | ((op & 2) << 29) | ((op & 1) << 29) | 0x11000000u | (sh12 << 22) | (imm12 << 10) | (rn << 5) | rd);
}
static inline void a64_add_imm(a64 *a, int sf, int rd, int rn, uint32_t imm)  { a64_addsub_imm(a, sf, 0, rd, rn, imm, 0); }
static inline void a64_adds_imm(a64 *a, int sf, int rd, int rn, uint32_t imm) { a64_addsub_imm(a, sf, 1, rd, rn, imm, 0); }
static inline void a64_sub_imm(a64 *a, int sf, int rd, int rn, uint32_t imm)  { a64_addsub_imm(a, sf, 2, rd, rn, imm, 0); }
static inline void a64_subs_imm(a64 *a, int sf, int rd, int rn, uint32_t imm) { a64_addsub_imm(a, sf, 3, rd, rn, imm, 0); }
static inline void a64_cmp_imm(a64 *a, int sf, int rn, uint32_t imm) { a64_subs_imm(a, sf, ZR, rn, imm); }
/* add/sub with carry */
static inline void a64_adcs(a64 *a, int sf, int rd, int rn, int rm) { a64_emit(a, (sf << 31) | 0x3A000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_sbcs(a64 *a, int sf, int rd, int rn, int rm) { a64_emit(a, (sf << 31) | 0x7A000000u | (rm << 16) | (rn << 5) | rd); }

/* ---- logical (shifted register) ---- opc: 0 and, 1 orr, 2 eor, 3 ands; N=1 for bic/orn/eon/bics */
static inline void a64_logic_reg(a64 *a, int sf, int opc, int N, int rd, int rn, int rm, int shift, int amt) {
    a64_emit(a, (sf << 31) | (opc << 29) | 0x0A000000u | (shift << 22) | (N << 21) | (rm << 16) | (amt << 10) | (rn << 5) | rd);
}
static inline void a64_and(a64 *a, int sf, int rd, int rn, int rm)  { a64_logic_reg(a, sf, 0, 0, rd, rn, rm, 0, 0); }
static inline void a64_orr(a64 *a, int sf, int rd, int rn, int rm)  { a64_logic_reg(a, sf, 1, 0, rd, rn, rm, 0, 0); }
static inline void a64_eor(a64 *a, int sf, int rd, int rn, int rm)  { a64_logic_reg(a, sf, 2, 0, rd, rn, rm, 0, 0); }
static inline void a64_ands(a64 *a, int sf, int rd, int rn, int rm) { a64_logic_reg(a, sf, 3, 0, rd, rn, rm, 0, 0); }
static inline void a64_orn(a64 *a, int sf, int rd, int rn, int rm)  { a64_logic_reg(a, sf, 1, 1, rd, rn, rm, 0, 0); }
static inline void a64_mvn(a64 *a, int sf, int rd, int rm) { a64_orn(a, sf, rd, ZR, rm); }
static inline void a64_mov_reg(a64 *a, int sf, int rd, int rm) { a64_orr(a, sf, rd, ZR, rm); }
static inline void a64_orr_shifted(a64 *a, int sf, int rd, int rn, int rm, int shift, int amt) { a64_logic_reg(a, sf, 1, 0, rd, rn, rm, shift, amt); }
static inline void a64_tst(a64 *a, int sf, int rn, int rm) { a64_ands(a, sf, ZR, rn, rm); }

/* ---- bitfield ---- (opc: 0 sbfm, 1 bfm, 2 ubfm) */
static inline void a64_bfm(a64 *a, int sf, int opc, int rd, int rn, int immr, int imms) {
    a64_emit(a, (sf << 31) | (opc << 29) | 0x13000000u | (sf << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}
static inline void a64_ubfx(a64 *a, int sf, int rd, int rn, int lsb, int width) { a64_bfm(a, sf, 2, rd, rn, lsb, lsb + width - 1); }
static inline void a64_sbfx(a64 *a, int sf, int rd, int rn, int lsb, int width) { a64_bfm(a, sf, 0, rd, rn, lsb, lsb + width - 1); }
/* bfi rd, rn, #lsb, #width */
static inline void a64_bfi(a64 *a, int sf, int rd, int rn, int lsb, int width) { int regsz = sf ? 64 : 32; a64_bfm(a, sf, 1, rd, rn, (regsz - lsb) % regsz, width - 1); }
static inline void a64_lsl_imm(a64 *a, int sf, int rd, int rn, int sh) { int regsz = sf ? 64 : 32; a64_bfm(a, sf, 2, rd, rn, (regsz - sh) % regsz, regsz - 1 - sh); }
static inline void a64_lsr_imm(a64 *a, int sf, int rd, int rn, int sh) { a64_bfm(a, sf, 2, rd, rn, sh, sf ? 63 : 31); }
static inline void a64_asr_imm(a64 *a, int sf, int rd, int rn, int sh) { a64_bfm(a, sf, 0, rd, rn, sh, sf ? 63 : 31); }
static inline void a64_uxtb(a64 *a, int rd, int rn) { a64_ubfx(a, 0, rd, rn, 0, 8); }
static inline void a64_uxth(a64 *a, int rd, int rn) { a64_ubfx(a, 0, rd, rn, 0, 16); }
static inline void a64_sxtb(a64 *a, int sf, int rd, int rn) { a64_sbfx(a, sf, rd, rn, 0, 8); }
static inline void a64_sxth(a64 *a, int sf, int rd, int rn) { a64_sbfx(a, sf, rd, rn, 0, 16); }
static inline void a64_sxtw(a64 *a, int rd, int rn) { a64_sbfx(a, 1, rd, rn, 0, 32); }
/* extr rd, rn, rm, #lsb  (ror when rn == rm) */
static inline void a64_extr(a64 *a, int sf, int rd, int rn, int rm, int lsb) { a64_emit(a, (sf << 31) | 0x13800000u | (sf << 22) | (rm << 16) | (lsb << 10) | (rn << 5) | rd); }
static inline void a64_ror_imm(a64 *a, int sf, int rd, int rn, int sh) { a64_extr(a, sf, rd, rn, rn, sh); }
/* variable shifts: lslv/lsrv/asrv/rorv */
static inline void a64_shiftv(a64 *a, int sf, int op, int rd, int rn, int rm) { a64_emit(a, (sf << 31) | 0x1AC02000u | (rm << 16) | (op << 10) | (rn << 5) | rd); }

/* ---- byte reverse ---- rev wd/xd */
static inline void a64_rev(a64 *a, int sf, int rd, int rn) { a64_emit(a, sf ? (0xDAC00C00u | (rn << 5) | rd) : (0x5AC00800u | (rn << 5) | rd)); }

/* ---- multiply ---- */
static inline void a64_madd(a64 *a, int sf, int rd, int rn, int rm, int ra) { a64_emit(a, (sf << 31) | 0x1B000000u | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
static inline void a64_mul(a64 *a, int sf, int rd, int rn, int rm) { a64_madd(a, sf, rd, rn, rm, ZR); }
static inline void a64_smull(a64 *a, int rd, int rn, int rm) { a64_emit(a, 0x9B200000u | (rm << 16) | (ZR << 10) | (rn << 5) | rd); }
static inline void a64_smulh(a64 *a, int rd, int rn, int rm) { a64_emit(a, 0x9B400000u | (rm << 16) | (ZR << 10) | (rn << 5) | rd); }
static inline void a64_umulh(a64 *a, int rd, int rn, int rm) { a64_emit(a, 0x9BC00000u | (rm << 16) | (ZR << 10) | (rn << 5) | rd); }

/* ---- conditional ---- */
static inline void a64_csel(a64 *a, int sf, int rd, int rn, int rm, int cond) { a64_emit(a, (sf << 31) | 0x1A800000u | (rm << 16) | (cond << 12) | (rn << 5) | rd); }
static inline void a64_csinc(a64 *a, int sf, int rd, int rn, int rm, int cond) { a64_emit(a, (sf << 31) | 0x1A800400u | (rm << 16) | (cond << 12) | (rn << 5) | rd); }
static inline void a64_cset(a64 *a, int sf, int rd, int cond) { a64_csinc(a, sf, rd, ZR, ZR, cond ^ 1); }
static inline void a64_cinc(a64 *a, int sf, int rd, int rn, int cond) { a64_csinc(a, sf, rd, rn, rn, cond ^ 1); }

/* ---- loads / stores ---- size: 0 byte, 1 half, 2 word, 3 dword */
/* unsigned scaled immediate offset: ldr/str Rt, [Rn, #imm*size] */
static inline void a64_ldst_uimm(a64 *a, int size, int opc, int rt, int rn, uint32_t imm_scaled) {
    a64_emit(a, (size << 30) | 0x39000000u | (opc << 22) | (imm_scaled << 10) | (rn << 5) | rt);
}
static inline void a64_str_off(a64 *a, int size, int rt, int rn, uint32_t byteoff) { a64_ldst_uimm(a, size, 0, rt, rn, byteoff >> size); }
static inline void a64_ldr_off(a64 *a, int size, int rt, int rn, uint32_t byteoff) { a64_ldst_uimm(a, size, 1, rt, rn, byteoff >> size); }
/* sign-extending loads to 64-bit (ldrsb/ldrsh/ldrsw with opc=2), to 32-bit (opc=3) */
static inline void a64_ldrs_off(a64 *a, int size, int to64, int rt, int rn, uint32_t byteoff) { a64_ldst_uimm(a, size, to64 ? 2 : 3, rt, rn, byteoff >> size); }
/* register offset: ldr Rt, [Rn, Rm{, extend}]  option: 3 = LSL/x, 2 = uxtw, 6 = sxtw; S=0 (no scaling) */
static inline void a64_ldst_reg(a64 *a, int size, int opc, int rt, int rn, int rm, int option) {
    a64_emit(a, (size << 30) | 0x38200800u | (opc << 22) | (rm << 16) | (option << 13) | (rn << 5) | rt);
}
static inline void a64_ldr_reg(a64 *a, int size, int rt, int rn, int rm, int option) { a64_ldst_reg(a, size, 1, rt, rn, rm, option); }
static inline void a64_str_reg(a64 *a, int size, int rt, int rn, int rm, int option) { a64_ldst_reg(a, size, 0, rt, rn, rm, option); }
static inline void a64_ldrs_reg(a64 *a, int size, int to64, int rt, int rn, int rm, int option) { a64_ldst_reg(a, size, to64 ? 2 : 3, rt, rn, rm, option); }
/* pairs: stp/ldp Xt1, Xt2, [Xn, #imm7*8] ; pre/post-index variants */
static inline void a64_stp(a64 *a, int rt, int rt2, int rn, int imm) { a64_emit(a, 0xA9000000u | (((imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
static inline void a64_ldp(a64 *a, int rt, int rt2, int rn, int imm) { a64_emit(a, 0xA9400000u | (((imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
static inline void a64_stp_pre(a64 *a, int rt, int rt2, int rn, int imm) { a64_emit(a, 0xA9800000u | (((imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
static inline void a64_ldp_post(a64 *a, int rt, int rt2, int rn, int imm) { a64_emit(a, 0xA8C00000u | (((imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }

/* ---- branches ---- (offsets in instructions, patched later via a64_patch_*) */
static inline void a64_b(a64 *a, int32_t off) { a64_emit(a, 0x14000000u | ((uint32_t)off & 0x3FFFFFF)); }
static inline void a64_bcond(a64 *a, int cond, int32_t off) { a64_emit(a, 0x54000000u | (((uint32_t)off & 0x7FFFF) << 5) | cond); }
static inline void a64_cbz(a64 *a, int sf, int rt, int32_t off) { a64_emit(a, (sf << 31) | 0x34000000u | (((uint32_t)off & 0x7FFFF) << 5) | rt); }
static inline void a64_cbnz(a64 *a, int sf, int rt, int32_t off) { a64_emit(a, (sf << 31) | 0x35000000u | (((uint32_t)off & 0x7FFFF) << 5) | rt); }
static inline void a64_tbz(a64 *a, int rt, int bit, int32_t off) { a64_emit(a, 0x36000000u | (((bit >> 5) & 1) << 31) | ((bit & 31) << 19) | (((uint32_t)off & 0x3FFF) << 5) | rt); }
static inline void a64_tbnz(a64 *a, int rt, int bit, int32_t off) { a64_emit(a, 0x37000000u | (((bit >> 5) & 1) << 31) | ((bit & 31) << 19) | (((uint32_t)off & 0x3FFF) << 5) | rt); }
/* adr Xd, pc+off (bytes, +-1 MB) */
static inline void a64_adr(a64 *a, int rd, int32_t off) { a64_emit(a, 0x10000000u | (((uint32_t)off & 3) << 29) | ((((uint32_t)off >> 2) & 0x7FFFF) << 5) | rd); }
static inline void a64_br(a64 *a, int rn) { a64_emit(a, 0xD61F0000u | (rn << 5)); }
static inline void a64_blr(a64 *a, int rn) { a64_emit(a, 0xD63F0000u | (rn << 5)); }
static inline void a64_ret(a64 *a) { a64_emit(a, 0xD65F03C0u); }
static inline void a64_nop(a64 *a) { a64_emit(a, 0xD503201Fu); }
static inline void a64_brk(a64 *a, int imm) { a64_emit(a, 0xD4200000u | (imm << 5)); }

/* ---- SIMD & FP ----
 * Vector registers are numbered 0-31 like the others. `sz` picks the lane:
 * for integer ops 0 = 8-bit, 1 = 16, 2 = 32, 3 = 64; for FP ops 0 = single,
 * 1 = double. All vector forms are the 128-bit (Q=1) shape unless named
 * otherwise. */
/* loads/stores with unsigned scaled immediate: size 0 s, 1 d, 2 q */
static inline void a64_fldst_off(a64 *a, int fsz, int load, int rt, int rn, uint32_t byteoff) {
    static const uint32_t base[3] = { 0xBD000000u, 0xFD000000u, 0x3D800000u };
    a64_emit(a, base[fsz] | ((uint32_t)load << 22) | ((byteoff >> (2 + fsz)) << 10) | (rn << 5) | rt);
}
/* register offset: option 3 = LSL/x, 2 = uxtw */
static inline void a64_fldst_reg(a64 *a, int fsz, int load, int rt, int rn, int rm, int option) {
    static const uint32_t base[3] = { 0xBC200800u, 0xFC200800u, 0x3CA00800u };
    a64_emit(a, base[fsz] | ((uint32_t)load << 22) | (rm << 16) | (option << 13) | (rn << 5) | rt);
}
/* three-same: op = U<<5 | opcode; size = lane size (bits 22-23) */
static inline void a64_v3(a64 *a, int U, int size, int opcode, int rd, int rn, int rm) {
    a64_emit(a, 0x4E200400u | ((uint32_t)U << 29) | ((uint32_t)size << 22) | (rm << 16) | ((uint32_t)opcode << 11) | (rn << 5) | rd);
}
static inline void a64_vadd(a64 *a, int sz, int rd, int rn, int rm)   { a64_v3(a, 0, sz, 0x10, rd, rn, rm); }
static inline void a64_vsub(a64 *a, int sz, int rd, int rn, int rm)   { a64_v3(a, 1, sz, 0x10, rd, rn, rm); }
static inline void a64_vmul(a64 *a, int sz, int rd, int rn, int rm)   { a64_v3(a, 0, sz, 0x13, rd, rn, rm); }
static inline void a64_vcmeq(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 1, sz, 0x11, rd, rn, rm); }
static inline void a64_vcmgt(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 0, sz, 0x06, rd, rn, rm); }
static inline void a64_vcmhi(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 1, sz, 0x06, rd, rn, rm); }
static inline void a64_vuqadd(a64 *a, int sz, int rd, int rn, int rm) { a64_v3(a, 1, sz, 0x01, rd, rn, rm); }
static inline void a64_vsqadd(a64 *a, int sz, int rd, int rn, int rm) { a64_v3(a, 0, sz, 0x01, rd, rn, rm); }
static inline void a64_vuqsub(a64 *a, int sz, int rd, int rn, int rm) { a64_v3(a, 1, sz, 0x05, rd, rn, rm); }
static inline void a64_vsqsub(a64 *a, int sz, int rd, int rn, int rm) { a64_v3(a, 0, sz, 0x05, rd, rn, rm); }
static inline void a64_vsmax(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 0, sz, 0x0C, rd, rn, rm); }
static inline void a64_vsmin(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 0, sz, 0x0D, rd, rn, rm); }
static inline void a64_vumax(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 1, sz, 0x0C, rd, rn, rm); }
static inline void a64_vumin(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 1, sz, 0x0D, rd, rn, rm); }
static inline void a64_vurhadd(a64 *a, int sz, int rd, int rn, int rm){ a64_v3(a, 1, sz, 0x02, rd, rn, rm); }
static inline void a64_vushl(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 1, sz, 0x08, rd, rn, rm); }
static inline void a64_vsshl(a64 *a, int sz, int rd, int rn, int rm)  { a64_v3(a, 0, sz, 0x08, rd, rn, rm); }
static inline void a64_vand(a64 *a, int rd, int rn, int rm) { a64_v3(a, 0, 0, 0x03, rd, rn, rm); }
static inline void a64_vbic(a64 *a, int rd, int rn, int rm) { a64_v3(a, 0, 1, 0x03, rd, rn, rm); }
static inline void a64_vorr(a64 *a, int rd, int rn, int rm) { a64_v3(a, 0, 2, 0x03, rd, rn, rm); }
static inline void a64_vorn(a64 *a, int rd, int rn, int rm) { a64_v3(a, 0, 3, 0x03, rd, rn, rm); }
static inline void a64_veor(a64 *a, int rd, int rn, int rm) { a64_v3(a, 1, 0, 0x03, rd, rn, rm); }
static inline void a64_vbsl(a64 *a, int rd, int rn, int rm) { a64_v3(a, 1, 1, 0x03, rd, rn, rm); }
static inline void a64_vmov(a64 *a, int rd, int rn) { a64_vorr(a, rd, rn, rn); }
/* FP three-same: fsz 0 single (4s), 1 double (2d); size field = hi<<1 | fsz */
static inline void a64_vfadd(a64 *a, int fsz, int rd, int rn, int rm)  { a64_v3(a, 0, fsz, 0x1A, rd, rn, rm); }
static inline void a64_vfsub(a64 *a, int fsz, int rd, int rn, int rm)  { a64_v3(a, 0, 2 | fsz, 0x1A, rd, rn, rm); }
static inline void a64_vfmul(a64 *a, int fsz, int rd, int rn, int rm)  { a64_v3(a, 1, fsz, 0x1B, rd, rn, rm); }
static inline void a64_vfdiv(a64 *a, int fsz, int rd, int rn, int rm)  { a64_v3(a, 1, fsz, 0x1F, rd, rn, rm); }
static inline void a64_vfcmeq(a64 *a, int fsz, int rd, int rn, int rm) { a64_v3(a, 0, fsz, 0x1C, rd, rn, rm); }
static inline void a64_vfcmge(a64 *a, int fsz, int rd, int rn, int rm) { a64_v3(a, 1, fsz, 0x1C, rd, rn, rm); }
static inline void a64_vfcmgt(a64 *a, int fsz, int rd, int rn, int rm) { a64_v3(a, 1, 2 | fsz, 0x1C, rd, rn, rm); }
/* two-register misc: 0x4E200800 | U<<29 | size<<22 | opcode<<12 */
static inline void a64_v2(a64 *a, int Q, int U, int size, int opcode, int rd, int rn) {
    a64_emit(a, 0x0E200800u | ((uint32_t)Q << 30) | ((uint32_t)U << 29) | ((uint32_t)size << 22) | ((uint32_t)opcode << 12) | (rn << 5) | rd);
}
static inline void a64_vfsqrt(a64 *a, int fsz, int rd, int rn)  { a64_v2(a, 1, 1, 2 | fsz, 0x1F, rd, rn); }
static inline void a64_vfcvtzs(a64 *a, int fsz, int rd, int rn) { a64_v2(a, 1, 0, 2 | fsz, 0x1B, rd, rn); }
static inline void a64_vscvtf(a64 *a, int fsz, int rd, int rn)  { a64_v2(a, 1, 0, fsz, 0x1D, rd, rn); }
static inline void a64_vfrintx(a64 *a, int fsz, int rd, int rn) { a64_v2(a, 1, 1, fsz, 0x19, rd, rn); }
static inline void a64_vfcvtl(a64 *a, int rd, int rn)  { a64_v2(a, 0, 0, 1, 0x17, rd, rn); }     /* fcvtl vd.2d, vn.2s */
static inline void a64_vfcvtn(a64 *a, int rd, int rn)  { a64_v2(a, 0, 0, 1, 0x16, rd, rn); }     /* fcvtn vd.2s, vn.2d */
static inline void a64_vnot(a64 *a, int rd, int rn)    { a64_v2(a, 1, 1, 0, 0x05, rd, rn); }
static inline void a64_vneg(a64 *a, int sz, int rd, int rn) { a64_v2(a, 1, 1, sz, 0x0B, rd, rn); }
static inline void a64_vsqxtn(a64 *a, int hi, int sz, int rd, int rn)  { a64_v2(a, hi, 0, sz, 0x14, rd, rn); }   /* sz = destination lane size */
static inline void a64_vsqxtun(a64 *a, int hi, int sz, int rd, int rn) { a64_v2(a, hi, 1, sz, 0x12, rd, rn); }
static inline void a64_vuqxtn(a64 *a, int hi, int sz, int rd, int rn)  { a64_v2(a, hi, 1, sz, 0x14, rd, rn); }
static inline void a64_vxtn(a64 *a, int hi, int sz, int rd, int rn)    { a64_v2(a, hi, 0, sz, 0x12, rd, rn); }
/* across lanes: uminv sd, vn.4s */
static inline void a64_vuminv4s(a64 *a, int rd, int rn) { a64_emit(a, 0x6EB1A800u | (rn << 5) | rd); }
/* shift by immediate: 0x4F000400 | U<<29 | immh:immb<<16 | opcode<<11 */
static inline void a64_vshift_imm(a64 *a, int U, int opcode, int immhb, int rd, int rn) {
    a64_emit(a, 0x4F000400u | ((uint32_t)U << 29) | ((uint32_t)immhb << 16) | ((uint32_t)opcode << 11) | (rn << 5) | rd);
}
static inline int a64_shl_hb(int sz, int amt) { return (8 << sz) + amt; }        /* immh:immb for left shifts */
static inline int a64_shr_hb(int sz, int amt) { return (16 << sz) - amt; }       /* immh:immb for right shifts */
static inline void a64_vshl(a64 *a, int sz, int rd, int rn, int amt)  { a64_vshift_imm(a, 0, 0x0A, a64_shl_hb(sz, amt), rd, rn); }
static inline void a64_vushr(a64 *a, int sz, int rd, int rn, int amt) { a64_vshift_imm(a, 1, 0x00, a64_shr_hb(sz, amt), rd, rn); }
static inline void a64_vsshr(a64 *a, int sz, int rd, int rn, int amt) { a64_vshift_imm(a, 0, 0x00, a64_shr_hb(sz, amt), rd, rn); }
/* ext vd.16b, vn.16b, vm.16b, #imm */
static inline void a64_vext(a64 *a, int rd, int rn, int rm, int imm) { a64_emit(a, 0x6E000000u | (rm << 16) | ((uint32_t)imm << 11) | (rn << 5) | rd); }
/* permutes */
static inline void a64_vzip1(a64 *a, int sz, int rd, int rn, int rm) { a64_emit(a, 0x4E003800u | ((uint32_t)sz << 22) | (rm << 16) | (rn << 5) | rd); }
static inline void a64_vzip2(a64 *a, int sz, int rd, int rn, int rm) { a64_emit(a, 0x4E007800u | ((uint32_t)sz << 22) | (rm << 16) | (rn << 5) | rd); }
static inline void a64_vuzp1(a64 *a, int sz, int rd, int rn, int rm) { a64_emit(a, 0x4E001800u | ((uint32_t)sz << 22) | (rm << 16) | (rn << 5) | rd); }
static inline void a64_vuzp2(a64 *a, int sz, int rd, int rn, int rm) { a64_emit(a, 0x4E005800u | ((uint32_t)sz << 22) | (rm << 16) | (rn << 5) | rd); }
/* widening multiply: smull/umull vd.2d, vn.2s, vm.2s (hi = second half) */
static inline void a64_vmull(a64 *a, int hi, int U, int sz, int rd, int rn, int rm) {
    a64_emit(a, 0x0E20C000u | ((uint32_t)hi << 30) | ((uint32_t)U << 29) | ((uint32_t)sz << 22) | (rm << 16) | (rn << 5) | rd);
}
/* element moves. imm5 encodes lane size and index */
static inline int a64_imm5(int sz, int idx) { return (idx << (sz + 1)) | (1 << sz); }
static inline void a64_ins_elem(a64 *a, int sz, int rd, int di, int rn, int si) {   /* mov vd.T[di], vn.T[si] */
    a64_emit(a, 0x6E000400u | ((uint32_t)a64_imm5(sz, di) << 16) | ((uint32_t)(si << sz) << 11) | (rn << 5) | rd);
}
static inline void a64_ins_gen(a64 *a, int sz, int rd, int di, int rn) {           /* mov vd.T[di], wn/xn */
    a64_emit(a, 0x4E001C00u | ((uint32_t)a64_imm5(sz, di) << 16) | (rn << 5) | rd);
}
static inline void a64_umov(a64 *a, int sz, int rd, int rn, int si) {              /* umov wd/xd, vn.T[si] */
    a64_emit(a, 0x0E003C00u | ((uint32_t)(sz == 3) << 30) | ((uint32_t)a64_imm5(sz, si) << 16) | (rn << 5) | rd);
}
static inline void a64_dup_elem(a64 *a, int sz, int rd, int rn, int si) {          /* dup vd.T, vn.T[si] */
    a64_emit(a, 0x4E000400u | ((uint32_t)a64_imm5(sz, si) << 16) | (rn << 5) | rd);
}
static inline void a64_dup_gen(a64 *a, int sz, int rd, int rn) {                   /* dup vd.T, wn/xn */
    a64_emit(a, 0x4E000C00u | ((uint32_t)a64_imm5(sz, 0) << 16) | (rn << 5) | rd);
}
static inline void a64_dup_scalar(a64 *a, int sz, int rd, int rn, int si) {        /* mov dd, vn.d[si] (scalar dup) */
    a64_emit(a, 0x5E000400u | ((uint32_t)a64_imm5(sz, si) << 16) | (rn << 5) | rd);
}
static inline void a64_vmovi0(a64 *a, int rd) { a64_emit(a, 0x6F00E400u | rd); }   /* movi vd.2d, #0 */
/* scalar FP: type 0 single, 1 double */
static inline void a64_fop2(a64 *a, int type, uint32_t base, int rd, int rn, int rm) { a64_emit(a, base | ((uint32_t)type << 22) | (rm << 16) | (rn << 5) | rd); }
static inline void a64_fadd(a64 *a, int t, int rd, int rn, int rm) { a64_fop2(a, t, 0x1E202800u, rd, rn, rm); }
static inline void a64_fsub(a64 *a, int t, int rd, int rn, int rm) { a64_fop2(a, t, 0x1E203800u, rd, rn, rm); }
static inline void a64_fmul(a64 *a, int t, int rd, int rn, int rm) { a64_fop2(a, t, 0x1E200800u, rd, rn, rm); }
static inline void a64_fdiv(a64 *a, int t, int rd, int rn, int rm) { a64_fop2(a, t, 0x1E201800u, rd, rn, rm); }
static inline void a64_fsqrt(a64 *a, int t, int rd, int rn) { a64_emit(a, 0x1E21C000u | ((uint32_t)t << 22) | (rn << 5) | rd); }
static inline void a64_fmov_ss(a64 *a, int t, int rd, int rn) { a64_emit(a, 0x1E204000u | ((uint32_t)t << 22) | (rn << 5) | rd); }
static inline void a64_fcmp(a64 *a, int t, int rn, int rm) { a64_emit(a, 0x1E202000u | ((uint32_t)t << 22) | (rm << 16) | (rn << 5)); }
static inline void a64_fcvt_sd(a64 *a, int rd, int rn) { a64_emit(a, 0x1E22C000u | (rn << 5) | rd); }   /* fcvt dd, sn */
static inline void a64_fcvt_ds(a64 *a, int rd, int rn) { a64_emit(a, 0x1E624000u | (rn << 5) | rd); }   /* fcvt sd, dn */
static inline void a64_scvtf(a64 *a, int sf, int t, int rd, int rn)  { a64_emit(a, 0x1E220000u | ((uint32_t)sf << 31) | ((uint32_t)t << 22) | (rn << 5) | rd); }
static inline void a64_fcvtzs(a64 *a, int sf, int t, int rd, int rn) { a64_emit(a, 0x1E380000u | ((uint32_t)sf << 31) | ((uint32_t)t << 22) | (rn << 5) | rd); }
static inline void a64_frintx(a64 *a, int t, int rd, int rn) { a64_emit(a, 0x1E274000u | ((uint32_t)t << 22) | (rn << 5) | rd); }
/* fmov between general and FP registers */
static inline void a64_fmov_ws(a64 *a, int rd, int rn) { a64_emit(a, 0x1E260000u | (rn << 5) | rd); }   /* fmov wd, sn */
static inline void a64_fmov_sw(a64 *a, int rd, int rn) { a64_emit(a, 0x1E270000u | (rn << 5) | rd); }   /* fmov sd, wn */
static inline void a64_fmov_xd(a64 *a, int rd, int rn) { a64_emit(a, 0x9E660000u | (rn << 5) | rd); }   /* fmov xd, dn */
static inline void a64_fmov_dx(a64 *a, int rd, int rn) { a64_emit(a, 0x9E670000u | (rn << 5) | rd); }   /* fmov dd, xn */
/* system registers */
static inline void a64_mrs_fpsr(a64 *a, int rd) { a64_emit(a, 0xD53B4420u | rd); }
static inline void a64_msr_fpsr(a64 *a, int rn) { a64_emit(a, 0xD51B4420u | rn); }
static inline void a64_bic_reg(a64 *a, int sf, int rd, int rn, int rm) { a64_logic_reg(a, sf, 0, 1, rd, rn, rm, 0, 0); }

static inline void a64_fcmpe(a64 *a, int t, int rn, int rm) { a64_emit(a, 0x1E202010u | ((uint32_t)t << 22) | (rm << 16) | (rn << 5)); }
static inline void a64_frintz(a64 *a, int t, int rd, int rn) { a64_emit(a, 0x1E25C000u | ((uint32_t)t << 22) | (rn << 5) | rd); }
static inline void a64_vfrintz(a64 *a, int fsz, int rd, int rn) { a64_v2(a, 1, 0, 2 | fsz, 0x19, rd, rn); }
static inline void a64_vsxtl(a64 *a, int rd, int rn) { a64_emit(a, 0x0F20A400u | (rn << 5) | rd); }        /* sxtl vd.2d, vn.2s */
/* umaxv sd, vn.4s */
static inline void a64_vumaxv4s(a64 *a, int rd, int rn) { a64_emit(a, 0x6EB0A800u | (rn << 5) | rd); }
/* movi vd.16b, #imm8 ; mvni vd.4s, #imm8, lsl #24 */
static inline void a64_vmovi8(a64 *a, int rd, int imm8) { a64_emit(a, 0x4F00E400u | ((uint32_t)(imm8 >> 5) << 16) | ((uint32_t)(imm8 & 31) << 5) | rd); }
static inline void a64_vmvni32_lsl24(a64 *a, int rd, int imm8) { a64_emit(a, 0x6F006400u | ((uint32_t)(imm8 >> 5) << 16) | ((uint32_t)(imm8 & 31) << 5) | rd); }
/* eor rd, rn, rm, asr #amt */
static inline void a64_eor_asr(a64 *a, int sf, int rd, int rn, int rm, int amt) { a64_logic_reg(a, sf, 2, 0, rd, rn, rm, SH_ASR, amt); }

/* scalar FP compares producing a mask: fcmeq/fcmge/fcmgt sd/dd, sn, sm */
static inline void a64_fcmeq_s(a64 *a, int fsz, int rd, int rn, int rm) { a64_emit(a, 0x5E20E400u | ((uint32_t)fsz << 22) | (rm << 16) | (rn << 5) | rd); }
static inline void a64_fcmge_s(a64 *a, int fsz, int rd, int rn, int rm) { a64_emit(a, 0x7E20E400u | ((uint32_t)fsz << 22) | (rm << 16) | (rn << 5) | rd); }
static inline void a64_fcmgt_s(a64 *a, int fsz, int rd, int rn, int rm) { a64_emit(a, 0x7EA0E400u | ((uint32_t)fsz << 22) | (rm << 16) | (rn << 5) | rd); }

/* Patch a forward branch at word index `at` to land on word index `target`. */
static inline void a64_patch_b(a64 *a, uint32_t at, uint32_t target) {
    if (at >= a->cap) return;
    int32_t off = (int32_t)target - (int32_t)at;
    a->buf[at] = (a->buf[at] & 0xFC000000u) | ((uint32_t)off & 0x3FFFFFF);
}
static inline void a64_patch_bcond(a64 *a, uint32_t at, uint32_t target) {   /* also cbz/cbnz */
    if (at >= a->cap) return;
    int32_t off = (int32_t)target - (int32_t)at;
    a->buf[at] = (a->buf[at] & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFF) << 5);
}

#endif
