/* xop -- a decoded operand with everything the executor needs pre-resolved.
 *
 * Zydis' operand structure is general; at execution time we only want "which
 * register slot, how wide, high byte or not" and "base index, index index,
 * scale, displacement, segment". Resolving those on every step cost more than
 * the arithmetic did (three table lookups per register operand). This is the
 * form the block cache stores and the interpreter executes; it is also the
 * operand form the dynarec will lower from.
 *
 * Immediates come pre-sign-extended, and a *relative* immediate (branch
 * target) is already the absolute target address. RIP-relative memory
 * operands have the instruction's own address folded into `disp`, with no
 * base register. Both need the instruction's address, so an xop belongs to
 * one instruction at one address -- which is what a block cache entry is.
 */
#ifndef XCORE_XOP_H
#define XCORE_XOP_H

#include <Zydis/Zydis.h>
#include <stdint.h>

enum { XOP_NONE = 0, XOP_REG, XOP_MEM, XOP_IMM };
enum { XR_GPR = 0, XR_XMM, XR_X87, XR_SEG, XR_IP, XR_FLAGS, XR_OTHER };

typedef struct {
    uint8_t  type;       /* XOP_* */
    uint16_t size;       /* operand size in bits */
    /* register */
    uint16_t reg;        /* ZydisRegister, for the few places that want the enum */
    uint8_t  rcls;       /* XR_* */
    uint8_t  ridx;       /* gpr 0-15, xmm 0-15, st 0-7, segment 0-5 */
    uint8_t  rhi8;       /* AH/BH/CH/DH */
    uint8_t  rbits;      /* register width: 8/16/32/64, 128 for xmm */
    /* memory */
    int8_t   mbase;      /* gpr index, or -1 */
    int8_t   mindex;     /* gpr index, or -1 */
    uint8_t  mscale;
    uint8_t  mseg;       /* 0 flat, 1 FS, 2 GS */
    int64_t  disp;       /* includes next-RIP for RIP-relative */
    /* immediate */
    uint64_t imm;        /* sign-extended; absolute target for relative branches */
} xop;

/* Convert all operands of one decoded instruction at `rip`. */
void xop_convert(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *zops, uint64_t rip,
                 int mode, xop *out);

#endif
