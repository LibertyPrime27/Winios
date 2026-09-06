/* x87 on NEON doubles. Included by jit.c after the register cache, the
 * callout machinery and jit_sse.h; everything here is `static` and part of
 * the compiler.
 *
 * The idea. Windows sets the x87 precision control to 53 bits (FCW 0x027F),
 * and under PC=53 every FADD/FSUB/FMUL/FDIV/FSQRT result is rounded to
 * exactly the 53-bit significand IEEE double has. The only thing an x87
 * still has over a double is the 15-bit exponent: results below 2^-1022 or
 * at and above 2^1024 stay finite and normal on the FPU where a double
 * would be denormal or infinite. So a stack register whose value *is* a
 * double can be held as one, and its arithmetic done with the host's
 * scalar FP unit, as long as every result is checked to lie in the normal
 * double range (or be an exact zero) -- anything else leaves the block
 * through the interpreter, which redoes the instruction in 80-bit
 * SoftFloat from the untouched inputs. That check is six instructions; the
 * escape almost never runs in real code.
 *
 * The state model. cpu->fpr[] stays the architectural 80-bit file, which
 * the interpreter uses. Beside it, cpu->fpr_d[] holds each register as a
 * double, with fpr_dv saying which of those are exact and fpr_dd which are
 * newer than fpr[] (written by the JIT). C code that reads fpr[] --
 * callouts, the end of xc_run_jit -- first materialises the dd registers
 * (f64 -> f80 is exact, and bit-identical to FLD m64), and after the
 * interpreter has run an x87 instruction the shadow is refreshed from
 * fpr[] (f80 -> f64 when that is exact; else the register is simply not a
 * double and blocks that need it take the slow path).
 *
 * Inside a block the eight physical registers live in d8-d15 (callee-saved,
 * so a C call cannot lose them; the enter stub saves them for the host).
 * TOP and the tag word are compile-time state: compiler-generated code has a
 * fixed stack depth at every address, so a block is compiled for the TOP,
 * tags, FCW and MXCSR rounding seen when it was first reached, and checks
 * that word once, at its first x87 instruction. A mismatch executes that
 * one instruction in the interpreter and leaves; the next block compiles
 * for the new state. FXCH is therefore a renaming of two table entries and
 * costs nothing; FLD/FSTP move TOP at compile time and emit a load or store.
 *
 * Exception flags accumulate in FPSR like the SSE ones do; since both units
 * share it, the flags are folded into whichever status word owns them
 * (MXCSR by default, the FPU while a block is between its first x87 op and
 * its exit) whenever the owner changes. The FPU's own status bits -- C0-C3
 * from compares, C1 cleared by pushes and FXCH, TOP -- are written to fsw at
 * the block exit, or earlier when FNSTSW wants them. Not tracked: the
 * denormal-operand flag (DE) and C1-as-round-up, neither of which the
 * interpreter reports either.
 *
 * Anything else -- 80-bit loads and stores, transcendentals, FPREM, the
 * environment and save/restore instructions, empty-register faults known
 * at compile time -- goes to the interpreter and closes the block, so the
 * compile-time stack model never has to follow the interpreter's changes.
 */

enum { UNIT_MXCSR = 0, UNIT_X87 = 1 };
#define FSW_TOP_MASK_ 0x3800u
#define FREG(p) (8 + (p))                     /* home D register of physical register p */
enum { FT0 = 4, FT1 = 5, FT2 = 6, FT3 = 7 };  /* x87 temporaries (v0-v3 belong to the SSE lowering) */

static void emit_cond_to_w0(jc *j, int cc);
static int x86_cc(ZydisMnemonic m);

/* ---------------------------------------------------------- C helpers */

/* f80 -> f64 when exact. Returns 1 and *out on success. */
static int f80_to_f64_exact(xc_f80 v, double *out) {
    unsigned e = v.se & 0x7FFF; uint64_t sign = (uint64_t)(v.se >> 15) << 63, bits;
    if (e == 0 && v.mant == 0) bits = sign;                                            /* zero */
    else if (e == 0x7FFF) {
        if (!(v.mant >> 63)) return 0;                                                  /* unsupported encoding */
        if (v.mant & 0x7FF) return 0;                                                   /* NaN payload does not fit */
        bits = sign | 0x7FF0000000000000ull | ((v.mant & 0x7FFFFFFFFFFFFFFFull) >> 11); /* inf or NaN */
    } else {
        if (!(v.mant >> 63)) return 0;                                                  /* denormal / pseudo-denormal */
        if (v.mant & 0x7FF) return 0;                                                   /* needs more than 53 bits */
        int de = (int)e - 0x3FFF + 1023;
        if (de < 1 || de > 0x7FE) return 0;                                             /* outside the normal double range */
        bits = sign | ((uint64_t)de << 52) | ((v.mant & 0x7FFFFFFFFFFFFFFFull) >> 11);
    }
    memcpy(out, &bits, 8);
    return 1;
}
/* f64 -> f80, exact (what FLD m64 does). */
static xc_f80 f64_to_f80(double d) {
    uint64_t bits; memcpy(&bits, &d, 8);
    xc_f80 r; uint16_t sign = (uint16_t)((bits >> 63) << 15);
    unsigned e = (unsigned)((bits >> 52) & 0x7FF); uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    if (e == 0) {
        if (!frac) { r.mant = 0; r.se = sign; return r; }
        int sh = 0; while (!(frac >> 52)) { frac <<= 1; sh++; }                        /* denormal: normalise */
        r.mant = frac << 11; r.se = (uint16_t)(sign | (0x3FFF - 1022 - sh)); return r;
    }
    if (e == 0x7FF) { r.mant = (1ull << 63) | (frac << 11); r.se = (uint16_t)(sign | 0x7FFF); return r; }
    r.mant = (1ull << 63) | (frac << 11); r.se = (uint16_t)(sign | (e - 1023 + 0x3FFF));
    return r;
}
static void x87_materialize(xc_cpu *c) {
    if (!c->fpr_dd) return;
    for (int p = 0; p < 8; p++) if (c->fpr_dd & (1u << p)) c->fpr[p] = f64_to_f80(c->fpr_d[p]);
    c->fpr_dd = 0;
}
static void x87_refresh(xc_cpu *c) {
    uint8_t dv = 0;
    for (int p = 0; p < 8; p++) if (f80_to_f64_exact(c->fpr[p], &c->fpr_d[p])) dv |= (uint8_t)(1u << p);
    c->fpr_dv = dv; c->fpr_dd = 0;
}

/* -------------------------------------------------------- compile state */

/* The guard word: FCW | TOP<<16 | tags<<32 | MXCSR.RC<<40 | DAZ<<42 | FTZ<<43. */
static uint64_t x87_mode_word(uint16_t fcw, uint16_t fsw, uint8_t tags, uint32_t mxcsr) {
    return (uint64_t)fcw | ((uint64_t)(fsw & FSW_TOP_MASK_) << 16) | ((uint64_t)tags << 32) |
           ((uint64_t)((mxcsr >> 13) & 3) << 40) | ((uint64_t)((mxcsr >> 6) & 1) << 42) | ((uint64_t)((mxcsr >> 15) & 1) << 43);
}
static void x87_block_begin(jc *j, const xc_cpu *c) {
    for (int p = 0; p < 8; p++) j->x87_v[p] = FREG(p);
    j->x87_top = (c->fsw >> 11) & 7;
    j->x87_empty = j->x87_empty0 = c->ftag_empty;
    j->x87_rc = (c->fcw >> 10) & 3;
    j->x87_fcw = c->fcw; j->x87_mxcsr = c->mxcsr;
    j->fp_unit = UNIT_MXCSR;
    /* the fast path needs: 53-bit precision, all exceptions masked, the FPU
     * and SSE rounding modes equal (one FPCR), no flush-to-zero, and every
     * valid register already a double */
    int pc = (c->fcw >> 8) & 3;
    j->x87_on = pc == 2 && (c->fcw & 0x3F) == 0x3F && ((c->fcw >> 10) & 3) == ((c->mxcsr >> 13) & 3) &&
                !(c->mxcsr & 0x8040) && (uint8_t)(c->fpr_dv | c->ftag_empty) == 0xFF;
    if (getenv("XCORE_JIT_X87") && getenv("XCORE_JIT_X87")[0] == '0') j->x87_on = 0;
}

/* spill()/flush() call the writeback below, and callers hold live values in
 * x0/x1 and rely on NZCV across the call (a Jcc's condition, a computed exit
 * address). So the whole writeback path uses only x2/x3/x28 as scratch and
 * never sets the flags -- the "are any FPSR flags pending" test is a cbz, not
 * an ands. */
#define SA T2       /* x2 */
#define SB T3       /* x3 */
#define SC R_TMP    /* x28 */

/* FPSR -> the owning status word, and FPSR cleared. Same bit mapping for
 * both (IE 0, DE 1, ZE 2, OE 3, UE 4, PE 5 <- IOC 0, IDC 7, DZC 1, OFC 2,
 * UFC 3, IXC 4). Compile state: only the owner changes. */
static void x87_emit_fold(jc *j, int to_fsw) {
    a64_mrs_fpsr(&j->a, SA);
    a64_mov_imm(&j->a, SB, 0x9f);
    a64_and(&j->a, 0, SB, SA, SB);
    uint32_t skip = a64_here(&j->a); a64_cbz(&j->a, 0, SB, 0);
    if (to_fsw) a64_ldr_off(&j->a, 1, SB, R_CPU, OFF(fsw)); else a64_ldr_off(&j->a, 2, SB, R_CPU, OFF(mxcsr));
    a64_mov_imm(&j->a, SC, 0x1e);
    a64_and(&j->a, 0, SC, SA, SC);
    a64_orr_shifted(&j->a, 0, SB, SB, SC, SH_LSL, 1);           /* DZC OFC UFC IXC -> ZE OE UE PE */
    a64_ubfx(&j->a, 0, SC, SA, 0, 1);
    a64_orr(&j->a, 0, SB, SB, SC);                              /* IOC -> IE */
    a64_ubfx(&j->a, 0, SC, SA, 7, 1);
    a64_orr_shifted(&j->a, 0, SB, SB, SC, SH_LSL, 1);           /* IDC -> DE */
    if (to_fsw) a64_str_off(&j->a, 1, SB, R_CPU, OFF(fsw)); else a64_str_off(&j->a, 2, SB, R_CPU, OFF(mxcsr));
    a64_msr_fpsr(&j->a, ZR);
    a64_patch_bcond(&j->a, skip, a64_here(&j->a));              /* patches the cbz */
}
static void x87_fold_fpsr(jc *j, int to_fsw) { x87_emit_fold(j, to_fsw); j->fp_unit = to_fsw ? UNIT_MXCSR : UNIT_X87; j->x87_ixc = 0; }

/* Leave through the interpreter from an x87 slow path. The op that sent us
 * here may have raised flags the FPU would not have (a double overflowed
 * where an f80 does not, a NaN compare, ...); the interpreter redoes it and
 * sets the true ones. On the fast path only IXC is ever left pending (every
 * other flag means the result was refused), so those bits are the aborted
 * op's alone and are dropped. Its IXC is dropped too when no earlier op
 * could have left one; otherwise it stays, which can only set PE early. */
static void x87_slow_exit(jc *j) {
    if (j->fp_unit == UNIT_X87) {
        if (!j->x87_ixc) a64_msr_fpsr(&j->a, ZR);
        else { a64_mrs_fpsr(&j->a, T1); a64_mov_imm(&j->a, T2, 0x90); a64_and(&j->a, 0, T1, T1, T2); a64_msr_fpsr(&j->a, T1); }
    }
    emit_slow_exit(j);
}

/* Everything the block knows about the FPU -> cpu, without changing compile
 * state (slow paths and exits share it). */
static void x87_writeback(jc *j) {
    for (int p = 0; p < 8; p++) if (j->x87_dirty & (1u << p)) a64_fldst_off(&j->a, 1, 0, j->x87_v[p], R_CPU, OFF(fpr_d) + 8u * p);
    if (j->x87_dirty) {                                          /* fpr_dv | fpr_dd << 8, one halfword */
        a64_ldr_off(&j->a, 1, SA, R_CPU, OFF(fpr_dv));
        a64_mov_imm(&j->a, SB, (uint32_t)j->x87_dirty | ((uint32_t)j->x87_dirty << 8));
        a64_orr(&j->a, 0, SA, SA, SB);
        a64_str_off(&j->a, 1, SA, R_CPU, OFF(fpr_dv));
    }
    if (j->x87_touched || j->fp_unit == UNIT_X87) {
        if (j->fp_unit == UNIT_X87) x87_emit_fold(j, 1);
        a64_ldr_off(&j->a, 1, SA, R_CPU, OFF(fsw));
        uint32_t keep = 0xFFFFu & ~(FSW_TOP_MASK_ | 0x8080u | (j->x87_c1clr ? 0x0200u : 0));
        a64_mov_imm(&j->a, SB, keep);
        a64_and(&j->a, 0, SA, SA, SB);
        if (j->x87_top) { a64_mov_imm(&j->a, SB, (uint32_t)j->x87_top << 11); a64_orr(&j->a, 0, SA, SA, SB); }
        a64_str_off(&j->a, 1, SA, R_CPU, OFF(fsw));
        if (j->x87_empty != j->x87_empty0) { a64_mov_imm(&j->a, SA, j->x87_empty); a64_str_off(&j->a, 0, SA, R_CPU, OFF(ftag_empty)); }
    }
}
static void x87_spill(jc *j) { if (j->x87_dirty || j->x87_touched || j->fp_unit == UNIT_X87) x87_writeback(j); }
static void x87_flushed(jc *j) {
    j->x87_dirty = 0; j->x87_touched = 0; j->x87_c1clr = 0; j->x87_empty0 = j->x87_empty;
    j->fp_unit = UNIT_MXCSR; j->x87_ixc = 0;
}
static void x87_invalidate(jc *j) { j->x87_loaded = 0; for (int p = 0; p < 8; p++) j->x87_v[p] = FREG(p); }

/* The guard, once per block: the runtime FCW/TOP/tags/MXCSR must be what the
 * block was compiled for, and every valid register must be a double. */
static void x87_guard(jc *j) {
    if (j->x87_guarded) return;
    /* against the *current* static state: after a mid-block C call the block
     * has written its TOP and tags back, and re-checks from there */
    uint64_t mode = x87_mode_word(j->x87_fcw, (uint16_t)(j->x87_top << 11), j->x87_empty, j->x87_mxcsr);
    a64_ldr_off(&j->a, 1, T0, R_CPU, OFF(fcw));
    a64_ldr_off(&j->a, 1, T1, R_CPU, OFF(fsw));
    a64_mov_imm(&j->a, T2, FSW_TOP_MASK_);
    a64_and(&j->a, 0, T1, T1, T2);
    a64_orr_shifted(&j->a, 1, T0, T0, T1, SH_LSL, 16);
    a64_ldr_off(&j->a, 0, T1, R_CPU, OFF(ftag_empty));
    a64_orr_shifted(&j->a, 1, T0, T0, T1, SH_LSL, 32);
    a64_ldr_off(&j->a, 0, T2, R_CPU, OFF(fpr_dv));
    a64_orr(&j->a, 0, T1, T1, T2);
    a64_ldr_off(&j->a, 2, T2, R_CPU, OFF(mxcsr));
    a64_ubfx(&j->a, 0, T3, T2, 13, 2); a64_orr_shifted(&j->a, 1, T0, T0, T3, SH_LSL, 40);
    a64_ubfx(&j->a, 0, T3, T2, 6, 1);  a64_orr_shifted(&j->a, 1, T0, T0, T3, SH_LSL, 42);
    a64_ubfx(&j->a, 0, T3, T2, 15, 1); a64_orr_shifted(&j->a, 1, T0, T0, T3, SH_LSL, 43);
    a64_mov_imm(&j->a, T2, mode);
    a64_cmp(&j->a, 1, T0, T2);
    uint32_t miss = a64_here(&j->a); a64_bcond(&j->a, CC_NE, 0);
    a64_cmp_imm(&j->a, 0, T1, 0xFF);
    uint32_t ok = a64_here(&j->a); a64_bcond(&j->a, CC_EQ, 0);
    a64_patch_bcond(&j->a, miss, a64_here(&j->a));
    emit_slow_exit(j);                                           /* this instruction in the interpreter, then leave */
    a64_patch_bcond(&j->a, ok, a64_here(&j->a));
    j->nz = NZ_NONE;
    j->x87_guarded = 1;
}

/* ------------------------------------------------------ the stack model */

static int x87_phys(const jc *j, int i) { return (j->x87_top + i) & 7; }
static int x87_is_empty(const jc *j, int i) { return (j->x87_empty >> x87_phys(j, i)) & 1; }
/* host register holding ST(i), loading it if needed */
static int fst(jc *j, int i) {
    int p = x87_phys(j, i);
    if (!(j->x87_loaded & (1u << p))) { a64_fldst_off(&j->a, 1, 1, j->x87_v[p], R_CPU, OFF(fpr_d) + 8u * p); j->x87_loaded |= (uint8_t)(1u << p); }
    return j->x87_v[p];
}
/* ST(i) is about to be fully written: its host register */
static int fset(jc *j, int i) {
    int p = x87_phys(j, i);
    j->x87_loaded |= (uint8_t)(1u << p); j->x87_dirty |= (uint8_t)(1u << p);
    j->x87_empty &= (uint8_t)~(1u << p);
    j->x87_touched = 1;
    return j->x87_v[p];
}
static void fpush(jc *j) { j->x87_top = (j->x87_top - 1) & 7; j->x87_c1clr = 1; j->x87_touched = 1; }
static void fpop(jc *j) { j->x87_empty |= (uint8_t)(1u << j->x87_top); j->x87_top = (j->x87_top + 1) & 7; j->x87_touched = 1; }
/* the FPSR flags an x87 op is about to raise belong to the FPU */
static void x87_unit(jc *j) { if (j->fp_unit != UNIT_X87) x87_fold_fpsr(j, 0); }

/* Operand helpers */
static int st_of(const xop *o) { return o->type == XOP_REG && o->rcls == XR_X87 ? o->ridx : -1; }
/* a memory operand as a double in vt: m32/m64 floats (exact), m16/m32/m64 integers (exact up to 2^53;
 * a 64-bit integer that does not round-trip takes the slow path) */
static int x87_mem_src(jc *j, const xop *op, int integer, int vt) {
    int w = emit_ea(j, op, T4);
    emit_bounds(j, op->size / 8);
    if (!integer) {
        if (op->size == 64) { a64_fldst_reg(&j->a, 1, 1, vt, R_BASE, T4, w ? 2 : 3); return vt; }
        if (op->size == 32) { a64_fldst_reg(&j->a, 0, 1, vt, R_BASE, T4, w ? 2 : 3); a64_fcvt_sd(&j->a, vt, vt); return vt; }
        j->failed = 1; return vt;
    }
    if (op->size == 64) {
        a64_ldr_reg(&j->a, 3, T0, R_BASE, T4, w ? 2 : 3);
        a64_scvtf(&j->a, 1, 1, vt, T0);
        a64_fcvtzs(&j->a, 1, 1, T1, vt);
        a64_cmp(&j->a, 1, T0, T1);
        uint32_t ok = a64_here(&j->a); a64_bcond(&j->a, CC_EQ, 0);
        x87_slow_exit(j);
        a64_patch_bcond(&j->a, ok, a64_here(&j->a));
        j->nz = NZ_NONE;
        return vt;
    }
    a64_ldrs_reg(&j->a, op->size == 32 ? 2 : 1, 1, T0, R_BASE, T4, w ? 2 : 3);
    a64_scvtf(&j->a, 1, 1, vt, T0);
    return vt;
}

/* Result in vr is safe to keep as a double: a normal number other than
 * DBL_MIN itself (a result that rounded up into the normal range from below
 * is DBL_MIN, and the FPU would have kept it below), or an exact zero.
 * `zero_ok`: 1 when a zero result is always exact (add, sub, sqrt), 0 when
 * it may be an underflow (mul, div) -- then it is fine only if an input
 * (va, or vb) is zero. Anything else: the interpreter, from the untouched
 * inputs. Compile state unchanged. */
static void x87_range_guard(jc *j, int vr, int zero_ok, int va, int vb) {
    a64_fmov_xd(&j->a, T0, vr);
    a64_ubfx(&j->a, 1, T1, T0, 52, 11);
    a64_sub_imm(&j->a, 1, T1, T1, 1);
    a64_cmp_imm(&j->a, 1, T1, 0x7FD);
    uint32_t notnormal = a64_here(&j->a); a64_bcond(&j->a, CC_HI, 0);
    uint32_t ok1 = a64_here(&j->a); a64_cbnz(&j->a, 1, T1, 0);                  /* exponent 2..0x7FE: fine */
    a64_lsl_imm(&j->a, 1, T2, T0, 12);
    uint32_t ok2 = a64_here(&j->a); a64_cbnz(&j->a, 1, T2, 0);                  /* exponent 1 with a fraction: fine; DBL_MIN: not */
    uint32_t slow1 = a64_here(&j->a); a64_b(&j->a, 0);
    a64_patch_bcond(&j->a, notnormal, a64_here(&j->a));
    /* exponent 0 (zero or denormal) or 0x7FF (inf, NaN) */
    a64_lsl_imm(&j->a, 1, T2, T0, 1);
    uint32_t slow2 = a64_here(&j->a); a64_cbnz(&j->a, 1, T2, 0);                /* anything but a zero */
    uint32_t ok3 = 0, ok4 = 0;
    if (zero_ok) { ok3 = a64_here(&j->a); a64_b(&j->a, 0); }
    else {
        a64_fcmp0(&j->a, 1, va); ok3 = a64_here(&j->a); a64_bcond(&j->a, CC_EQ, 0);
        if (vb >= 0) { a64_fcmp0(&j->a, 1, vb); ok4 = a64_here(&j->a); a64_bcond(&j->a, CC_EQ, 0); }
    }
    uint32_t slow = a64_here(&j->a);
    a64_patch_b(&j->a, slow1, slow); a64_patch_bcond(&j->a, slow2, slow);
    x87_slow_exit(j);
    uint32_t ok = a64_here(&j->a);
    a64_patch_bcond(&j->a, ok1, ok); a64_patch_bcond(&j->a, ok2, ok);
    if (zero_ok) a64_patch_b(&j->a, ok3, ok); else { a64_patch_bcond(&j->a, ok3, ok); if (ok4) a64_patch_bcond(&j->a, ok4, ok); }
    j->nz = NZ_NONE;
    j->x87_ixc = 1;
}
/* The compare left NZCV; unordered (a NaN somewhere) is the interpreter's:
 * IE for FCOM/FCOMI or an SNaN, and the C3=C2=C0=1 result. */
static void x87_unordered_guard(jc *j) {
    uint32_t ok = a64_here(&j->a); a64_bcond(&j->a, CC_VC, 0);
    x87_slow_exit(j);
    a64_patch_bcond(&j->a, ok, a64_here(&j->a));
}
/* FST m32: the value must convert to a normal float (or zero) so the only
 * flag the conversion can raise is inexact. */
static void x87_float_range_guard(jc *j, int v) {
    a64_fmov_xd(&j->a, T0, v);
    a64_ubfx(&j->a, 1, T1, T0, 52, 11);
    a64_sub_imm(&j->a, 1, T1, T1, 0x381);                                        /* 2^-126 .. */
    a64_cmp_imm(&j->a, 1, T1, 0x47E - 0x381);                                    /* .. < 2^128 */
    uint32_t ok1 = a64_here(&j->a); a64_bcond(&j->a, CC_LS, 0);
    a64_lsl_imm(&j->a, 1, T1, T0, 1);
    uint32_t ok2 = a64_here(&j->a); a64_cbz(&j->a, 1, T1, 0);                    /* +/- 0 */
    x87_slow_exit(j);
    uint32_t ok = a64_here(&j->a);
    a64_patch_bcond(&j->a, ok1, ok); a64_patch_bcond(&j->a, ok2, ok);
    j->nz = NZ_NONE;
}
/* A value loaded as-is (FLD m32/m64): a NaN would need quieting / IE -- the
 * interpreter's. Tested on the integer bits, not with fcmp: fcmp of a
 * signaling NaN raises IOC in FPSR, and here the FPU does not yet own it. */
static void x87_nan_guard(jc *j, int v) {
    a64_fmov_xd(&j->a, T0, v);
    a64_lsl_imm(&j->a, 1, T0, T0, 1);                            /* drop the sign; inf<<1 = 0xFFE0.., NaN<<1 is above it */
    a64_mov_imm(&j->a, T1, 0xFFE0000000000000ull);
    a64_cmp(&j->a, 1, T0, T1);
    uint32_t ok = a64_here(&j->a); a64_bcond(&j->a, CC_LS, 0);
    x87_slow_exit(j);
    a64_patch_bcond(&j->a, ok, a64_here(&j->a));
    j->nz = NZ_NONE;
}

/* The instruction goes to the interpreter and the block ends there. */
static void x87_callout_end(jc *j) {
    emit_callout(j);
    emit_exit_imm(j, j->d->rip + j->d->in.length);
    j->ended = 1;
}

/* fsw <- the compare's C3:C2:C0 (and C1 cleared), from NZCV after an fcmp */
static void x87_cc_to_fsw(jc *j) {
    a64_cset(&j->a, 0, T0, CC_MI);                                   /* less */
    a64_cset(&j->a, 0, T1, CC_EQ);                                   /* equal */
    a64_cset(&j->a, 0, T2, CC_VS);                                   /* unordered */
    a64_orr(&j->a, 0, T0, T0, T2);                                   /* C0 = less | unordered */
    a64_orr(&j->a, 0, T1, T1, T2);                                   /* C3 = equal | unordered */
    a64_ldr_off(&j->a, 1, T3, R_CPU, OFF(fsw));
    a64_mov_imm(&j->a, T4, 0xFFFFu & ~(0x4000u | 0x0400u | 0x0200u | 0x0100u));
    a64_and(&j->a, 0, T3, T3, T4);
    a64_orr_shifted(&j->a, 0, T3, T3, T0, SH_LSL, 8);
    a64_orr_shifted(&j->a, 0, T3, T3, T2, SH_LSL, 10);
    a64_orr_shifted(&j->a, 0, T3, T3, T1, SH_LSL, 14);
    a64_str_off(&j->a, 1, T3, R_CPU, OFF(fsw));
    j->nz = NZ_NONE;
}
/* rflags <- ZF,PF,CF from the compare; OF,SF,AF cleared (FCOMI) */
static void x87_cc_to_rflags(jc *j) {
    a64_cset(&j->a, 0, T0, CC_MI);
    a64_cset(&j->a, 0, T1, CC_VS);
    a64_cset(&j->a, 0, T2, CC_EQ);
    a64_orr(&j->a, 0, T0, T0, T1);
    a64_orr(&j->a, 0, T2, T2, T1);
    a64_ldr_off(&j->a, 3, T3, R_CPU, OFF(rflags));
    a64_mov_imm(&j->a, T4, XC_ARITH_FLAGS);
    a64_bic_reg(&j->a, 1, T3, T3, T4);
    a64_orr(&j->a, 1, T3, T3, T0);
    a64_orr_shifted(&j->a, 1, T3, T3, T1, SH_LSL, 2);
    a64_orr_shifted(&j->a, 1, T3, T3, T2, SH_LSL, 6);
    a64_str_off(&j->a, 3, T3, R_CPU, OFF(rflags));
    a64_str_off(&j->a, 2, ZR, R_CPU, OFF(lz_op));
    j->lz = LZ_VALID;
    j->nz = NZ_FCMP;
}

/* ------------------------------------------------------------ lowering */

static void emit_x87(jc *j) {
    const ZydisDecodedInstruction *in = &j->d->in;
    const xop *ops = j->ops;
    ZydisMnemonic m = in->mnemonic;
    int nvis = in->operand_count_visible;

    if (!j->x87_on) { emit_callout(j); return; }               /* PC=64 (or an odd mode): the interpreter, as before */

    /* control-word traffic needs no stack state */
    if (m == ZYDIS_MNEMONIC_FNSTCW && ops[0].type == XOP_MEM) { a64_ldr_off(&j->a, 1, T0, R_CPU, OFF(fcw)); st_op(j, &ops[0], T0); return; }
    if (m == ZYDIS_MNEMONIC_FLDCW && ops[0].type == XOP_MEM) {
        /* the rest of the block was compiled for the old mode: end it here */
        int r = ld_op(j, &ops[0], T0, 0);
        a64_mov_imm(&j->a, T1, 0x1F7F); a64_and(&j->a, 0, T0, r, T1);
        a64_mov_imm(&j->a, T1, 0x40); a64_orr(&j->a, 0, T0, T0, T1);
        a64_str_off(&j->a, 1, T0, R_CPU, OFF(fcw));
        emit_exit_imm(j, j->d->rip + in->length);
        j->ended = 1;
        return;
    }

    x87_guard(j);

    switch (m) {
    /* ---- loads ---- */
    case ZYDIS_MNEMONIC_FLD: {
        if (!x87_is_empty(j, 7)) break;                           /* push into a full stack: the interpreter's fault */
        if (ops[0].type == XOP_MEM) {
            if (ops[0].size == 80) break;
            x87_mem_src(j, &ops[0], 0, FT0);
            x87_nan_guard(j, FT0);
            fpush(j); a64_fmov_ss(&j->a, 1, fset(j, 0), FT0);
        } else {
            int i = st_of(&ops[0]);
            if (x87_is_empty(j, i)) break;
            int s = fst(j, i);
            fpush(j); a64_fmov_ss(&j->a, 1, fset(j, 0), s);
        }
        return;
    }
    case ZYDIS_MNEMONIC_FILD:
        if (!x87_is_empty(j, 7)) break;
        x87_mem_src(j, &ops[0], 1, FT0);
        fpush(j); a64_fmov_ss(&j->a, 1, fset(j, 0), FT0);
        return;
    case ZYDIS_MNEMONIC_FLDZ:
        if (!x87_is_empty(j, 7)) break;
        fpush(j); a64_fmov_dx(&j->a, fset(j, 0), ZR); return;
    case ZYDIS_MNEMONIC_FLD1:
        if (!x87_is_empty(j, 7)) break;
        a64_mov_imm(&j->a, T0, 0x3FF0000000000000ull);
        fpush(j); a64_fmov_dx(&j->a, fset(j, 0), T0); return;

    /* ---- stores ---- */
    case ZYDIS_MNEMONIC_FST: case ZYDIS_MNEMONIC_FSTP: {
        if (x87_is_empty(j, 0)) break;
        int s = fst(j, 0);
        if (ops[0].type == XOP_MEM) {
            if (ops[0].size == 80) break;
            int w = emit_ea(j, &ops[0], T4);
            emit_bounds(j, ops[0].size / 8);
            if (ops[0].size == 64) a64_fldst_reg(&j->a, 1, 0, s, R_BASE, T4, w ? 2 : 3);
            else { x87_unit(j); x87_float_range_guard(j, s); a64_fcvt_ds(&j->a, FT0, s); a64_fldst_reg(&j->a, 0, 0, FT0, R_BASE, T4, w ? 2 : 3); j->x87_ixc = 1; }
        } else {
            int i = st_of(&ops[0]);
            int d = fset(j, i);
            if (d != s) a64_fmov_ss(&j->a, 1, d, s);
        }
        if (m == ZYDIS_MNEMONIC_FSTP) fpop(j);
        return;
    }
    case ZYDIS_MNEMONIC_FIST: case ZYDIS_MNEMONIC_FISTP: case ZYDIS_MNEMONIC_FISTTP: {
        if (x87_is_empty(j, 0) || ops[0].type != XOP_MEM) break;
        int s = fst(j, 0);
        int bits = ops[0].size;
        static const int rmode[4] = { 0, 2, 1, 3 };              /* x86 RN, RM, RP, RZ -> A64 N, M, P, Z */
        int rm = m == ZYDIS_MNEMONIC_FISTTP ? 3 : rmode[j->x87_rc];
        x87_unit(j);
        /* NaN -> indefinite, out of range -> indefinite + IE: the interpreter's */
        a64_fcmp(&j->a, 1, s, s);
        uint32_t nan = a64_here(&j->a); a64_bcond(&j->a, CC_VS, 0);
        a64_fcvts(&j->a, 1, 1, rm, T0, s);                       /* 64-bit result, saturating */
        uint32_t bad;
        if (bits == 64) {
            a64_eor_asr(&j->a, 1, T1, T0, T0, 63);               /* saturated <=> T0 is INT64_MIN/MAX <=> T1 == INT64_MAX */
            a64_add_imm(&j->a, 1, T1, T1, 1);
            bad = a64_here(&j->a); a64_tbnz(&j->a, T1, 63, 0);
        } else if (bits == 32) {
            a64_sxtw(&j->a, T1, T0);
            a64_cmp(&j->a, 1, T0, T1);
            bad = a64_here(&j->a); a64_bcond(&j->a, CC_NE, 0);
        } else {
            a64_sxth(&j->a, 1, T1, T0);
            a64_cmp(&j->a, 1, T0, T1);
            bad = a64_here(&j->a); a64_bcond(&j->a, CC_NE, 0);
        }
        int w = emit_ea(j, &ops[0], T4);
        emit_bounds(j, bits / 8);
        a64_str_reg(&j->a, ldst_size(bits), T0, R_BASE, T4, w ? 2 : 3);
        uint32_t done = a64_here(&j->a); a64_b(&j->a, 0);
        a64_patch_bcond(&j->a, nan, a64_here(&j->a)); a64_patch_bcond(&j->a, bad, a64_here(&j->a));
        x87_slow_exit(j);
        a64_patch_b(&j->a, done, a64_here(&j->a));
        j->nz = NZ_NONE; j->x87_ixc = 1;
        if (m != ZYDIS_MNEMONIC_FIST) fpop(j);
        return;
    }

    /* ---- stack ---- */
    case ZYDIS_MNEMONIC_FXCH: {
        int i = 1;
        for (int k = 0; k < nvis; k++) if (st_of(&ops[k]) > 0) i = st_of(&ops[k]);
        if (x87_is_empty(j, 0) || x87_is_empty(j, i)) break;
        int p0 = x87_phys(j, 0), pi = x87_phys(j, i);
        fst(j, 0); fst(j, i);
        int t = j->x87_v[p0]; j->x87_v[p0] = j->x87_v[pi]; j->x87_v[pi] = t;
        j->x87_dirty |= (uint8_t)((1u << p0) | (1u << pi));
        j->x87_c1clr = 1; j->x87_touched = 1;
        return;
    }
    case ZYDIS_MNEMONIC_FINCSTP: j->x87_top = (j->x87_top + 1) & 7; j->x87_c1clr = 1; j->x87_touched = 1; return;
    case ZYDIS_MNEMONIC_FDECSTP: j->x87_top = (j->x87_top - 1) & 7; j->x87_c1clr = 1; j->x87_touched = 1; return;
    case ZYDIS_MNEMONIC_FFREE:  j->x87_empty |= (uint8_t)(1u << x87_phys(j, st_of(&ops[0]))); j->x87_touched = 1; return;
    case ZYDIS_MNEMONIC_FFREEP: j->x87_empty |= (uint8_t)(1u << x87_phys(j, st_of(&ops[0]))); fpop(j); return;

    /* ---- arithmetic ---- */
    case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD:
    case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP: case ZYDIS_MNEMONIC_FISUB:
    case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP: case ZYDIS_MNEMONIC_FISUBR:
    case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL:
    case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV:
    case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR: {
        int op, pop = 0, integer = 0;                            /* op: 0 add 1 sub 2 subr 3 mul 4 div 5 divr */
        switch (m) {
        case ZYDIS_MNEMONIC_FADDP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIADD: integer = m == ZYDIS_MNEMONIC_FIADD; /* fallthrough */
        case ZYDIS_MNEMONIC_FADD: op = 0; break;
        case ZYDIS_MNEMONIC_FSUBP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FISUB: integer = m == ZYDIS_MNEMONIC_FISUB; /* fallthrough */
        case ZYDIS_MNEMONIC_FSUB: op = 1; break;
        case ZYDIS_MNEMONIC_FSUBRP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FISUBR: integer = m == ZYDIS_MNEMONIC_FISUBR; /* fallthrough */
        case ZYDIS_MNEMONIC_FSUBR: op = 2; break;
        case ZYDIS_MNEMONIC_FMULP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIMUL: integer = m == ZYDIS_MNEMONIC_FIMUL; /* fallthrough */
        case ZYDIS_MNEMONIC_FMUL: op = 3; break;
        case ZYDIS_MNEMONIC_FDIVP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIDIV: integer = m == ZYDIS_MNEMONIC_FIDIV; /* fallthrough */
        case ZYDIS_MNEMONIC_FDIV: op = 4; break;
        case ZYDIS_MNEMONIC_FDIVRP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIDIVR: integer = m == ZYDIS_MNEMONIC_FIDIVR; /* fallthrough */
        default: op = 5; break;
        }
        int dst = 0, a, b;
        if (nvis == 0) {                                         /* ST1 op= ST0, pop */
            if (x87_is_empty(j, 0) || x87_is_empty(j, 1)) break;
            dst = 1; a = fst(j, 1); b = fst(j, 0);
        } else if (ops[0].type == XOP_MEM) {                     /* ST0 op= mem */
            if (x87_is_empty(j, 0)) break;
            b = x87_mem_src(j, &ops[0], integer, FT0);
            a = fst(j, 0);
        } else {                                                 /* ST(i) op= ST(j) */
            dst = st_of(&ops[0]);
            if (x87_is_empty(j, dst) || x87_is_empty(j, st_of(&ops[1]))) break;
            a = fst(j, dst); b = fst(j, st_of(&ops[1]));
        }
        x87_unit(j);
        switch (op) {
        case 0: a64_fadd(&j->a, 1, FT1, a, b); break;
        case 1: a64_fsub(&j->a, 1, FT1, a, b); break;
        case 2: a64_fsub(&j->a, 1, FT1, b, a); break;
        case 3: a64_fmul(&j->a, 1, FT1, a, b); break;
        case 4: a64_fdiv(&j->a, 1, FT1, a, b); break;
        default: a64_fdiv(&j->a, 1, FT1, b, a); break;
        }
        x87_range_guard(j, FT1, op <= 2, a, b);
        a64_fmov_ss(&j->a, 1, fset(j, dst), FT1);
        if (pop) fpop(j);
        return;
    }
    case ZYDIS_MNEMONIC_FCHS: { if (x87_is_empty(j, 0)) break; int s = fst(j, 0); a64_fneg(&j->a, 1, fset(j, 0), s); j->x87_c1clr = 1; return; }
    case ZYDIS_MNEMONIC_FABS: { if (x87_is_empty(j, 0)) break; int s = fst(j, 0); a64_fabs(&j->a, 1, fset(j, 0), s); j->x87_c1clr = 1; return; }
    case ZYDIS_MNEMONIC_FSQRT: {
        if (x87_is_empty(j, 0)) break;
        int s = fst(j, 0);
        x87_unit(j);
        a64_fsqrt(&j->a, 1, FT1, s);
        x87_range_guard(j, FT1, 1, s, -1);
        a64_fmov_ss(&j->a, 1, fset(j, 0), FT1);
        return;
    }
    case ZYDIS_MNEMONIC_FRNDINT: {
        if (x87_is_empty(j, 0)) break;
        int s = fst(j, 0);
        x87_unit(j);
        a64_frintx(&j->a, 1, FT1, s);                            /* current rounding mode, inexact when it changed the value */
        x87_range_guard(j, FT1, 1, s, -1);                       /* NaN, inf: the interpreter's flags */
        a64_fmov_ss(&j->a, 1, fset(j, 0), FT1);
        return;
    }

    /* ---- compares ---- */
    case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP:
    case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP: {
        int ordered = m == ZYDIS_MNEMONIC_FCOM || m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
        int integer = m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
        if (x87_is_empty(j, 0)) break;
        const xop *src = nvis >= 1 ? &ops[nvis - 1] : 0;
        int a = fst(j, 0), b;
        if (src && src->type == XOP_MEM) b = x87_mem_src(j, src, integer, FT0);
        else { int i = src && st_of(src) >= 0 ? st_of(src) : 1; if (x87_is_empty(j, i)) break; b = fst(j, i); }
        x87_unit(j);
        if (ordered) a64_fcmpe(&j->a, 1, a, b); else a64_fcmp(&j->a, 1, a, b);
        x87_unordered_guard(j);
        x87_cc_to_fsw(j);
        j->x87_c1clr = 1; j->x87_touched = 1;
        if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP || m == ZYDIS_MNEMONIC_FICOMP) fpop(j);
        return;
    }
    case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOMPP: {
        if (x87_is_empty(j, 0) || x87_is_empty(j, 1)) break;
        int a = fst(j, 0), b = fst(j, 1);
        x87_unit(j);
        if (m == ZYDIS_MNEMONIC_FCOMPP) a64_fcmpe(&j->a, 1, a, b); else a64_fcmp(&j->a, 1, a, b);
        x87_unordered_guard(j);
        x87_cc_to_fsw(j);
        j->x87_c1clr = 1;
        fpop(j); fpop(j);
        return;
    }
    case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
        int i = st_of(&ops[nvis - 1]);
        if (x87_is_empty(j, 0) || x87_is_empty(j, i)) break;
        int a = fst(j, 0), b = fst(j, i);
        x87_unit(j);
        if (m == ZYDIS_MNEMONIC_FCOMI || m == ZYDIS_MNEMONIC_FCOMIP) a64_fcmpe(&j->a, 1, a, b); else a64_fcmp(&j->a, 1, a, b);
        x87_unordered_guard(j);                                 /* (the slow path is a branch away: NZCV is still the compare's) */
        x87_cc_to_rflags(j);
        j->x87_c1clr = 1; j->x87_touched = 1;
        if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) fpop(j);
        return;
    }
    case ZYDIS_MNEMONIC_FTST: {
        if (x87_is_empty(j, 0)) break;
        int a = fst(j, 0);
        x87_unit(j);
        a64_fcmpe0(&j->a, 1, a);
        x87_unordered_guard(j);
        x87_cc_to_fsw(j);
        j->x87_c1clr = 1; j->x87_touched = 1;
        return;
    }
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVU:
    case ZYDIS_MNEMONIC_FCMOVNB: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNU: {
        int i = st_of(&ops[nvis - 1]);
        if (x87_is_empty(j, 0) || x87_is_empty(j, i)) break;
        int cc;
        switch (m) {
        case ZYDIS_MNEMONIC_FCMOVB: cc = 2; break;   case ZYDIS_MNEMONIC_FCMOVNB: cc = 3; break;
        case ZYDIS_MNEMONIC_FCMOVE: cc = 4; break;   case ZYDIS_MNEMONIC_FCMOVNE: cc = 5; break;
        case ZYDIS_MNEMONIC_FCMOVBE: cc = 6; break;  case ZYDIS_MNEMONIC_FCMOVNBE: cc = 7; break;
        case ZYDIS_MNEMONIC_FCMOVU: cc = 10; break;  default: cc = 11; break;
        }
        int a = fst(j, 0), b = fst(j, i);
        emit_cond_to_w0(j, cc);
        a64_cmp_imm(&j->a, 0, T0, 0);
        a64_fcsel(&j->a, 1, fset(j, 0), b, a, CC_NE);
        j->nz = NZ_NONE;
        j->x87_c1clr = 1;
        return;
    }

    /* ---- control ---- */
    case ZYDIS_MNEMONIC_FNSTSW: {
        /* the status word as the interpreter would have it: static bits and flags written first */
        x87_writeback(j); x87_flushed(j);
        a64_ldr_off(&j->a, 1, T0, R_CPU, OFF(fsw));
        st_op(j, &ops[0], T0);
        return;
    }
    case ZYDIS_MNEMONIC_FNOP: return;

    default: break;
    }
    x87_callout_end(j);
}
