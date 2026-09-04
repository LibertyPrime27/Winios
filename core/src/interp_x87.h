/* x87 FPU -- included into interp.c (it needs the operand helpers there).
 *
 * Eight 80-bit registers as a stack, a control word, a status word, and tags.
 * Arithmetic is Berkeley SoftFloat's extFloat80 with the 8086 NaN rules: the
 * same bits an x87 produces, on any host. Precision control (the 24/53/64-bit
 * rounding D3D9 games run under) and the rounding mode are honoured on every
 * operation; exception flags accumulate in the status word.
 *
 * The transcendental group (FSIN, FCOS, FPTAN, FPATAN, F2XM1, FYL2X, FYL2XP1)
 * is computed in the host's long double and rounded to extended precision.
 * Real x87 implementations differ from each other in the last bits of these,
 * so nothing can be bit-exact here anyway; the difftest compares them with a
 * tolerance.
 *
 * Not implemented (stop with XC_STOP_UNDEFINED): FBLD/FBSTP (packed BCD).
 */

#define FSW_IE 0x0001u
#define FSW_DE 0x0002u
#define FSW_ZE 0x0004u
#define FSW_OE 0x0008u
#define FSW_UE 0x0010u
#define FSW_PE 0x0020u
#define FSW_SF 0x0040u
#define FSW_ES 0x0080u
#define FSW_C0 0x0100u
#define FSW_C1 0x0200u
#define FSW_C2 0x0400u
#define FSW_C3 0x4000u
#define FSW_TOP_SHIFT 11
#define FSW_TOP_MASK  0x3800u
#define FSW_B  0x8000u

static inline int x87_top(const xc_cpu *c) { return (c->fsw >> FSW_TOP_SHIFT) & 7; }
static inline void x87_set_top(xc_cpu *c, int t) { c->fsw = (uint16_t)((c->fsw & ~FSW_TOP_MASK) | ((t & 7) << FSW_TOP_SHIFT)); }
static inline int x87_phys(const xc_cpu *c, int i) { return (x87_top(c) + i) & 7; }
static inline int x87_empty(const xc_cpu *c, int i) { return (c->ftag_empty >> x87_phys(c, i)) & 1; }

static inline extFloat80_t f80_of(xc_f80 v) { extFloat80_t r; memcpy(&r, &v, sizeof r); return r; }
static inline xc_f80 f80_to(extFloat80_t v) { xc_f80 r; memcpy(&r, &v, sizeof r); return r; }
static const xc_f80 F80_INDEFINITE = { 0xC000000000000000ull, 0xFFFF };
static const xc_f80 F80_ZERO = { 0, 0 };

static inline int f80_is_nan(xc_f80 v) { return (v.se & 0x7FFF) == 0x7FFF && (v.mant & 0x7FFFFFFFFFFFFFFFull) != 0; }
static inline int f80_is_snan(xc_f80 v) { return f80_is_nan(v) && !(v.mant & 0x4000000000000000ull); }
static inline int f80_is_inf(xc_f80 v) { return (v.se & 0x7FFF) == 0x7FFF && (v.mant & 0x7FFFFFFFFFFFFFFFull) == 0; }
static inline int f80_is_zero(xc_f80 v) { return (v.se & 0x7FFF) == 0 && v.mant == 0; }
static inline int f80_is_denorm(xc_f80 v) { return (v.se & 0x7FFF) == 0 && v.mant != 0; }
/* unnormal / pseudo-NaN / pseudo-infinity: exponent nonzero and integer bit clear */
static inline int f80_is_unsupported(xc_f80 v) { return (v.se & 0x7FFF) != 0 && !(v.mant >> 63); }

/* Set SoftFloat's mode from the control word and clear its flags. */
static void x87_env_begin(const xc_cpu *c) {
    static const uint8_t rc[4] = { softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag };
    softfloat_roundingMode = rc[(c->fcw >> 10) & 3];
    switch ((c->fcw >> 8) & 3) {
    case 0:  extF80_roundingPrecision = 32; break;
    case 2:  extF80_roundingPrecision = 64; break;
    default: extF80_roundingPrecision = 80; break;
    }
    softfloat_exceptionFlags = 0;
}
/* Merge SoftFloat's flags into the status word; `extra` adds DE/SF/etc. */
static void x87_env_end(xc_cpu *c, unsigned extra) {
    unsigned f = extra, e = softfloat_exceptionFlags;
    if (e & softfloat_flag_inexact)   f |= FSW_PE;
    if (e & softfloat_flag_underflow) f |= FSW_UE;
    if (e & softfloat_flag_overflow)  f |= FSW_OE;
    if (e & softfloat_flag_infinite)  f |= FSW_ZE;
    if (e & softfloat_flag_invalid)   f |= FSW_IE;
    c->fsw |= (uint16_t)f;
    if ((c->fsw & 0x3F) & ~(c->fcw & 0x3F)) c->fsw |= FSW_ES | FSW_B; else c->fsw &= ~(FSW_ES | FSW_B);
}

static inline xc_f80 x87_get(const xc_cpu *c, int i) { return c->fpr[x87_phys(c, i)]; }
static inline void x87_set(xc_cpu *c, int i, xc_f80 v) { int p = x87_phys(c, i); c->fpr[p] = v; c->ftag_empty &= (uint8_t)~(1u << p); }

/* Push: stack overflow (target not empty) is a stack fault -- IE, SF, C1=1,
 * and the loaded value becomes indefinite (masked-exception behaviour). */
static void x87_push(xc_cpu *c, xc_f80 v) {
    int t = (x87_top(c) - 1) & 7;
    x87_set_top(c, t);
    if (!((c->ftag_empty >> t) & 1)) { c->fsw |= FSW_IE | FSW_SF | FSW_C1; v = F80_INDEFINITE; }
    else c->fsw &= ~FSW_C1;
    c->fpr[t] = v;
    c->ftag_empty &= (uint8_t)~(1u << t);
}
static void x87_pop(xc_cpu *c) {
    int t = x87_top(c);
    c->ftag_empty |= (uint8_t)(1u << t);
    x87_set_top(c, (t + 1) & 7);
}
/* Reading an empty register is a stack underflow: IE, SF, C1=0, indefinite. */
static xc_f80 x87_read(xc_cpu *c, int i) {
    if (x87_empty(c, i)) { c->fsw |= FSW_IE | FSW_SF; c->fsw &= ~FSW_C1; return F80_INDEFINITE; }
    return x87_get(c, i);
}

static unsigned x87_denorm_flag(xc_f80 a) { return f80_is_denorm(a) ? FSW_DE : 0; }

/* --- memory operands ------------------------------------------------- */

static int x87_load_mem(ctx *x, const xop *op, int integer, xc_f80 *out, unsigned *flags) {
    uint64_t a = ea(x, op), lo, hi = 0;
    *flags = 0;
    if (op->size == 80) {
        if (!mem_read(x, a, 64, &lo) || !mem_read(x, a + 8, 16, &hi)) return 0;
        out->mant = lo; out->se = (uint16_t)hi;
        return 1;
    }
    if (!mem_read(x, a, op->size, &lo)) return 0;
    if (integer) {
        int64_t v = (int64_t)sext(lo, op->size);
        *out = f80_to(i64_to_extF80(v));
        return 1;
    }
    if (op->size == 32) {
        float32_t f = { (uint32_t)lo };
        if ((lo & 0x7F800000u) == 0 && (lo & 0x007FFFFFu)) *flags |= FSW_DE;
        *out = f80_to(f32_to_extF80(f));
    } else {
        float64_t f = { lo };
        if ((lo & 0x7FF0000000000000ull) == 0 && (lo & 0x000FFFFFFFFFFFFFull)) *flags |= FSW_DE;
        *out = f80_to(f64_to_extF80(f));
    }
    return 1;
}

static int x87_store_mem(ctx *x, const xop *op, int integer, int trunc, xc_f80 v) {
    uint64_t a = ea(x, op);
    if (op->size == 80) return mem_write(x, a, 64, v.mant) && mem_write(x, a + 8, 16, v.se);
    extFloat80_t e = f80_of(v);
    if (integer) {
        uint_fast8_t rm = trunc ? softfloat_round_minMag : softfloat_roundingMode;
        uint64_t r;
        if (op->size == 64) r = (uint64_t)extF80_to_i64(e, rm, true);
        else {
            int32_t i = extF80_to_i32(e, rm, true);
            if (op->size == 16 && (i < -32768 || i > 32767)) { softfloat_exceptionFlags |= softfloat_flag_invalid; i = (int32_t)0x8000; }
            /* an in-range 16-bit result may still carry the inexact flag from the 32-bit conversion; correct */
            r = (uint64_t)(uint32_t)i;
        }
        return mem_write(x, a, op->size, r);
    }
    if (op->size == 32) { float32_t f = extF80_to_f32(e); return mem_write(x, a, 32, f.v); }
    float64_t f = extF80_to_f64(e); return mem_write(x, a, 64, f.v);
}

/* --- compare ---------------------------------------------------------- */

/* Result as C3:C2:C0 bits: 000 greater, 001 less, 100 equal, 111 unordered.
 * `ordered` (FCOM/FCOMI) signals IE for any NaN; FUCOM only for SNaN. */
static unsigned x87_compare(xc_cpu *c, xc_f80 a, xc_f80 b, int ordered) {
    unsigned f = x87_denorm_flag(a) | x87_denorm_flag(b);
    if (f80_is_nan(a) || f80_is_nan(b) || f80_is_unsupported(a) || f80_is_unsupported(b)) {
        if (ordered || f80_is_snan(a) || f80_is_snan(b) || f80_is_unsupported(a) || f80_is_unsupported(b)) f |= FSW_IE;
        c->fsw |= (uint16_t)f;
        return FSW_C3 | FSW_C2 | FSW_C0;
    }
    extFloat80_t ea_ = f80_of(a), eb = f80_of(b);
    c->fsw |= (uint16_t)f;
    if (extF80_eq(ea_, eb)) return FSW_C3;
    if (extF80_lt_quiet(ea_, eb)) return FSW_C0;
    return 0;
}
static void x87_set_cc(xc_cpu *c, unsigned cc) { c->fsw = (uint16_t)((c->fsw & ~(FSW_C3 | FSW_C2 | FSW_C0)) | cc); }
static void x87_set_eflags(xc_cpu *c, unsigned cc) {
    c->rflags &= ~(uint64_t)XC_ARITH_FLAGS;
    if (cc & FSW_C3) c->rflags |= XC_ZF;
    if (cc & FSW_C2) c->rflags |= XC_PF;
    if (cc & FSW_C0) c->rflags |= XC_CF;
}

/* --- arithmetic ------------------------------------------------------- */

typedef enum { FA_ADD, FA_SUB, FA_SUBR, FA_MUL, FA_DIV, FA_DIVR } x87_arith;

static xc_f80 x87_binop(xc_cpu *c, x87_arith op, xc_f80 a, xc_f80 b, unsigned *flags) {
    *flags |= x87_denorm_flag(a) | x87_denorm_flag(b);
    if (f80_is_unsupported(a) || f80_is_unsupported(b)) { *flags |= FSW_IE; return F80_INDEFINITE; }
    extFloat80_t x = f80_of(a), y = f80_of(b), r;
    switch (op) {
    case FA_ADD:  r = extF80_add(x, y); break;
    case FA_SUB:  r = extF80_sub(x, y); break;
    case FA_SUBR: r = extF80_sub(y, x); break;
    case FA_MUL:  r = extF80_mul(x, y); break;
    case FA_DIV:  r = extF80_div(x, y); break;
    default:      r = extF80_div(y, x); break;
    }
    return f80_to(r);
}

/* 2^n as extended, |n| <= 16383 */
static extFloat80_t f80_pow2(int n) { extFloat80_t r; r.signif = 1ull << 63; r.signExp = (uint16_t)(0x3FFF + n); return r; }

/* FSCALE: ST0 * 2^trunc(ST1), exact except at the range ends. */
static xc_f80 x87_scale(xc_f80 a, xc_f80 b, unsigned *flags) {
    *flags |= x87_denorm_flag(a) | x87_denorm_flag(b);
    if (f80_is_nan(a) || f80_is_nan(b)) return f80_to(extF80_mul(f80_of(a), f80_of(b)));   /* NaN propagation */
    if (f80_is_inf(b)) {
        if (f80_is_zero(a) && !(b.se & 0x8000)) { *flags |= FSW_IE; return F80_INDEFINITE; }   /* 0 * 2^+inf */
        if (f80_is_inf(a) && (b.se & 0x8000)) { *flags |= FSW_IE; return F80_INDEFINITE; }     /* inf * 2^-inf */
        if (b.se & 0x8000) { xc_f80 z = F80_ZERO; z.se = a.se & 0x8000; return z; }
        xc_f80 inf = { 1ull << 63, (uint16_t)(0x7FFF | (a.se & 0x8000)) }; return inf;
    }
    int64_t n;
    if ((b.se & 0x7FFF) >= 0x3FFF + 62) n = (b.se & 0x8000) ? -40000 : 40000;   /* beyond any exponent */
    else {
        n = extF80_to_i64(f80_of(b), softfloat_round_minMag, false);
        if (n > 40000) n = 40000;
        if (n < -40000) n = -40000;
    }
    extFloat80_t r = f80_of(a);
    while (n != 0) {
        int step = n > 16383 ? 16383 : n < -16383 ? -16383 : (int)n;
        r = extF80_mul(r, f80_pow2(step));
        n -= step;
    }
    return f80_to(r);
}

/* FPREM / FPREM1. Complete when the exponent difference is below 64, in
 * which case C0,C3,C1 receive quotient bits 2,1,0; otherwise a partial
 * remainder with C2 set, and the caller loops (that is how glibc's fmod
 * works). The quotient bits come from exact 128-bit integer arithmetic. */
static xc_f80 x87_prem(xc_cpu *c, xc_f80 a, xc_f80 b, int nearest, unsigned *flags) {
    *flags |= x87_denorm_flag(a) | x87_denorm_flag(b);
    c->fsw &= ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (f80_is_nan(a) || f80_is_nan(b) || f80_is_inf(a) || f80_is_zero(b) || f80_is_unsupported(a) || f80_is_unsupported(b)) {
        if (f80_is_nan(a) || f80_is_nan(b)) return f80_to(extF80_add(f80_of(a), f80_of(b)));
        *flags |= FSW_IE; return F80_INDEFINITE;
    }
    if (f80_is_inf(b) || f80_is_zero(a)) return a;
    int ea_ = (a.se & 0x7FFF), eb = (b.se & 0x7FFF);
    /* normalise denormal exponents: value = mant * 2^(exp - 16383 - 63), exp 0 means 1 */
    if (ea_ == 0) ea_ = 1;
    if (eb == 0) eb = 1;
    int d = ea_ - eb;
    /* Align significands so both have the integer bit at 63. */
    uint64_t ma = a.mant, mb = b.mant;
    while (!(ma >> 63)) { ma <<= 1; ea_--; }
    while (!(mb >> 63)) { mb <<= 1; eb--; }
    d = ea_ - eb;
    if (d < 64) {
        /* exact quotient: floor(|a| / |b|) */
        uint64_t q;
        if (d < 0) q = 0;
        else {
            unsigned __int128 num = (unsigned __int128)ma << d;
            q = (uint64_t)(num / mb);
        }
        extFloat80_t r = extF80_rem(f80_of(a), f80_of(b));                /* IEEE (nearest) remainder, exact */
        if (!nearest) {
            /* trunc-remainder: same sign as the dividend */
            int rs = (r.signExp >> 15) & 1, as_ = (a.se >> 15) & 1;
            if (!(r.signExp == 0 && r.signif == 0) && rs != as_) {
                extFloat80_t ab = f80_of(b); ab.signExp = (uint16_t)((ab.signExp & 0x7FFF) | (as_ << 15));
                r = extF80_add(r, ab);
            }
        } else {
            /* nearest quotient may be floor + 1 */
            extFloat80_t rr = extF80_rem(f80_of(a), f80_of(b));
            int rs = (rr.signExp >> 15) & 1, as_ = (a.se >> 15) & 1;
            if (!(rr.signExp == 0 && rr.signif == 0) && rs != as_) q++;
        }
        if (q & 1) c->fsw |= FSW_C1;
        if (q & 2) c->fsw |= FSW_C3;
        if (q & 4) c->fsw |= FSW_C0;
        return f80_to(r);
    }
    /* partial: reduce by b * 2^(d-63) */
    c->fsw |= FSW_C2;
    extFloat80_t bs = f80_of(b);
    int n = d - 63;
    while (n) { int s = n > 16383 ? 16383 : n; bs = extF80_mul(bs, f80_pow2(s)); n -= s; }
    extFloat80_t r = extF80_rem(f80_of(a), bs);
    int rs = (r.signExp >> 15) & 1, as_ = (a.se >> 15) & 1;
    if (!(r.signExp == 0 && r.signif == 0) && rs != as_) {
        bs.signExp = (uint16_t)((bs.signExp & 0x7FFF) | (as_ << 15));
        r = extF80_add(r, bs);
    }
    return f80_to(r);
}

/* --- transcendentals via the host --------------------------------------- */

static long double f80_to_host(xc_f80 v) {
#if defined(__x86_64__) || defined(__i386__)
    long double r; memset(&r, 0, sizeof r); memcpy(&r, &v, 10); return r;
#else
    float64_t d = extF80_to_f64(f80_of(v)); double h; memcpy(&h, &d, 8); return (long double)h;
#endif
}
static xc_f80 x87_round_pc(xc_f80 v);
static xc_f80 f80_from_host(long double v) {
    xc_f80 r;
#if defined(__x86_64__) || defined(__i386__)
    memset(&r, 0, sizeof r); memcpy(&r, &v, 10);
#else
    double h = (double)v; float64_t d; memcpy(&d, &h, 8); r = f80_to(f64_to_extF80(d));
#endif
    return x87_round_pc(r);
}
/* Round a host result to the current precision control (the host computed at
 * its own precision; an x87 rounds the final result). Multiplying by one
 * runs the value through SoftFloat's rounding path at the current PC. */
static xc_f80 x87_round_pc(xc_f80 v) {
    if (extF80_roundingPrecision == 80) return v;
    return f80_to(extF80_mul(f80_of(v), f80_pow2(0)));
}

/* --- FXAM ----------------------------------------------------------------- */

static void x87_fxam(xc_cpu *c) {
    unsigned cc;
    c->fsw &= ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (x87_empty(c, 0)) { c->fsw |= FSW_C3 | FSW_C0; return; }
    xc_f80 v = x87_get(c, 0);
    if (v.se & 0x8000) c->fsw |= FSW_C1;
    if (f80_is_unsupported(v)) cc = 0;
    else if (f80_is_nan(v)) cc = FSW_C0;
    else if (f80_is_inf(v)) cc = FSW_C2 | FSW_C0;
    else if (f80_is_zero(v)) cc = FSW_C3;
    else if (f80_is_denorm(v)) cc = FSW_C3 | FSW_C2;
    else cc = FSW_C2;
    c->fsw |= (uint16_t)cc;
}

/* --- environment / save areas --------------------------------------------- */

static uint16_t x87_full_tag(const xc_cpu *c) {
    uint16_t t = 0;
    for (int p = 0; p < 8; p++) {
        unsigned tag;
        if ((c->ftag_empty >> p) & 1) tag = 3;
        else { xc_f80 v = c->fpr[p]; tag = f80_is_zero(v) ? 1 : (f80_is_nan(v) || f80_is_inf(v) || f80_is_denorm(v) || f80_is_unsupported(v)) ? 2 : 0; }
        t |= (uint16_t)(tag << (2 * p));
    }
    return t;
}
static void x87_set_full_tag(xc_cpu *c, uint16_t t) {
    c->ftag_empty = 0;
    for (int p = 0; p < 8; p++) if (((t >> (2 * p)) & 3) == 3) c->ftag_empty |= (uint8_t)(1u << p);
}
static void x87_init(xc_cpu *c) {
    c->fcw = 0x037F; c->fsw = 0; c->ftag_empty = 0xFF;
}

/* 32-bit protected-mode layout (what 64-bit code gets too): 7 dwords. */
static int x87_store_env(ctx *x, uint64_t a) {
    xc_cpu *c = x->c;
    return mem_write(x, a, 32, c->fcw | 0xFFFF0000u) && mem_write(x, a + 4, 32, c->fsw | 0xFFFF0000u)
        && mem_write(x, a + 8, 32, x87_full_tag(c) | 0xFFFF0000u) && mem_write(x, a + 12, 32, 0)
        && mem_write(x, a + 16, 32, 0) && mem_write(x, a + 20, 32, 0) && mem_write(x, a + 24, 32, 0xFFFF0000u);   /* FDS: selector 0, upper half set, as hardware does */
}
static int x87_load_env(ctx *x, uint64_t a) {
    xc_cpu *c = x->c; uint64_t v;
    if (!mem_read(x, a, 16, &v)) return 0;
    c->fcw = (uint16_t)v;
    if (!mem_read(x, a + 4, 16, &v)) return 0;
    c->fsw = (uint16_t)v;
    if (!mem_read(x, a + 8, 16, &v)) return 0;
    x87_set_full_tag(c, (uint16_t)v);
    return 1;
}
static int x87_fxsave(ctx *x, uint64_t a) {
    xc_cpu *c = x->c;
    if (!mem_write(x, a, 16, c->fcw) || !mem_write(x, a + 2, 16, c->fsw) || !mem_write(x, a + 4, 8, (uint8_t)~c->ftag_empty)
        || !mem_write(x, a + 5, 8, 0) || !mem_write(x, a + 6, 16, 0) || !mem_write(x, a + 8, 64, 0) || !mem_write(x, a + 16, 64, 0)
        || !mem_write(x, a + 24, 32, c->mxcsr) || !mem_write(x, a + 28, 32, 0xFFFF)) return 0;
    for (int i = 0; i < 8; i++) {
        xc_f80 v = x87_get(c, i);
        if (!mem_write(x, a + 32 + 16 * i, 64, v.mant) || !mem_write(x, a + 40 + 16 * i, 16, v.se) || !mem_write(x, a + 42 + 16 * i, 48, 0)) return 0;
    }
    for (int i = 0; i < 16; i++)
        if (!mem_write(x, a + 160 + 16 * i, 64, c->xmm[i].lo) || !mem_write(x, a + 168 + 16 * i, 64, c->xmm[i].hi)) return 0;
    return 1;
}
static int x87_fxrstor(ctx *x, uint64_t a) {
    xc_cpu *c = x->c; uint64_t v, w;
    if (!mem_read(x, a, 16, &v)) return 0;
    c->fcw = (uint16_t)v;
    if (!mem_read(x, a + 2, 16, &v)) return 0;
    c->fsw = (uint16_t)v;
    if (!mem_read(x, a + 4, 8, &v)) return 0;
    c->ftag_empty = (uint8_t)~v;
    if (!mem_read(x, a + 24, 32, &v)) return 0;
    c->mxcsr = (uint32_t)v & 0xFFFF;
    for (int i = 0; i < 8; i++) {
        if (!mem_read(x, a + 32 + 16 * i, 64, &v) || !mem_read(x, a + 40 + 16 * i, 16, &w)) return 0;
        xc_f80 f = { v, (uint16_t)w }; c->fpr[x87_phys(c, i)] = f;
    }
    for (int i = 0; i < 16; i++)
        if (!mem_read(x, a + 160 + 16 * i, 64, &c->xmm[i].lo) || !mem_read(x, a + 168 + 16 * i, 64, &c->xmm[i].hi)) return 0;
    return 1;
}

/* --- dispatch ------------------------------------------------------------- */

static inline int st_index(const xop *op) { return op->ridx; }
static inline int is_st(const xop *op) { return op->type == XOP_REG && op->rcls == XR_X87; }
static inline int is_mem(const xop *op) { return op->type == XOP_MEM; }

/* Returns 1 if handled (ok in *ok), 0 if not an x87 instruction. */
static int do_x87(ctx *x, ZydisMnemonic m, int *ok) {
    xc_cpu *c = x->c;
    const xop *ops = x->ops;
    int nvis = x->in->operand_count_visible;
    unsigned fl = 0;
    xc_f80 a, b, r;
    *ok = 1;
    x87_env_begin(c);
#define FAIL() do { *ok = 0; return 1; } while (0)
#define DONE() do { x87_env_end(c, fl); return 1; } while (0)

    switch (m) {
    /* ---- loads ---- */
    case ZYDIS_MNEMONIC_FLD:
        if (is_mem(&ops[0])) { if (!x87_load_mem(x, &ops[0], 0, &a, &fl)) FAIL(); }
        else a = x87_read(c, st_index(&ops[0]));
        x87_push(c, a); DONE();
    case ZYDIS_MNEMONIC_FILD:
        if (!x87_load_mem(x, &ops[0], 1, &a, &fl)) FAIL();
        x87_push(c, a); DONE();
    case ZYDIS_MNEMONIC_FLD1:   { xc_f80 v = { 1ull << 63, 0x3FFF }; x87_push(c, v); DONE(); }
    case ZYDIS_MNEMONIC_FLDZ:   x87_push(c, F80_ZERO); DONE();
    case ZYDIS_MNEMONIC_FLDPI:  { xc_f80 v = { 0xC90FDAA22168C235ull, 0x4000 }; x87_push(c, v); DONE(); }
    case ZYDIS_MNEMONIC_FLDL2E: { xc_f80 v = { 0xB8AA3B295C17F0BCull, 0x3FFF }; x87_push(c, v); DONE(); }
    case ZYDIS_MNEMONIC_FLDLN2: { xc_f80 v = { 0xB17217F7D1CF79ACull, 0x3FFE }; x87_push(c, v); DONE(); }
    case ZYDIS_MNEMONIC_FLDLG2: { xc_f80 v = { 0x9A209A84FBCFF799ull, 0x3FFD }; x87_push(c, v); DONE(); }
    case ZYDIS_MNEMONIC_FLDL2T: { xc_f80 v = { 0xD49A784BCD1B8AFEull, 0x4000 }; x87_push(c, v); DONE(); }

    /* ---- stores ---- */
    case ZYDIS_MNEMONIC_FST: case ZYDIS_MNEMONIC_FSTP:
        a = x87_read(c, 0);
        if (is_mem(&ops[0])) { if (!x87_store_mem(x, &ops[0], 0, 0, a)) FAIL(); }
        else x87_set(c, st_index(&ops[0]), a);
        if (m == ZYDIS_MNEMONIC_FSTP) x87_pop(c);
        DONE();
    case ZYDIS_MNEMONIC_FIST: case ZYDIS_MNEMONIC_FISTP: case ZYDIS_MNEMONIC_FISTTP:
        a = x87_read(c, 0);
        if (!x87_store_mem(x, &ops[0], 1, m == ZYDIS_MNEMONIC_FISTTP, a)) FAIL();
        if (m != ZYDIS_MNEMONIC_FIST) x87_pop(c);
        DONE();

    /* ---- stack ---- */
    case ZYDIS_MNEMONIC_FXCH: {
        int i = 1;
        for (int k = 0; k < nvis; k++) if (is_st(&ops[k]) && st_index(&ops[k]) != 0) i = st_index(&ops[k]);
        a = x87_read(c, 0); b = x87_read(c, i);
        x87_set(c, 0, b); x87_set(c, i, a);
        c->fsw &= ~FSW_C1;
        DONE();
    }
    case ZYDIS_MNEMONIC_FINCSTP: x87_set_top(c, x87_top(c) + 1); c->fsw &= ~FSW_C1; DONE();
    case ZYDIS_MNEMONIC_FDECSTP: x87_set_top(c, x87_top(c) - 1); c->fsw &= ~FSW_C1; DONE();
    case ZYDIS_MNEMONIC_FFREE:  c->ftag_empty |= (uint8_t)(1u << x87_phys(c, st_index(&ops[0]))); DONE();
    case ZYDIS_MNEMONIC_FFREEP: c->ftag_empty |= (uint8_t)(1u << x87_phys(c, st_index(&ops[0]))); x87_pop(c); DONE();

    /* ---- arithmetic ---- */
    case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD:
    case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP: case ZYDIS_MNEMONIC_FISUB:
    case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP: case ZYDIS_MNEMONIC_FISUBR:
    case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL:
    case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV:
    case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR: {
        x87_arith op;
        int pop = 0, integer = 0;
        switch (m) {
        case ZYDIS_MNEMONIC_FADDP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIADD: integer = m == ZYDIS_MNEMONIC_FIADD; /* fallthrough */
        case ZYDIS_MNEMONIC_FADD: op = FA_ADD; break;
        case ZYDIS_MNEMONIC_FSUBP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FISUB: integer = m == ZYDIS_MNEMONIC_FISUB; /* fallthrough */
        case ZYDIS_MNEMONIC_FSUB: op = FA_SUB; break;
        case ZYDIS_MNEMONIC_FSUBRP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FISUBR: integer = m == ZYDIS_MNEMONIC_FISUBR; /* fallthrough */
        case ZYDIS_MNEMONIC_FSUBR: op = FA_SUBR; break;
        case ZYDIS_MNEMONIC_FMULP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIMUL: integer = m == ZYDIS_MNEMONIC_FIMUL; /* fallthrough */
        case ZYDIS_MNEMONIC_FMUL: op = FA_MUL; break;
        case ZYDIS_MNEMONIC_FDIVP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIDIV: integer = m == ZYDIS_MNEMONIC_FIDIV; /* fallthrough */
        case ZYDIS_MNEMONIC_FDIV: op = FA_DIV; break;
        case ZYDIS_MNEMONIC_FDIVRP: pop = 1; /* fallthrough */
        case ZYDIS_MNEMONIC_FIDIVR: integer = m == ZYDIS_MNEMONIC_FIDIVR; /* fallthrough */
        default: op = FA_DIVR; break;
        }
        int dst = 0;
        if (nvis == 0) { dst = 1; a = x87_read(c, 1); b = x87_read(c, 0); }
        else if (is_mem(&ops[0])) {                /* ST0 op= mem */
            if (!x87_load_mem(x, &ops[0], integer, &b, &fl)) FAIL();
            a = x87_read(c, 0);
        } else {                                   /* ST(i) op= ST(j) */
            dst = st_index(&ops[0]);
            a = x87_read(c, dst); b = x87_read(c, st_index(&ops[1]));
        }
        r = x87_binop(c, op, a, b, &fl);
        x87_set(c, dst, r);
        if (pop) x87_pop(c);
        DONE();
    }
    case ZYDIS_MNEMONIC_FCHS: a = x87_read(c, 0); a.se ^= 0x8000; x87_set(c, 0, a); c->fsw &= ~FSW_C1; DONE();
    case ZYDIS_MNEMONIC_FABS: a = x87_read(c, 0); a.se &= 0x7FFF; x87_set(c, 0, a); c->fsw &= ~FSW_C1; DONE();
    case ZYDIS_MNEMONIC_FSQRT:
        a = x87_read(c, 0); fl |= x87_denorm_flag(a);
        x87_set(c, 0, f80_to(extF80_sqrt(f80_of(a)))); DONE();
    case ZYDIS_MNEMONIC_FRNDINT:
        a = x87_read(c, 0); fl |= x87_denorm_flag(a);
        x87_set(c, 0, f80_to(extF80_roundToInt(f80_of(a), softfloat_roundingMode, true))); DONE();
    case ZYDIS_MNEMONIC_FSCALE:
        a = x87_read(c, 0); b = x87_read(c, 1);
        x87_set(c, 0, x87_scale(a, b, &fl)); DONE();
    case ZYDIS_MNEMONIC_FPREM: case ZYDIS_MNEMONIC_FPREM1:
        a = x87_read(c, 0); b = x87_read(c, 1);
        x87_set(c, 0, x87_prem(c, a, b, m == ZYDIS_MNEMONIC_FPREM1, &fl)); DONE();
    case ZYDIS_MNEMONIC_FXTRACT: {
        a = x87_read(c, 0); fl |= x87_denorm_flag(a);
        if (f80_is_zero(a)) { xc_f80 ninf = { 1ull << 63, 0xFFFF }; fl |= FSW_ZE; x87_set(c, 0, ninf); x87_push(c, a); DONE(); }
        if (f80_is_nan(a) || f80_is_inf(a)) { x87_set(c, 0, f80_is_inf(a) ? (xc_f80){ 1ull << 63, 0x7FFF } : a); x87_push(c, a); DONE(); }
        int e = (a.se & 0x7FFF); uint64_t mant = a.mant;
        if (e == 0) { e = 1; while (!(mant >> 63)) { mant <<= 1; e--; } }
        xc_f80 sig = { mant, (uint16_t)(0x3FFF | (a.se & 0x8000)) };
        x87_set(c, 0, f80_to(i64_to_extF80(e - 0x3FFF)));
        x87_push(c, sig); DONE();
    }

    /* ---- transcendentals ---- */
    case ZYDIS_MNEMONIC_FSIN: case ZYDIS_MNEMONIC_FCOS: case ZYDIS_MNEMONIC_FSINCOS: case ZYDIS_MNEMONIC_FPTAN: {
        a = x87_read(c, 0); fl |= x87_denorm_flag(a);
        c->fsw &= ~FSW_C2;
        if (f80_is_inf(a)) { fl |= FSW_IE; x87_set(c, 0, F80_INDEFINITE); DONE(); }
        if (f80_is_nan(a)) { x87_set(c, 0, f80_to(extF80_add(f80_of(a), f80_of(a)))); DONE(); }
        long double v = f80_to_host(a);
        if (fabsl(v) >= 9223372036854775808.0L) { c->fsw |= FSW_C2; DONE(); }     /* out of range: unchanged, C2=1 */
        if (m == ZYDIS_MNEMONIC_FSIN) x87_set(c, 0, f80_from_host(sinl(v)));
        else if (m == ZYDIS_MNEMONIC_FCOS) x87_set(c, 0, f80_from_host(cosl(v)));
        else if (m == ZYDIS_MNEMONIC_FSINCOS) { x87_set(c, 0, f80_from_host(sinl(v))); x87_push(c, f80_from_host(cosl(v))); }
        else { x87_set(c, 0, f80_from_host(tanl(v))); xc_f80 one = { 1ull << 63, 0x3FFF }; x87_push(c, one); }
        if (!f80_is_zero(a)) fl |= FSW_PE;
        DONE();
    }
    case ZYDIS_MNEMONIC_FPATAN: {                  /* ST1 = atan2(ST1, ST0); pop */
        a = x87_read(c, 1); b = x87_read(c, 0);
        fl |= x87_denorm_flag(a) | x87_denorm_flag(b);
        if (f80_is_nan(a) || f80_is_nan(b)) x87_set(c, 1, f80_to(extF80_add(f80_of(a), f80_of(b))));
        else { x87_set(c, 1, f80_from_host(atan2l(f80_to_host(a), f80_to_host(b)))); fl |= FSW_PE; }
        x87_pop(c); DONE();
    }
    case ZYDIS_MNEMONIC_F2XM1: {
        a = x87_read(c, 0); fl |= x87_denorm_flag(a);
        if (f80_is_nan(a)) { x87_set(c, 0, f80_to(extF80_add(f80_of(a), f80_of(a)))); DONE(); }
        long double v = f80_to_host(a);
        x87_set(c, 0, f80_from_host(exp2l(v) - 1.0L)); if (!f80_is_zero(a)) fl |= FSW_PE; DONE();
    }
    case ZYDIS_MNEMONIC_FYL2X: case ZYDIS_MNEMONIC_FYL2XP1: {      /* ST1 = ST1 * log2(ST0 [+1]); pop */
        a = x87_read(c, 1); b = x87_read(c, 0);
        fl |= x87_denorm_flag(a) | x87_denorm_flag(b);
        if (f80_is_nan(a) || f80_is_nan(b)) { x87_set(c, 1, f80_to(extF80_add(f80_of(a), f80_of(b)))); x87_pop(c); DONE(); }
        long double xv = f80_to_host(b), yv = f80_to_host(a);
        long double l = m == ZYDIS_MNEMONIC_FYL2X ? log2l(xv) : log2l(1.0L + xv);
        if (m == ZYDIS_MNEMONIC_FYL2X && xv == 0.0L) { fl |= FSW_ZE; }
        else if (m == ZYDIS_MNEMONIC_FYL2X ? xv < 0.0L : xv < -1.0L) { fl |= FSW_IE; x87_set(c, 1, F80_INDEFINITE); x87_pop(c); DONE(); }
        else fl |= FSW_PE;
        x87_set(c, 1, f80_from_host(yv * l)); x87_pop(c); DONE();
    }

    /* ---- compares ---- */
    case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP:
    case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP: {
        int ordered = m == ZYDIS_MNEMONIC_FCOM || m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
        int integer = m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
        a = x87_read(c, 0);
        const xop *src = nvis >= 1 ? &ops[nvis - 1] : 0;
        if (src && is_mem(src)) { if (!x87_load_mem(x, src, integer, &b, &fl)) FAIL(); }
        else b = x87_read(c, src && is_st(src) ? st_index(src) : 1);
        x87_set_cc(c, x87_compare(c, a, b, ordered));
        c->fsw &= ~FSW_C1;
        if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP || m == ZYDIS_MNEMONIC_FICOMP) x87_pop(c);
        DONE();
    }
    case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOMPP:
        a = x87_read(c, 0); b = x87_read(c, 1);
        x87_set_cc(c, x87_compare(c, a, b, m == ZYDIS_MNEMONIC_FCOMPP));
        c->fsw &= ~FSW_C1;
        x87_pop(c); x87_pop(c); DONE();
    case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
        a = x87_read(c, 0); b = x87_read(c, st_index(&ops[nvis - 1]));
        x87_set_eflags(c, x87_compare(c, a, b, m == ZYDIS_MNEMONIC_FCOMI || m == ZYDIS_MNEMONIC_FCOMIP));
        c->fsw &= ~FSW_C1;
        if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) x87_pop(c);
        DONE();
    }
    case ZYDIS_MNEMONIC_FTST:
        a = x87_read(c, 0);
        x87_set_cc(c, x87_compare(c, a, F80_ZERO, 1));
        c->fsw &= ~FSW_C1; DONE();
    case ZYDIS_MNEMONIC_FXAM: x87_fxam(c); DONE();

    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVU:
    case ZYDIS_MNEMONIC_FCMOVNB: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNU: {
        int cf = get_flag(c, XC_CF), zf = get_flag(c, XC_ZF), pf = get_flag(c, XC_PF), take;
        switch (m) {
        case ZYDIS_MNEMONIC_FCMOVB:   take = cf; break;
        case ZYDIS_MNEMONIC_FCMOVE:   take = zf; break;
        case ZYDIS_MNEMONIC_FCMOVBE:  take = cf || zf; break;
        case ZYDIS_MNEMONIC_FCMOVU:   take = pf; break;
        case ZYDIS_MNEMONIC_FCMOVNB:  take = !cf; break;
        case ZYDIS_MNEMONIC_FCMOVNE:  take = !zf; break;
        case ZYDIS_MNEMONIC_FCMOVNBE: take = !(cf || zf); break;
        default:                      take = !pf; break;
        }
        if (take) { b = x87_read(c, st_index(&ops[nvis - 1])); x87_set(c, 0, b); }
        c->fsw &= ~FSW_C1;
        DONE();
    }

    /* ---- control ---- */
    case ZYDIS_MNEMONIC_FNSTCW: if (!op_write(x, 0, c->fcw)) FAIL(); DONE();
    case ZYDIS_MNEMONIC_FLDCW: { uint64_t v; if (!op_read(x, 0, &v)) FAIL(); c->fcw = (uint16_t)((v & 0x1F7F) | 0x40); fl = 0; DONE(); }
    case ZYDIS_MNEMONIC_FNSTSW: if (!op_write(x, 0, c->fsw)) FAIL(); DONE();
    case ZYDIS_MNEMONIC_FNCLEX: c->fsw &= ~(FSW_IE | FSW_DE | FSW_ZE | FSW_OE | FSW_UE | FSW_PE | FSW_SF | FSW_ES | FSW_B); DONE();
    case ZYDIS_MNEMONIC_FNINIT: x87_init(c); DONE();
    case ZYDIS_MNEMONIC_FNOP: DONE();
    case ZYDIS_MNEMONIC_FNSTENV: if (!x87_store_env(x, ea(x, &ops[0]))) FAIL(); c->fcw |= 0x3F; DONE();   /* masks all exceptions afterwards */
    case ZYDIS_MNEMONIC_FLDENV: if (!x87_load_env(x, ea(x, &ops[0]))) FAIL(); DONE();
    case ZYDIS_MNEMONIC_FNSAVE: {
        uint64_t base = ea(x, &ops[0]);
        if (!x87_store_env(x, base)) FAIL();
        for (int i = 0; i < 8; i++) { xc_f80 v = x87_get(c, i); if (!mem_write(x, base + 28 + 10 * i, 64, v.mant) || !mem_write(x, base + 36 + 10 * i, 16, v.se)) FAIL(); }
        x87_init(c); DONE();
    }
    case ZYDIS_MNEMONIC_FRSTOR: {
        uint64_t base = ea(x, &ops[0]), v, w;
        if (!x87_load_env(x, base)) FAIL();
        for (int i = 0; i < 8; i++) { if (!mem_read(x, base + 28 + 10 * i, 64, &v) || !mem_read(x, base + 36 + 10 * i, 16, &w)) FAIL(); xc_f80 f = { v, (uint16_t)w }; c->fpr[x87_phys(c, i)] = f; }
        DONE();
    }
    case ZYDIS_MNEMONIC_FXSAVE: case ZYDIS_MNEMONIC_FXSAVE64: if (!x87_fxsave(x, ea(x, &ops[0]))) FAIL(); DONE();
    case ZYDIS_MNEMONIC_FXRSTOR: case ZYDIS_MNEMONIC_FXRSTOR64: if (!x87_fxrstor(x, ea(x, &ops[0]))) FAIL(); DONE();

    default:
        return 0;
    }
#undef FAIL
#undef DONE
}
