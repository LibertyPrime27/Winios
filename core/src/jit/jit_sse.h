/* SSE/SSE2 on NEON. Included by jit.c after the register cache and the
 * callout machinery; everything here is `static` and part of the compiler.
 *
 * Two rules keep this exact against the silicon recordings:
 *
 * 1. Integer SIMD, moves, shuffles and logic have identical semantics on
 *    both ISAs and are lowered one-to-one (a few need two or three
 *    instructions, e.g. PMULHW = smull + smull2 + uzp2).
 *
 * 2. Floating point is lowered natively *for non-NaN results*. x86 and ARM
 *    differ only when the result is a NaN: x86 propagates the first NaN
 *    operand (ARM prefers an SNaN in either position) and its default NaN
 *    is negative (ARM's is positive). So every FP op computes into a
 *    temporary, tests the result for NaN (one fcmp for scalars, fcmeq +
 *    uminv for vectors) and, in that case, leaves the block through the
 *    interpreter, which redoes the instruction from the untouched inputs.
 *    NaN results are rare in real code, so the check costs two to four
 *    instructions and the slow path almost never runs. Conversions use the
 *    same trick for x86's "integer indefinite" result on overflow.
 *
 * MXCSR: the rounding mode and flush-to-zero are mirrored into FPCR while
 * the JIT runs (xc_run_jit / jit_callout); the sticky exception flags
 * accumulate in FPSR and are folded into MXCSR at callouts and exits. The
 * denormal-operand flag (DE) is not tracked natively.
 */

/* --- NZ_FCMP: condition flags left by an fcmp for COMISS-style compares --- */
enum { NZ_FCMP = 10 };

static int xmm_of(const xop *o) { return o->type == XOP_REG && o->rcls == XR_XMM ? o->ridx : -1; }

/* Leave the block through the interpreter for the current instruction:
 * spill, call jit_callout, go to the dispatcher. Compile state unchanged
 * (the fall-through path continues with the cache intact). */
static void emit_slow_exit(jc *j) {
    spill(j);
    emit_set_rip_imm(j, j->d->rip);
    a64_mov_reg(&j->a, 1, 0, R_CPU);
    a64_mov_imm(&j->a, 1, (uint64_t)(uintptr_t)j->d);
    a64_mov_imm(&j->a, R_TMP, (uint64_t)(uintptr_t)jit_callout);
    a64_blr(&j->a, R_TMP);
    a64_br(&j->a, R_DISP);
}

/* Load an SSE source operand: an XMM register's host register, or a memory
 * operand of `bits` (32/64/128) into vt. Returns the register to read. */
static int xsrc(jc *j, const xop *op, int vt, int bits) {
    int x = xmm_of(op);
    if (x >= 0) return xreg(j, x);
    if (op->type != XOP_MEM) { j->failed = 1; return vt; }
    int w = emit_ea(j, op, T4);
    emit_bounds(j, bits / 8);
    fault_site(j);
    a64_fldst_reg(&j->a, bits == 128 ? 2 : bits == 64 ? 1 : 0, 1, vt, R_BASE, T4, w ? 2 : 3);
    return vt;
}
/* Store `bits` of vs to an XMM (full overwrite) or memory destination. */
static void xdst(jc *j, const xop *op, int vs, int bits) {
    int x = xmm_of(op);
    if (x >= 0) { if (VREG(x) != vs) a64_vmov(&j->a, VREG(x), vs); xset(j, x); return; }
    if (op->type != XOP_MEM) { j->failed = 1; return; }
    int w = emit_ea(j, op, T4);
    emit_bounds(j, bits / 8);
    fault_site(j);
    a64_fldst_reg(&j->a, bits == 128 ? 2 : bits == 64 ? 1 : 0, 0, vs, R_BASE, T4, w ? 2 : 3);
}

/* Result in vt is a NaN in some lane (packed) / lane 0 (scalar): slow path. */
static void emit_nan_guard(jc *j, int vt, int packed, int fsz) {
    uint32_t br;
    if (!packed) { a64_fcmp(&j->a, fsz, vt, vt); br = a64_here(&j->a); a64_bcond(&j->a, CC_VC, 0); j->nz = NZ_NONE; }
    else {
        a64_vfcmeq(&j->a, fsz, VT3, vt, vt);          /* all-ones where not NaN */
        a64_vuminv4s(&j->a, VT3, VT3);
        a64_fmov_ws(&j->a, T0, VT3);
        br = a64_here(&j->a); a64_cbnz(&j->a, 0, T0, 0);
    }
    emit_slow_exit(j);
    a64_patch_bcond(&j->a, br, a64_here(&j->a));
}

/* dst.lane0 = vt.lane0 (fsz 0: 32-bit lane, 1: 64-bit) */
static void ins_low(jc *j, int vd, int vt, int fsz) { a64_ins_elem(&j->a, fsz ? 3 : 2, vd, 0, vt, 0); }

/* Two-operand packed/scalar FP arithmetic. op: 0 add 1 sub 2 mul 3 div 4 min 5 max */
static void emit_fp_arith(jc *j, int op, int packed, int fsz) {
    const xop *dst = &j->ops[0], *src = &j->ops[1];
    int d = xreg(j, xmm_of(dst));
    int s = xsrc(j, src, VT0, packed ? 128 : (fsz ? 64 : 32));
    int r = VT1;
    if (op <= 3) {
        if (packed) {
            switch (op) { case 0: a64_vfadd(&j->a, fsz, r, d, s); break; case 1: a64_vfsub(&j->a, fsz, r, d, s); break;
                          case 2: a64_vfmul(&j->a, fsz, r, d, s); break; default: a64_vfdiv(&j->a, fsz, r, d, s); break; }
        } else {
            switch (op) { case 0: a64_fadd(&j->a, fsz, r, d, s); break; case 1: a64_fsub(&j->a, fsz, r, d, s); break;
                          case 2: a64_fmul(&j->a, fsz, r, d, s); break; default: a64_fdiv(&j->a, fsz, r, d, s); break; }
        }
    } else {
        /* x86 MIN/MAX: dst if the strict compare holds, else src (so NaN and
         * equal-zero cases pick src). Done on the whole vector; the scalar
         * form only commits lane 0. */
        if (packed) {
            if (op == 4) a64_vfcmgt(&j->a, fsz, r, s, d);      /* s > d  <=> d < s : keep d */
            else         a64_vfcmgt(&j->a, fsz, r, d, s);      /* d > s : keep d */
        } else {                                               /* lane 0 only: the other lanes must not signal */
            if (op == 4) a64_fcmgt_s(&j->a, fsz, r, s, d); else a64_fcmgt_s(&j->a, fsz, r, d, s);
        }
        a64_vbsl(&j->a, r, d, s);
    }
    emit_nan_guard(j, r, packed, fsz);
    if (packed) a64_vmov(&j->a, d, r); else ins_low(j, d, r, fsz);
    xset(j, xmm_of(dst));
}

static void emit_fp_sqrt(jc *j, int packed, int fsz) {
    const xop *dst = &j->ops[0], *src = &j->ops[1];
    int s = xsrc(j, src, VT0, packed ? 128 : (fsz ? 64 : 32));
    int d = xreg(j, xmm_of(dst));
    if (packed) a64_vfsqrt(&j->a, fsz, VT1, s); else a64_fsqrt(&j->a, fsz, VT1, s);
    emit_nan_guard(j, VT1, packed, fsz);
    if (packed) a64_vmov(&j->a, d, VT1); else ins_low(j, d, VT1, fsz);
    xset(j, xmm_of(dst));
}

/* CMPPS/PD/SS/SD with predicate imm8 (0..7) -> all-ones / zero masks */
static void emit_fp_cmp(jc *j, int packed, int fsz) {
    const xop *dst = &j->ops[0], *src = &j->ops[1];
    int p = (int)(j->ops[2].imm & 7);
    int d = xreg(j, xmm_of(dst));
    int s = xsrc(j, src, VT0, packed ? 128 : (fsz ? 64 : 32));
    int r = VT1;
    if (packed) switch (p & 3) {
    case 0: a64_vfcmeq(&j->a, fsz, r, d, s); break;                 /* eq / neq */
    case 1: a64_vfcmgt(&j->a, fsz, r, s, d); break;                 /* lt / nlt */
    case 2: a64_vfcmge(&j->a, fsz, r, s, d); break;                 /* le / nle */
    default:                                                         /* unord / ord */
        a64_vfcmeq(&j->a, fsz, r, d, d); a64_vfcmeq(&j->a, fsz, VT2, s, s); a64_vand(&j->a, r, r, VT2); break;
    } else switch (p & 3) {                                          /* scalar: only lane 0 may signal */
    case 0: a64_fcmeq_s(&j->a, fsz, r, d, s); break;
    case 1: a64_fcmgt_s(&j->a, fsz, r, s, d); break;
    case 2: a64_fcmge_s(&j->a, fsz, r, s, d); break;
    default: a64_fcmeq_s(&j->a, fsz, r, d, d); a64_fcmeq_s(&j->a, fsz, VT2, s, s); a64_vand(&j->a, r, r, VT2); break;
    }
    if ((p & 4) ? (p & 3) != 3 : (p & 3) == 3) a64_vnot(&j->a, r, r);   /* neq, nlt, nle, unord are the negations */
    if (packed) a64_vmov(&j->a, d, r); else ins_low(j, d, r, fsz);
    xset(j, xmm_of(dst));
}

/* COMISS/UCOMISS/COMISD/UCOMISD: ZF,PF,CF from the compare; OF,SF,AF cleared. */
static void emit_fp_comis(jc *j, int fsz, int signaling) {
    int a = xreg(j, xmm_of(&j->ops[0]));
    int b = xsrc(j, &j->ops[1], VT0, fsz ? 64 : 32);
    if (signaling) a64_fcmpe(&j->a, fsz, a, b); else a64_fcmp(&j->a, fsz, a, b);
    a64_cset(&j->a, 0, T0, CC_MI);                 /* less */
    a64_cset(&j->a, 0, T1, CC_VS);                 /* unordered */
    a64_cset(&j->a, 0, T2, CC_EQ);                 /* equal */
    a64_orr(&j->a, 0, T0, T0, T1);                 /* CF = less | unordered */
    a64_orr(&j->a, 0, T2, T2, T1);                 /* ZF = equal | unordered */
    a64_ldr_off(&j->a, 3, T3, R_CPU, OFF(rflags));
    a64_mov_imm(&j->a, T4, XC_ARITH_FLAGS);
    a64_bic_reg(&j->a, 1, T3, T3, T4);
    a64_orr(&j->a, 1, T3, T3, T0);
    a64_orr_shifted(&j->a, 1, T3, T3, T1, SH_LSL, 2);   /* PF */
    a64_orr_shifted(&j->a, 1, T3, T3, T2, SH_LSL, 6);   /* ZF */
    a64_str_off(&j->a, 3, T3, R_CPU, OFF(rflags));
    a64_str_off(&j->a, 2, ZR, R_CPU, OFF(lz_op));
    j->lz = LZ_VALID;
    j->nz = NZ_FCMP;                               /* NZCV still describe the compare for a following Jcc */
}

/* CVTSI2SS/SD */
static void emit_cvt_i2f(jc *j, int fsz) {
    int d = xreg(j, xmm_of(&j->ops[0]));
    const xop *src = &j->ops[1];
    int sf = src->size == 64;
    int r = ld_op(j, src, T0, 0);
    a64_scvtf(&j->a, sf, fsz, VT0, r);
    ins_low(j, d, VT0, fsz);
    xset(j, xmm_of(&j->ops[0]));
}
/* CVT(T)SS2SI / CVT(T)SD2SI: out-of-range results (which ARM saturates and
 * x86 turns into 0x8000...) go through the interpreter. */
static void emit_cvt_f2i(jc *j, int fsz, int trunc) {
    const xop *dst = &j->ops[0];
    int s = xsrc(j, &j->ops[1], VT0, fsz ? 64 : 32);
    int sf = dst->size == 64;
    a64_fcmp(&j->a, fsz, s, s);                    /* NaN converts to 0 on ARM, to 0x8000... on x86 */
    j->nz = NZ_NONE;
    uint32_t br0 = a64_here(&j->a); a64_bcond(&j->a, CC_VC, 0);
    emit_slow_exit(j);
    a64_patch_bcond(&j->a, br0, a64_here(&j->a));
    if (!trunc) { a64_frintx(&j->a, fsz, VT1, s); s = VT1; }
    a64_fcvtzs(&j->a, sf, fsz, T0, s);             /* raises Inexact itself for the truncating forms */
    /* saturated (INT_MIN or INT_MAX) -> x ^ (x >> 31) == INT_MAX -> +1 == INT_MIN -> << 1 == 0 */
    a64_eor_asr(&j->a, sf, T1, T0, T0, sf ? 63 : 31);
    a64_add_imm(&j->a, sf, T1, T1, 1);
    a64_lsl_imm(&j->a, sf, T1, T1, 1);
    uint32_t br = a64_here(&j->a); a64_cbnz(&j->a, sf, T1, 0);
    emit_slow_exit(j);
    a64_patch_bcond(&j->a, br, a64_here(&j->a));
    st_op(j, dst, T0);
}
/* CVT(T)PS2DQ: four lanes, same escape for saturated or NaN lanes */
static void emit_cvt_ps2dq(jc *j, int trunc) {
    int d = xreg(j, xmm_of(&j->ops[0]));
    int s = xsrc(j, &j->ops[1], VT0, 128);
    if (trunc) { if (s != VT1) a64_vmov(&j->a, VT1, s); } else a64_vfrintx(&j->a, 0, VT1, s);
    a64_vfcvtzs(&j->a, 0, VT2, VT1);               /* the candidate result (raises Inexact for the truncating form) */
    a64_vsshr(&j->a, 2, VT3, VT2, 31);
    a64_veor(&j->a, VT3, VT3, VT2);                /* saturated lanes -> 0x7fffffff */
    a64_vmvni32_lsl24(&j->a, VT0, 0x80);           /* 0x7fffffff */
    a64_vcmeq(&j->a, 2, VT3, VT3, VT0);            /* saturated mask */
    a64_vfcmeq(&j->a, 0, VT0, VT1, VT1);           /* not-NaN mask */
    a64_vbic(&j->a, VT0, VT0, VT3);                /* ok = notNaN & ~saturated */
    a64_vuminv4s(&j->a, VT0, VT0);
    a64_fmov_ws(&j->a, T0, VT0);
    uint32_t br = a64_here(&j->a); a64_cbnz(&j->a, 0, T0, 0);
    emit_slow_exit(j);
    a64_patch_bcond(&j->a, br, a64_here(&j->a));
    a64_vmov(&j->a, d, VT2);
    xset(j, xmm_of(&j->ops[0]));
}

/* Variable shift count from an XMM (low 64 bits): min(count, 64) splat into
 * every byte of vt, negated for right shifts. */
static void emit_shift_count(jc *j, int vs, int vt, int right) {
    a64_vuqxtn(&j->a, 0, 2, vt, vs);               /* 2d -> 2s saturating */
    a64_vuqxtn(&j->a, 0, 1, vt, vt);               /* 4s -> 4h */
    a64_vuqxtn(&j->a, 0, 0, vt, vt);               /* 8h -> 8b : lane 0 = min(count, 255) */
    a64_dup_elem(&j->a, 0, vt, vt, 0);
    a64_vmovi8(&j->a, VT3, 64);
    a64_vumin(&j->a, 0, vt, vt, VT3);
    if (right) a64_vneg(&j->a, 0, vt, vt);
}

/* PSLL/PSRL/PSRA. kind 0 shl, 1 shr, 2 sar; sz lane size */
static void emit_pshift(jc *j, int kind, int sz) {
    int x = xmm_of(&j->ops[0]);
    int d = xreg(j, x);
    const xop *cnt = &j->ops[1];
    int bits = 8 << sz;
    if (cnt->type == XOP_IMM) {
        int amt = (int)(cnt->imm & 0xff);
        if (amt == 0) return;
        if (amt >= bits) {
            if (kind == 2) a64_vsshr(&j->a, sz, d, d, bits - 1); else a64_vmovi0(&j->a, d);
        } else if (kind == 0) a64_vshl(&j->a, sz, d, d, amt);
        else if (kind == 1) a64_vushr(&j->a, sz, d, d, amt);
        else a64_vsshr(&j->a, sz, d, d, amt);
        xset(j, x);
        return;
    }
    int s = xsrc(j, cnt, VT0, 128);
    emit_shift_count(j, s, VT1, kind != 0);
    if (kind == 2) a64_vsshl(&j->a, sz, d, d, VT1); else a64_vushl(&j->a, sz, d, d, VT1);
    xset(j, x);
}

/* PSHUFD-style lane selects: result lane i <- src lane sel(i) */
static void emit_shuf32(jc *j, int vd, int vs, int imm) {
    int same = 1;
    for (int i = 1; i < 4; i++) if (((imm >> (2 * i)) & 3) != (imm & 3)) same = 0;
    if (same) { a64_dup_elem(&j->a, 2, vd, vs, imm & 3); return; }
    if (imm == 0xE4) { if (vd != vs) a64_vmov(&j->a, vd, vs); return; }
    for (int i = 0; i < 4; i++) a64_ins_elem(&j->a, 2, VT1, i, vs, (imm >> (2 * i)) & 3);
    a64_vmov(&j->a, vd, VT1);
}

/* PMOVMSKB: sign bit of each byte -> 16-bit mask, no constants needed */
static void emit_pmovmskb(jc *j, int vs) {
    a64_vushr(&j->a, 0, VT0, vs, 7);               /* bytes 0/1 */
    a64_vushr(&j->a, 1, VT1, VT0, 7); a64_vorr(&j->a, VT0, VT0, VT1);    /* 2 bits per 16-bit lane */
    a64_vushr(&j->a, 2, VT1, VT0, 14); a64_vorr(&j->a, VT0, VT0, VT1);   /* 4 bits per 32-bit lane */
    a64_vushr(&j->a, 3, VT1, VT0, 28); a64_vorr(&j->a, VT0, VT0, VT1);   /* 8 bits per 64-bit lane (low byte) */
    a64_umov(&j->a, 0, T0, VT0, 0);
    a64_umov(&j->a, 0, T1, VT0, 8);
    a64_orr_shifted(&j->a, 0, T0, T0, T1, SH_LSL, 8);
}

/* Returns 1 if the instruction was lowered (or deliberately handed to the
 * slow path), 0 if the caller should emit a callout. */
static int emit_sse(jc *j) {
    const ZydisDecodedInstruction *in = &j->d->in;
    const xop *ops = j->ops;
    ZydisMnemonic m = in->mnemonic;
    int nops = in->operand_count_visible;
    int x0 = nops > 0 ? xmm_of(&ops[0]) : -1, x1 = nops > 1 ? xmm_of(&ops[1]) : -1;
    /* anything that is not xmm/gpr/mem/imm (e.g. MMX registers) stays in the interpreter */
    for (int i = 0; i < nops; i++)
        if (ops[i].type == XOP_REG && ops[i].rcls != XR_XMM && ops[i].rcls != XR_GPR) return 0;

    switch (m) {
    /* ---- full-width moves ---- */
    case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVAPD: case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU: case ZYDIS_MNEMONIC_MOVNTPS: case ZYDIS_MNEMONIC_MOVNTPD:
    case ZYDIS_MNEMONIC_MOVNTDQ: case ZYDIS_MNEMONIC_LDDQU:
        if (x0 >= 0) {
            if (x1 >= 0) { if (x0 != x1) { a64_vmov(&j->a, VREG(x0), xreg(j, x1)); xset(j, x0); } return 1; }
            xsrc(j, &ops[1], VREG(x0), 128); xset(j, x0); return 1;
        }
        xdst(j, &ops[0], xreg(j, x1), 128); return 1;

    /* ---- 64/32-bit moves that zero the rest of the destination ---- */
    case ZYDIS_MNEMONIC_MOVQ:
        if (x0 >= 0) {
            if (x1 >= 0) { a64_fmov_ss(&j->a, 1, VREG(x0), xreg(j, x1)); xset(j, x0); return 1; }
            if (ops[1].type == XOP_MEM) { xsrc(j, &ops[1], VREG(x0), 64); xset(j, x0); return 1; }
            { int r = ld_op(j, &ops[1], T0, 0); a64_fmov_dx(&j->a, VREG(x0), r); xset(j, x0); return 1; }
        }
        if (ops[0].type == XOP_MEM) { xdst(j, &ops[0], xreg(j, x1), 64); return 1; }
        a64_fmov_xd(&j->a, T0, xreg(j, x1)); st_op(j, &ops[0], T0); return 1;
    case ZYDIS_MNEMONIC_MOVD:
        if (x0 >= 0) {
            if (ops[1].type == XOP_MEM) { xsrc(j, &ops[1], VREG(x0), 32); xset(j, x0); return 1; }
            { int r = ld_op(j, &ops[1], T0, 0); a64_fmov_sw(&j->a, VREG(x0), r); xset(j, x0); return 1; }
        }
        if (ops[0].type == XOP_MEM) { xdst(j, &ops[0], xreg(j, x1), 32); return 1; }
        a64_fmov_ws(&j->a, T0, xreg(j, x1)); st_op(j, &ops[0], T0); return 1;
    case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_MOVSD: {
        int fsz = m == ZYDIS_MNEMONIC_MOVSD;
        if (x0 >= 0 && x1 >= 0) { int d = xreg(j, x0); ins_low(j, d, xreg(j, x1), fsz); xset(j, x0); return 1; }   /* merge */
        if (x0 >= 0) { xsrc(j, &ops[1], VREG(x0), fsz ? 64 : 32); xset(j, x0); return 1; }                      /* load zeroes the rest */
        xdst(j, &ops[0], xreg(j, x1), fsz ? 64 : 32); return 1;
    }
    case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD:
        if (x0 >= 0) { xsrc(j, &ops[1], VT0, 64); int d = xreg(j, x0); a64_ins_elem(&j->a, 3, d, 0, VT0, 0); xset(j, x0); return 1; }
        xdst(j, &ops[0], xreg(j, x1), 64); return 1;
    case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVHPD:
        if (x0 >= 0) { xsrc(j, &ops[1], VT0, 64); int d = xreg(j, x0); a64_ins_elem(&j->a, 3, d, 1, VT0, 0); xset(j, x0); return 1; }
        a64_dup_scalar(&j->a, 3, VT0, xreg(j, x1), 1); xdst(j, &ops[0], VT0, 64); return 1;
    case ZYDIS_MNEMONIC_MOVHLPS: { int d = xreg(j, x0), s = xreg(j, x1); a64_ins_elem(&j->a, 3, d, 0, s, 1); xset(j, x0); return 1; }
    case ZYDIS_MNEMONIC_MOVLHPS: { int d = xreg(j, x0), s = xreg(j, x1); a64_ins_elem(&j->a, 3, d, 1, s, 0); xset(j, x0); return 1; }

    /* ---- logic ---- */
    case ZYDIS_MNEMONIC_PXOR: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD:
        if (x0 == x1) { a64_vmovi0(&j->a, VREG(x0)); xset(j, x0); return 1; }
        { int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); a64_veor(&j->a, d, d, s); xset(j, x0); return 1; }
    case ZYDIS_MNEMONIC_POR: case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD:
        { int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); a64_vorr(&j->a, d, d, s); xset(j, x0); return 1; }
    case ZYDIS_MNEMONIC_PAND: case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD:
        { int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); a64_vand(&j->a, d, d, s); xset(j, x0); return 1; }
    case ZYDIS_MNEMONIC_PANDN: case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_ANDNPD:
        if (x0 == x1) { a64_vmovi0(&j->a, VREG(x0)); xset(j, x0); return 1; }
        { int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); a64_vbic(&j->a, d, s, d); xset(j, x0); return 1; }

    /* ---- integer arithmetic ---- */
#define PBIN(MN, FN, SZ) case MN: { int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); FN(&j->a, SZ, d, d, s); xset(j, x0); return 1; }
    PBIN(ZYDIS_MNEMONIC_PADDB, a64_vadd, 0) PBIN(ZYDIS_MNEMONIC_PADDW, a64_vadd, 1) PBIN(ZYDIS_MNEMONIC_PADDD, a64_vadd, 2) PBIN(ZYDIS_MNEMONIC_PADDQ, a64_vadd, 3)
    PBIN(ZYDIS_MNEMONIC_PADDUSB, a64_vuqadd, 0) PBIN(ZYDIS_MNEMONIC_PADDUSW, a64_vuqadd, 1)
    PBIN(ZYDIS_MNEMONIC_PADDSB, a64_vsqadd, 0) PBIN(ZYDIS_MNEMONIC_PADDSW, a64_vsqadd, 1)
    PBIN(ZYDIS_MNEMONIC_PSUBUSB, a64_vuqsub, 0) PBIN(ZYDIS_MNEMONIC_PSUBUSW, a64_vuqsub, 1)
    PBIN(ZYDIS_MNEMONIC_PSUBSB, a64_vsqsub, 0) PBIN(ZYDIS_MNEMONIC_PSUBSW, a64_vsqsub, 1)
    PBIN(ZYDIS_MNEMONIC_PMULLW, a64_vmul, 1) PBIN(ZYDIS_MNEMONIC_PMULLD, a64_vmul, 2)
    PBIN(ZYDIS_MNEMONIC_PCMPEQB, a64_vcmeq, 0) PBIN(ZYDIS_MNEMONIC_PCMPEQW, a64_vcmeq, 1) PBIN(ZYDIS_MNEMONIC_PCMPEQD, a64_vcmeq, 2)
    PBIN(ZYDIS_MNEMONIC_PMAXUB, a64_vumax, 0) PBIN(ZYDIS_MNEMONIC_PMINUB, a64_vumin, 0)
    PBIN(ZYDIS_MNEMONIC_PMAXSW, a64_vsmax, 1) PBIN(ZYDIS_MNEMONIC_PMINSW, a64_vsmin, 1)
    PBIN(ZYDIS_MNEMONIC_PAVGB, a64_vurhadd, 0) PBIN(ZYDIS_MNEMONIC_PAVGW, a64_vurhadd, 1)
    PBIN(ZYDIS_MNEMONIC_PUNPCKLBW, a64_vzip1, 0) PBIN(ZYDIS_MNEMONIC_PUNPCKLWD, a64_vzip1, 1)
    PBIN(ZYDIS_MNEMONIC_PUNPCKLDQ, a64_vzip1, 2) PBIN(ZYDIS_MNEMONIC_PUNPCKLQDQ, a64_vzip1, 3)
    PBIN(ZYDIS_MNEMONIC_PUNPCKHBW, a64_vzip2, 0) PBIN(ZYDIS_MNEMONIC_PUNPCKHWD, a64_vzip2, 1)
    PBIN(ZYDIS_MNEMONIC_PUNPCKHDQ, a64_vzip2, 2) PBIN(ZYDIS_MNEMONIC_PUNPCKHQDQ, a64_vzip2, 3)
    PBIN(ZYDIS_MNEMONIC_UNPCKLPS, a64_vzip1, 2) PBIN(ZYDIS_MNEMONIC_UNPCKHPS, a64_vzip2, 2)
    PBIN(ZYDIS_MNEMONIC_UNPCKLPD, a64_vzip1, 3) PBIN(ZYDIS_MNEMONIC_UNPCKHPD, a64_vzip2, 3)
#undef PBIN
    case ZYDIS_MNEMONIC_PSUBB: case ZYDIS_MNEMONIC_PSUBW: case ZYDIS_MNEMONIC_PSUBD: case ZYDIS_MNEMONIC_PSUBQ: {
        int sz = m == ZYDIS_MNEMONIC_PSUBB ? 0 : m == ZYDIS_MNEMONIC_PSUBW ? 1 : m == ZYDIS_MNEMONIC_PSUBD ? 2 : 3;
        if (x0 == x1) { a64_vmovi0(&j->a, VREG(x0)); xset(j, x0); return 1; }
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); a64_vsub(&j->a, sz, d, d, s); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PCMPGTB: case ZYDIS_MNEMONIC_PCMPGTW: case ZYDIS_MNEMONIC_PCMPGTD: {
        int sz = m == ZYDIS_MNEMONIC_PCMPGTB ? 0 : m == ZYDIS_MNEMONIC_PCMPGTW ? 1 : 2;
        if (x0 == x1) { a64_vmovi0(&j->a, VREG(x0)); xset(j, x0); return 1; }
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128); a64_vcmgt(&j->a, sz, d, d, s); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PMULHW: case ZYDIS_MNEMONIC_PMULHUW: {
        int U = m == ZYDIS_MNEMONIC_PMULHUW;
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128);
        a64_vmull(&j->a, 0, U, 1, VT1, d, s);
        a64_vmull(&j->a, 1, U, 1, VT2, d, s);
        a64_vuzp2(&j->a, 1, d, VT1, VT2);          /* the high halves */
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PMULUDQ: {
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128);
        a64_vuzp1(&j->a, 2, VT1, d, d);            /* lanes 0,2 of each */
        a64_vuzp1(&j->a, 2, VT2, s, s);
        a64_vmull(&j->a, 0, 1, 2, d, VT1, VT2);
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PACKSSWB: case ZYDIS_MNEMONIC_PACKSSDW: case ZYDIS_MNEMONIC_PACKUSWB: {
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128);
        int sz = m == ZYDIS_MNEMONIC_PACKSSDW ? 1 : 0;
        if (m == ZYDIS_MNEMONIC_PACKUSWB) { a64_vsqxtun(&j->a, 0, sz, VT1, d); a64_vsqxtun(&j->a, 1, sz, VT1, s); }
        else { a64_vsqxtn(&j->a, 0, sz, VT1, d); a64_vsqxtn(&j->a, 1, sz, VT1, s); }
        a64_vmov(&j->a, d, VT1);
        xset(j, x0); return 1;
    }

    /* ---- shifts ---- */
    case ZYDIS_MNEMONIC_PSLLW: emit_pshift(j, 0, 1); return 1;
    case ZYDIS_MNEMONIC_PSLLD: emit_pshift(j, 0, 2); return 1;
    case ZYDIS_MNEMONIC_PSLLQ: emit_pshift(j, 0, 3); return 1;
    case ZYDIS_MNEMONIC_PSRLW: emit_pshift(j, 1, 1); return 1;
    case ZYDIS_MNEMONIC_PSRLD: emit_pshift(j, 1, 2); return 1;
    case ZYDIS_MNEMONIC_PSRLQ: emit_pshift(j, 1, 3); return 1;
    case ZYDIS_MNEMONIC_PSRAW: emit_pshift(j, 2, 1); return 1;
    case ZYDIS_MNEMONIC_PSRAD: emit_pshift(j, 2, 2); return 1;
    case ZYDIS_MNEMONIC_PSLLDQ: case ZYDIS_MNEMONIC_PSRLDQ: {
        int d = xreg(j, x0);
        int n = (int)(ops[1].imm & 0xff);
        if (n == 0) return 1;
        if (n >= 16) { a64_vmovi0(&j->a, d); xset(j, x0); return 1; }
        a64_vmovi0(&j->a, VT0);
        if (m == ZYDIS_MNEMONIC_PSLLDQ) a64_vext(&j->a, d, VT0, d, 16 - n); else a64_vext(&j->a, d, d, VT0, n);
        xset(j, x0); return 1;
    }

    /* ---- shuffles ---- */
    case ZYDIS_MNEMONIC_PSHUFD: {
        int s = xsrc(j, &ops[1], VT0, 128);
        emit_shuf32(j, VREG(x0), s, (int)(ops[2].imm & 0xff));
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PSHUFLW: case ZYDIS_MNEMONIC_PSHUFHW: {
        int s = xsrc(j, &ops[1], VT0, 128);
        int imm = (int)(ops[2].imm & 0xff), base = m == ZYDIS_MNEMONIC_PSHUFHW ? 4 : 0;
        a64_vmov(&j->a, VT1, s);
        for (int i = 0; i < 4; i++) a64_ins_elem(&j->a, 1, VT1, base + i, s, base + ((imm >> (2 * i)) & 3));
        a64_vmov(&j->a, VREG(x0), VT1);
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_SHUFPS: {
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128);
        int imm = (int)(ops[2].imm & 0xff);
        a64_ins_elem(&j->a, 2, VT1, 0, d, imm & 3);
        a64_ins_elem(&j->a, 2, VT1, 1, d, (imm >> 2) & 3);
        a64_ins_elem(&j->a, 2, VT1, 2, s, (imm >> 4) & 3);
        a64_ins_elem(&j->a, 2, VT1, 3, s, (imm >> 6) & 3);
        a64_vmov(&j->a, d, VT1);
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_SHUFPD: {
        int d = xreg(j, x0), s = xsrc(j, &ops[1], VT0, 128);
        int imm = (int)(ops[2].imm & 3);
        a64_ins_elem(&j->a, 3, VT1, 0, d, imm & 1);
        a64_ins_elem(&j->a, 3, VT1, 1, s, (imm >> 1) & 1);
        a64_vmov(&j->a, d, VT1);
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PINSRW: {
        int d = xreg(j, x0);
        int r = ld_op(j, &ops[1], T0, 0);
        a64_ins_gen(&j->a, 1, d, (int)(ops[2].imm & 7), r);
        xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_PEXTRW: {
        if (x1 < 0) return 0;
        a64_umov(&j->a, 1, T0, xreg(j, x1), (int)(ops[2].imm & 7));
        st_op(j, &ops[0], T0); return 1;
    }
    case ZYDIS_MNEMONIC_PMOVMSKB:
        if (x1 < 0) return 0;
        emit_pmovmskb(j, xreg(j, x1)); st_op(j, &ops[0], T0); return 1;
    case ZYDIS_MNEMONIC_MOVMSKPS: {
        if (x1 < 0) return 0;
        int s = xreg(j, x1);
        a64_vushr(&j->a, 2, VT0, s, 31);
        a64_vushr(&j->a, 3, VT1, VT0, 31); a64_vorr(&j->a, VT0, VT0, VT1);
        a64_umov(&j->a, 2, T0, VT0, 0); a64_umov(&j->a, 2, T1, VT0, 2);
        a64_orr_shifted(&j->a, 0, T0, T0, T1, SH_LSL, 2);
        st_op(j, &ops[0], T0); return 1;
    }
    case ZYDIS_MNEMONIC_MOVMSKPD: {
        if (x1 < 0) return 0;
        int s = xreg(j, x1);
        a64_vushr(&j->a, 3, VT0, s, 63);
        a64_umov(&j->a, 2, T0, VT0, 0); a64_umov(&j->a, 2, T1, VT0, 2);
        a64_orr_shifted(&j->a, 0, T0, T0, T1, SH_LSL, 1);
        st_op(j, &ops[0], T0); return 1;
    }

    /* ---- floating point ---- */
    case ZYDIS_MNEMONIC_ADDPS: emit_fp_arith(j, 0, 1, 0); return 1;
    case ZYDIS_MNEMONIC_ADDPD: emit_fp_arith(j, 0, 1, 1); return 1;
    case ZYDIS_MNEMONIC_ADDSS: emit_fp_arith(j, 0, 0, 0); return 1;
    case ZYDIS_MNEMONIC_ADDSD: emit_fp_arith(j, 0, 0, 1); return 1;
    case ZYDIS_MNEMONIC_SUBPS: emit_fp_arith(j, 1, 1, 0); return 1;
    case ZYDIS_MNEMONIC_SUBPD: emit_fp_arith(j, 1, 1, 1); return 1;
    case ZYDIS_MNEMONIC_SUBSS: emit_fp_arith(j, 1, 0, 0); return 1;
    case ZYDIS_MNEMONIC_SUBSD: emit_fp_arith(j, 1, 0, 1); return 1;
    case ZYDIS_MNEMONIC_MULPS: emit_fp_arith(j, 2, 1, 0); return 1;
    case ZYDIS_MNEMONIC_MULPD: emit_fp_arith(j, 2, 1, 1); return 1;
    case ZYDIS_MNEMONIC_MULSS: emit_fp_arith(j, 2, 0, 0); return 1;
    case ZYDIS_MNEMONIC_MULSD: emit_fp_arith(j, 2, 0, 1); return 1;
    case ZYDIS_MNEMONIC_DIVPS: emit_fp_arith(j, 3, 1, 0); return 1;
    case ZYDIS_MNEMONIC_DIVPD: emit_fp_arith(j, 3, 1, 1); return 1;
    case ZYDIS_MNEMONIC_DIVSS: emit_fp_arith(j, 3, 0, 0); return 1;
    case ZYDIS_MNEMONIC_DIVSD: emit_fp_arith(j, 3, 0, 1); return 1;
    case ZYDIS_MNEMONIC_MINPS: emit_fp_arith(j, 4, 1, 0); return 1;
    case ZYDIS_MNEMONIC_MINPD: emit_fp_arith(j, 4, 1, 1); return 1;
    case ZYDIS_MNEMONIC_MINSS: emit_fp_arith(j, 4, 0, 0); return 1;
    case ZYDIS_MNEMONIC_MINSD: emit_fp_arith(j, 4, 0, 1); return 1;
    case ZYDIS_MNEMONIC_MAXPS: emit_fp_arith(j, 5, 1, 0); return 1;
    case ZYDIS_MNEMONIC_MAXPD: emit_fp_arith(j, 5, 1, 1); return 1;
    case ZYDIS_MNEMONIC_MAXSS: emit_fp_arith(j, 5, 0, 0); return 1;
    case ZYDIS_MNEMONIC_MAXSD: emit_fp_arith(j, 5, 0, 1); return 1;
    case ZYDIS_MNEMONIC_SQRTPS: emit_fp_sqrt(j, 1, 0); return 1;
    case ZYDIS_MNEMONIC_SQRTPD: emit_fp_sqrt(j, 1, 1); return 1;
    case ZYDIS_MNEMONIC_SQRTSS: emit_fp_sqrt(j, 0, 0); return 1;
    case ZYDIS_MNEMONIC_SQRTSD: emit_fp_sqrt(j, 0, 1); return 1;
    case ZYDIS_MNEMONIC_CMPPS: emit_fp_cmp(j, 1, 0); return 1;
    case ZYDIS_MNEMONIC_CMPPD: emit_fp_cmp(j, 1, 1); return 1;
    case ZYDIS_MNEMONIC_CMPSS: emit_fp_cmp(j, 0, 0); return 1;
    case ZYDIS_MNEMONIC_CMPSD:
        if (in->meta.category == ZYDIS_CATEGORY_STRINGOP) return 0;
        emit_fp_cmp(j, 0, 1); return 1;
    case ZYDIS_MNEMONIC_COMISS:  emit_fp_comis(j, 0, 1); return 1;
    case ZYDIS_MNEMONIC_COMISD:  emit_fp_comis(j, 1, 1); return 1;
    case ZYDIS_MNEMONIC_UCOMISS: emit_fp_comis(j, 0, 0); return 1;
    case ZYDIS_MNEMONIC_UCOMISD: emit_fp_comis(j, 1, 0); return 1;

    /* ---- conversions ---- */
    case ZYDIS_MNEMONIC_CVTSI2SS: emit_cvt_i2f(j, 0); return 1;
    case ZYDIS_MNEMONIC_CVTSI2SD: emit_cvt_i2f(j, 1); return 1;
    case ZYDIS_MNEMONIC_CVTTSS2SI: emit_cvt_f2i(j, 0, 1); return 1;
    case ZYDIS_MNEMONIC_CVTTSD2SI: emit_cvt_f2i(j, 1, 1); return 1;
    case ZYDIS_MNEMONIC_CVTSS2SI: emit_cvt_f2i(j, 0, 0); return 1;
    case ZYDIS_MNEMONIC_CVTSD2SI: emit_cvt_f2i(j, 1, 0); return 1;
    case ZYDIS_MNEMONIC_CVTSS2SD: {
        int s = xsrc(j, &ops[1], VT0, 32); int d = xreg(j, x0);
        a64_fcvt_sd(&j->a, VT1, s); emit_nan_guard(j, VT1, 0, 1); ins_low(j, d, VT1, 1); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_CVTSD2SS: {
        int s = xsrc(j, &ops[1], VT0, 64); int d = xreg(j, x0);
        a64_fcvt_ds(&j->a, VT1, s); emit_nan_guard(j, VT1, 0, 0); ins_low(j, d, VT1, 0); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_CVTPS2PD: {
        int s = xsrc(j, &ops[1], VT0, 64);
        a64_vfcvtl(&j->a, VT1, s); emit_nan_guard(j, VT1, 1, 1); a64_vmov(&j->a, VREG(x0), VT1); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_CVTPD2PS: {
        int s = xsrc(j, &ops[1], VT0, 128);
        a64_vfcvtn(&j->a, VT1, s); emit_nan_guard(j, VT1, 1, 0); a64_vmov(&j->a, VREG(x0), VT1); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PS: {
        int s = xsrc(j, &ops[1], VT0, 128);
        a64_vscvtf(&j->a, 0, VREG(x0), s); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PD: {
        int s = xsrc(j, &ops[1], VT0, 64);
        a64_vsxtl(&j->a, VT1, s); a64_vscvtf(&j->a, 1, VREG(x0), VT1); xset(j, x0); return 1;
    }
    case ZYDIS_MNEMONIC_CVTPS2DQ:  emit_cvt_ps2dq(j, 0); return 1;
    case ZYDIS_MNEMONIC_CVTTPS2DQ: emit_cvt_ps2dq(j, 1); return 1;

    default:
        return 0;
    }
}
