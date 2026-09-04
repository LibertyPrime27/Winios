/* Encoder check: emit one of everything, disassemble with the system tool
 * (when present) and compare. Run by hand or in CI:
 *   test_a64 out.bin && aarch64-linux-gnu-objdump -D -b binary -m aarch64 out.bin */
#include "../core/src/jit/a64.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    static uint32_t buf[256]; a64 a = { buf, 0, 256, 0 };
    a64_mov_imm(&a, 0, 0x1234);                    /* mov w0, #0x1234 */
    a64_mov_imm(&a, 1, 0x123456789ULL);            /* mov x1, #0x6789; movk #0x2345,lsl 16; movk #1, lsl 32 */
    a64_mov_imm(&a, 2, ~0ull);                     /* mov x2, #-1 */
    a64_mov_imm(&a, 3, 0xFFFFFFFF00000000ull);
    a64_adds(&a, 1, 2, 3, 4);                      /* adds x2, x3, x4 */
    a64_subs(&a, 0, 2, 3, 4);                      /* subs w2, w3, w4 */
    a64_add_imm(&a, 1, 5, 6, 100);                 /* add x5, x6, #100 */
    a64_cmp_imm(&a, 0, 7, 0);                      /* cmp w7, #0 */
    a64_add_ext(&a, 1, 8, 25, 9, EXT_UXTW, 0);     /* add x8, x25, w9, uxtw */
    a64_ands(&a, 1, 10, 11, 12);                   /* ands x10, x11, x12 */
    a64_mov_reg(&a, 1, 13, 14);                    /* mov x13, x14 */
    a64_mvn(&a, 0, 15, 16);                        /* mvn w15, w16 */
    a64_ubfx(&a, 1, 0, 1, 8, 8);                   /* ubfx x0, x1, #8, #8 */
    a64_bfi(&a, 1, 2, 3, 8, 8);                    /* bfi x2, x3, #8, #8 */
    a64_bfi(&a, 1, 2, 3, 0, 16);                   /* bfi x2, x3, #0, #16 */
    a64_lsl_imm(&a, 0, 4, 5, 24);                  /* lsl w4, w5, #24 */
    a64_lsr_imm(&a, 0, 4, 5, 24);                  /* lsr w4, w5, #24 */
    a64_asr_imm(&a, 1, 4, 5, 7);                   /* asr x4, x5, #7 */
    a64_ror_imm(&a, 0, 4, 5, 13);                  /* ror w4, w5, #13 */
    a64_uxtb(&a, 6, 7); a64_sxth(&a, 1, 6, 7); a64_sxtw(&a, 6, 7);
    a64_shiftv(&a, 1, 0, 6, 7, 8);                 /* lsl x6, x7, x8 */
    a64_shiftv(&a, 0, 2, 6, 7, 8);                 /* asr w6, w7, w8 */
    a64_mul(&a, 1, 9, 10, 11); a64_smull(&a, 9, 10, 11); a64_smulh(&a, 9, 10, 11);
    a64_csel(&a, 1, 0, 1, 2, CC_EQ); a64_cset(&a, 0, 3, CC_LO); a64_cinc(&a, 0, 3, 3, CC_NE);
    a64_ldr_off(&a, 3, 0, 26, 0x48);               /* ldr x0, [x26, #72] */
    a64_str_off(&a, 2, 0, 26, 0x48);               /* str w0, [x26, #72] */
    a64_ldr_off(&a, 0, 0, 26, 3);                  /* ldrb w0, [x26, #3] */
    a64_ldrs_off(&a, 1, 1, 0, 26, 4);              /* ldrsh x0, [x26, #4] */
    a64_ldrs_off(&a, 2, 1, 0, 26, 8);              /* ldrsw x0, [x26, #8] */
    a64_ldr_reg(&a, 3, 0, 25, 1, 3);               /* ldr x0, [x25, x1] */
    a64_ldr_reg(&a, 2, 0, 25, 1, 2);               /* ldr w0, [x25, w1, uxtw] */
    a64_str_reg(&a, 0, 0, 25, 1, 2);               /* strb w0, [x25, w1, uxtw] */
    a64_ldrs_reg(&a, 0, 0, 0, 25, 1, 3);           /* ldrsb w0, [x25, x1] */
    a64_stp(&a, 19, 20, 26, 16); a64_ldp(&a, 19, 20, 26, 16);
    a64_stp_pre(&a, 29, 30, SP, -16); a64_ldp_post(&a, 29, 30, SP, 16);
    a64_b(&a, 4); a64_bcond(&a, CC_NE, -2); a64_cbz(&a, 0, 5, 3); a64_cbnz(&a, 1, 5, -3); a64_tbnz(&a, 5, 33, 2);
    a64_br(&a, 27); a64_blr(&a, 28); a64_ret(&a); a64_nop(&a); a64_brk(&a, 1);
    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); fwrite(buf, 4, a.n, f); fclose(f); }
    printf("%u words\n", a.n);
    return a.overflow;
}
