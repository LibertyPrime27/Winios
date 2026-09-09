/* A shader interpreter for Direct3D 10 and 11.
 *
 * A D3D11 program hands over compiled shaders as DXBC containers; the code
 * chunk inside is a stream of tokens for a GPU that is not here. Until now
 * the pipeline drew with a fixed-function reading of the shader's signature
 * -- right for "this sprite, at this place, tinted", wrong for anything the
 * shader itself decides, and every D3D11 engine decides its vertex transform
 * in the shader: GameMaker multiplies by a matrix it keeps in a constant
 * buffer, which no reading of the signature can see. So the code runs.
 *
 * This is the whole tokenised format as a compiler emits it for vs_4_0 and
 * ps_4_0 (and the arithmetic of 5_0): the operand encoding with its masks,
 * swizzles, modifiers and relative indexing; temporaries, indexable arrays,
 * constant buffers, immediate constants; if/else/loop/break/continue/ret;
 * float, integer and unsigned arithmetic and comparison; sampling, texel
 * loads and resource queries. Geometry, hull, domain and compute stages, the
 * texture-array and comparison sampling forms, calls and switches are not
 * here and a program that uses them is refused by name, so the pipeline can
 * fall back to the fixed-function reading and the report can say why.
 *
 * Interpreted, on the CPU, per vertex and per pixel. That is slow, and the
 * whole rasterizer is the ceiling here until the GPU draws (see
 * docs/WIN32.md); what this buys is a *correct* frame from a shader-driven
 * 2D engine, which is what "does it run" means for those games. The float
 * arithmetic is compiled with contraction off, so a*b+c is two roundings on
 * every machine and the frame checksums keep meaning something.
 */
#include "dxbc_exec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_TEMP = 0, OP_INPUT = 1, OP_OUTPUT = 2, OP_XTEMP = 3, OP_IMM32 = 4, OP_IMM64 = 5, OP_SAMPLER = 6, OP_RESOURCE = 7,
       OP_CB = 8, OP_ICB = 9, OP_LABEL = 10, OP_PRIMID = 11, OP_ODEPTH = 12, OP_NULL = 13, OP_RASTERIZER = 14, OP_OMASK = 15 };
enum { MAX_INSN = 4096, MAX_OPS = 6, MAX_XTEMP = 4, XTEMP_MAX = 256, ICB_MAX = 4096 };

typedef struct {
    uint8_t  type, ncomp, mode;      /* mode: 0 mask, 1 swizzle, 2 select1 */
    uint8_t  mask, swz[4];
    uint8_t  neg, abs_;
    uint8_t  ndim, rel_dim;          /* which index (if any) has a relative operand */
    int32_t  idx[3];
    uint32_t imm[4];
    uint8_t  rel_type, rel_comp; int32_t rel_idx;   /* the relative operand: a temp component */
} operand;
typedef struct {
    uint16_t op; uint8_t sat, test_nz, nop; uint8_t ret_type;
    operand ops[MAX_OPS];
    int32_t target;                  /* if: the else/endif; else: the endif; loop/endloop: each other; break/continue: the loop */
} insn;
struct dxbc_prog {
    int stage;
    insn *code; int n;
    float *icb; uint32_t icb_n;
    uint32_t xtemp_size[MAX_XTEMP];
};
typedef union { float f; uint32_t u; int32_t i; } reg;

/* ---- decoding ------------------------------------------------------------------ */
static int decode_operand(const uint32_t *t, uint32_t n, uint32_t *pos, operand *o, int allow_rel) {
    memset(o, 0, sizeof *o);
    if (*pos >= n) return -1;
    uint32_t tok = t[(*pos)++];
    uint32_t nc = tok & 3;
    o->ncomp = nc == 0 ? 0 : nc == 1 ? 1 : 4;
    o->type = (tok >> 12) & 0xFF;
    o->ndim = (tok >> 20) & 3;
    o->mode = 0; o->mask = 0xF; o->swz[0] = 0; o->swz[1] = 1; o->swz[2] = 2; o->swz[3] = 3;
    if (o->ncomp == 4) {
        o->mode = (tok >> 2) & 3;
        if (o->mode == 0) o->mask = (tok >> 4) & 0xF;
        else if (o->mode == 1) for (int i = 0; i < 4; i++) o->swz[i] = (tok >> (4 + 2 * i)) & 3;
        else { uint8_t s = (tok >> 4) & 3; for (int i = 0; i < 4; i++) o->swz[i] = s; o->mask = 1; }
    } else if (o->ncomp == 1) { o->mask = 1; for (int i = 0; i < 4; i++) o->swz[i] = 0; }
    uint32_t ext = tok >> 31;
    while (ext) {
        if (*pos >= n) return -1;
        uint32_t e = t[(*pos)++];
        if ((e & 0x3F) == 1) { uint32_t m = (e >> 6) & 0xFF; o->neg = m == 1 || m == 3; o->abs_ = m == 2 || m == 3; }
        ext = e >> 31;
    }
    if (o->type == OP_IMM32) {
        for (int i = 0; i < (o->ncomp == 4 ? 4 : 1); i++) { if (*pos >= n) return -1; o->imm[i] = t[(*pos)++]; }
        if (o->ncomp == 1) o->imm[1] = o->imm[2] = o->imm[3] = o->imm[0];
        return 0;
    }
    if (o->type == OP_IMM64) return -2;                /* doubles: not here */
    for (int d = 0; d < o->ndim; d++) {
        uint32_t rep = (tok >> (22 + 3 * d)) & 7;
        if (rep == 0) { if (*pos >= n) return -1; o->idx[d] = (int32_t)t[(*pos)++]; }
        else if (rep == 1) { if (*pos + 1 >= n) return -1; o->idx[d] = (int32_t)t[*pos]; *pos += 2; }
        else if (rep == 2 || rep == 3) {
            if (rep == 3) { if (*pos >= n) return -1; o->idx[d] = (int32_t)t[(*pos)++]; }
            if (!allow_rel) return -2;
            operand r; if (decode_operand(t, n, pos, &r, 0)) return -1;
            if (r.type != OP_TEMP && r.type != OP_INPUT && r.type != OP_XTEMP) return -2;
            o->rel_dim = (uint8_t)(d + 1); o->rel_type = r.type; o->rel_idx = r.idx[0]; o->rel_comp = r.swz[0];
            if (r.type == OP_XTEMP) return -2;
        } else return -2;
    }
    return 0;
}
static int is_decl(uint32_t op) { return (op >= 88 && op <= 106) || (op >= 107 && op <= 120) || (op >= 155 && op <= 168) || op == 206; }

dxbc_prog *dxbc_prog_new(const uint32_t *code, uint32_t words, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
#define WHY(...) do { if (why && why_len) snprintf(why, why_len, __VA_ARGS__); dxbc_prog_free(p); return 0; } while (0)
    if (!code || words < 2) return 0;
    dxbc_prog *p = calloc(1, sizeof *p); if (!p) return 0;
    uint32_t ver = code[0], type = ver >> 16, major = (ver >> 4) & 0xF;
    p->stage = (int)type;
    if (type > 1) WHY("a %s shader (only vertex and pixel shaders run)", type == 2 ? "geometry" : type == 3 ? "hull" : type == 4 ? "domain" : type == 5 ? "compute" : "unknown-stage");
    if (major < 4) WHY("shader model %u code (this is the model 4/5 format)", major);
    uint32_t len = code[1]; if (len > words) len = words;
    p->code = calloc(MAX_INSN, sizeof *p->code); if (!p->code) WHY("out of memory");
    uint32_t pos = 2;
    int stack[64], sp = 0;                  /* open if/loop blocks, by instruction index */
    while (pos < len) {
        uint32_t tok = code[pos];
        uint32_t op = tok & 0x7FF, ilen = (tok >> 24) & 0x7F;
        if (op == 53) {                      /* customdata: its own length */
            if (pos + 1 >= len) WHY("truncated customdata");
            uint32_t clen = code[pos + 1], cls = tok >> 11;
            if (clen < 2 || pos + clen > len) WHY("customdata overruns the code");
            if (cls == 3) {                  /* immediate constant buffer */
                uint32_t nf = clen - 2; if (nf / 4 > ICB_MAX) nf = ICB_MAX * 4;
                p->icb = malloc(nf * sizeof(float)); if (!p->icb) WHY("out of memory");
                memcpy(p->icb, code + pos + 2, nf * sizeof(float)); p->icb_n = nf / 4;
            }
            pos += clen; continue;
        }
        if (!ilen || pos + ilen > len) WHY("an instruction with a bad length");
        if (is_decl(op)) {
            if (op == 105 && ilen >= 4) {    /* dcl_indexableTemp x#[n], comps */
                uint32_t xi = code[pos + 1], xn = code[pos + 2];
                if (xi < MAX_XTEMP) p->xtemp_size[xi] = xn > XTEMP_MAX ? XTEMP_MAX : xn;
            }
            pos += ilen; continue;
        }
        if (p->n >= MAX_INSN) WHY("more than %d instructions", MAX_INSN);
        insn *in = &p->code[p->n];
        memset(in, 0, sizeof *in);
        in->op = (uint16_t)op; in->sat = (tok >> 13) & 1; in->test_nz = (tok >> 18) & 1; in->ret_type = (tok >> 11) & 3; in->target = -1;
        uint32_t q = pos + 1, end = pos + ilen, ext = tok >> 31;
        while (ext && q < end) { uint32_t e = code[q++]; ext = e >> 31; }   /* sample controls, resource dims: skipped */
        while (q < end && in->nop < MAX_OPS) {
            int r = decode_operand(code, end, &q, &in->ops[in->nop], 1);
            if (r == -2) WHY("an operand form not decoded here (opcode %u)", op);
            if (r) WHY("a malformed operand (opcode %u)", op);
            in->nop++;
        }
        switch (op) {
        case 0: case 1: case 14: case 15: case 16: case 17: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 32: case 33: case 34:
        case 36: case 37: case 39: case 40: case 41: case 42: case 43: case 47: case 49: case 51: case 52: case 54: case 56: case 57: case 59: case 60:
        case 64: case 65: case 66: case 67: case 68: case 75: case 79: case 80: case 83: case 84: case 85: case 86: case 87:
        case 35: case 50: case 55: case 82: case 38: case 81: case 78: case 77: case 69: case 72: case 73: case 74: case 45: case 61: case 11: case 12:
        case 108: case 13: case 62: case 63: case 58: break;
        case 31: case 48: if (sp < 64) stack[sp++] = p->n; break;                                 /* if, loop */
        case 18: { if (!sp) WHY("else without if"); int i = stack[sp - 1]; p->code[i].target = p->n; stack[sp - 1] = p->n; break; }   /* else */
        case 21: { if (!sp) WHY("endif without if"); int i = stack[--sp]; p->code[i].target = p->n; break; }
        case 22: { if (!sp) WHY("endloop without loop"); int i = stack[--sp]; p->code[i].target = p->n; in->target = i; break; }
        case 2: case 3: case 7: case 8: {                                                       /* break(c), continue(c): the innermost loop */
            int found = -1; for (int k = sp - 1; k >= 0; k--) if (p->code[stack[k]].op == 48) { found = stack[k]; break; }
            if (found < 0) WHY("break outside a loop");
            in->target = found; break;
        }
        case 4: case 5: case 44: WHY("subroutine calls");
        case 76: case 6: case 10: case 23: WHY("switch statements");
        case 70: case 71: WHY("comparison sampling");
        case 109: WHY("gather4");
        case 46: WHY("multisample loads");
        default: WHY("opcode %u", op);
        }
        p->n++; pos += ilen;
    }
    if (sp) WHY("an unterminated block");
    /* break/continue targets: resolved now that every loop has its end */
    for (int i = 0; i < p->n; i++) if (p->code[i].op == 2 || p->code[i].op == 3 || p->code[i].op == 7 || p->code[i].op == 8) {
        int loop = p->code[i].target;
        p->code[i].target = (p->code[i].op == 7 || p->code[i].op == 8) ? loop : p->code[loop].target;
    }
    return p;
#undef WHY
}
void dxbc_prog_free(dxbc_prog *p) { if (!p) return; free(p->code); free(p->icb); free(p); }
int dxbc_prog_stage(const dxbc_prog *p) { return p ? p->stage : -1; }

/* ---- sampling ---------------------------------------------------------------------- */
static uint32_t texel(const d3d11_texture *t, int64_t x, int64_t y, int wrap) {
    if (wrap) { x %= t->w; if (x < 0) x += t->w; y %= t->h; if (y < 0) y += t->h; }
    else { if (x < 0) x = 0; if (y < 0) y = 0; if (x >= t->w) x = t->w - 1; if (y >= t->h) y = t->h - 1; }
    return t->pixels[(size_t)y * (size_t)t->pitch_px + (size_t)x];
}
void dxbc_sample(const d3d11_texture *t, int wrap, int linear, float u, float v, float out[4]) {
    if (!t || !t->pixels || t->w <= 0 || t->h <= 0) { out[0] = out[1] = out[2] = out[3] = 1.0f; return; }
    /* 24.8 texel coordinates, floored: the same arithmetic on every machine */
    int64_t fx = (int64_t)floorf(u * (float)t->w * 256.0f), fy = (int64_t)floorf(v * (float)t->h * 256.0f);
    uint32_t c;
    if (!linear) c = texel(t, fx >> 8, fy >> 8, wrap);
    else {
        fx -= 128; fy -= 128;                              /* texel centres */
        int64_t x0 = fx >> 8, y0 = fy >> 8; uint32_t wx = (uint32_t)(fx & 255), wy = (uint32_t)(fy & 255);
        uint32_t p00 = texel(t, x0, y0, wrap), p10 = texel(t, x0 + 1, y0, wrap), p01 = texel(t, x0, y0 + 1, wrap), p11 = texel(t, x0 + 1, y0 + 1, wrap);
        c = 0;
        for (int ch = 0; ch < 4; ch++) {
            uint32_t a = (p00 >> (8 * ch)) & 0xFF, b = (p10 >> (8 * ch)) & 0xFF, d = (p01 >> (8 * ch)) & 0xFF, e = (p11 >> (8 * ch)) & 0xFF;
            uint32_t top = a * (256 - wx) + b * wx, bot = d * (256 - wx) + e * wx;
            uint32_t val = (top * (256 - wy) + bot * wy + 32768) >> 16;
            c |= (val > 255 ? 255 : val) << (8 * ch);
        }
    }
    out[0] = (float)((c >> 16) & 0xFF) / 255.0f; out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(c & 0xFF) / 255.0f; out[3] = (float)(c >> 24) / 255.0f;
}

/* ---- execution ------------------------------------------------------------------------- */
typedef struct {
    const dxbc_prog *p; const dxbc_env *env;
    reg r[DXBC_REGS][4], v[DXBC_REGS][4], o[DXBC_REGS][4];
    reg x[MAX_XTEMP][XTEMP_MAX][4];
} state;
static int32_t rel_value(const state *s, const operand *o) {
    if (!o->rel_dim) return 0;
    uint32_t i = (uint32_t)o->rel_idx & (DXBC_REGS - 1);
    return o->rel_type == OP_TEMP ? s->r[i][o->rel_comp].i : s->v[i][o->rel_comp].i;
}
static void read_op(const state *s, const operand *o, reg out[4]) {
    reg base[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
    int32_t i0 = o->idx[0] + (o->rel_dim == 1 ? rel_value(s, o) : 0), i1 = o->idx[1] + (o->rel_dim == 2 ? rel_value(s, o) : 0);
    switch (o->type) {
    case OP_TEMP:   if ((uint32_t)i0 < DXBC_REGS) memcpy(base, s->r[i0], sizeof base); break;
    case OP_INPUT:  if ((uint32_t)i0 < DXBC_REGS) memcpy(base, s->v[i0], sizeof base); break;
    case OP_OUTPUT: if ((uint32_t)i0 < DXBC_REGS) memcpy(base, s->o[i0], sizeof base); break;
    case OP_XTEMP:  if ((uint32_t)i0 < MAX_XTEMP && (uint32_t)i1 < XTEMP_MAX) memcpy(base, s->x[i0][i1], sizeof base); break;
    case OP_IMM32:  for (int c = 0; c < 4; c++) base[c].u = o->imm[c]; break;
    case OP_CB:
        if ((uint32_t)i0 < DXBC_SLOTS && s->env->cb[i0] && (uint32_t)i1 < s->env->cb_n[i0]) for (int c = 0; c < 4; c++) base[c].f = s->env->cb[i0][4 * (uint32_t)i1 + c];
        break;
    case OP_ICB:    if (s->p->icb && (uint32_t)i0 < s->p->icb_n) for (int c = 0; c < 4; c++) base[c].f = s->p->icb[4 * (uint32_t)i0 + c]; break;
    default: break;
    }
    for (int c = 0; c < 4; c++) {
        out[c] = base[o->swz[c]];
        if (o->abs_) out[c].f = fabsf(out[c].f);
        if (o->neg) out[c].f = -out[c].f;
    }
}
static void write_op(state *s, const operand *o, const reg val[4], int sat) {
    reg *dst = 0;
    int32_t i0 = o->idx[0] + (o->rel_dim == 1 ? rel_value(s, o) : 0), i1 = o->idx[1] + (o->rel_dim == 2 ? rel_value(s, o) : 0);
    switch (o->type) {
    case OP_TEMP:   if ((uint32_t)i0 < DXBC_REGS) dst = s->r[i0]; break;
    case OP_OUTPUT: if ((uint32_t)i0 < DXBC_REGS) dst = s->o[i0]; break;
    case OP_XTEMP:  if ((uint32_t)i0 < MAX_XTEMP && (uint32_t)i1 < XTEMP_MAX) dst = s->x[i0][i1]; break;
    default: return;                                            /* null, depth, coverage: dropped */
    }
    if (!dst) return;
    for (int c = 0; c < 4; c++) if (o->mask & (1 << c)) {
        reg v = val[c];
        if (sat) { if (v.f != v.f) v.f = 0.0f; else if (v.f < 0.0f) v.f = 0.0f; else if (v.f > 1.0f) v.f = 1.0f; }
        dst[c] = v;
    }
}
static float rnd_ne(float x) { float f = floorf(x), d = x - f; if (d > 0.5f || (d == 0.5f && fmodf(f, 2.0f) != 0.0f)) f += 1.0f; return f; }
#define F(c, expr) for (int c = 0; c < 4; c++) { d[c].f = (expr); }
#define I(c, expr) for (int c = 0; c < 4; c++) { d[c].i = (expr); }
#define U(c, expr) for (int c = 0; c < 4; c++) { d[c].u = (expr); }
#define CMP(c, cond) for (int c = 0; c < 4; c++) { d[c].u = (cond) ? 0xFFFFFFFFu : 0; }

int dxbc_exec(const dxbc_prog *p, const dxbc_env *env, const float v[DXBC_REGS][4], float o[DXBC_REGS][4]) {
    static _Thread_local state s;                               /* big, so not on the stack; one per thread, because the rasterizer shades pixels on several */
    s.p = p; s.env = env;
    memcpy(s.v, v, sizeof s.v); memset(s.o, 0, sizeof s.o); memset(s.r, 0, sizeof s.r);
    int pc = 0, steps = 0, discarded = 0;
    while (pc < p->n) {
        if (++steps > 200000) return -1;                       /* a loop that never ends is a frame that never ends */
        const insn *in = &p->code[pc];
        reg a[4], b[4], c4[4], d[4];
        int nop = in->nop;
        if (nop > 1) read_op(&s, &in->ops[1], a);
        if (nop > 2) read_op(&s, &in->ops[2], b);
        if (nop > 3) read_op(&s, &in->ops[3], c4);
        switch (in->op) {
        case 54: memcpy(d, a, sizeof d); break;                                                    /* mov */
        case 55: for (int c = 0; c < 4; c++) d[c] = a[c].u ? b[c] : c4[c]; break;                   /* movc */
        case 0:  F(c, a[c].f + b[c].f); break;
        case 56: F(c, a[c].f * b[c].f); break;
        case 50: F(c, a[c].f * b[c].f + c4[c].f); break;                                            /* mad: two roundings, contraction off */
        case 14: F(c, a[c].f / b[c].f); break;
        case 51: F(c, a[c].f < b[c].f ? a[c].f : b[c].f); break;
        case 52: F(c, a[c].f > b[c].f ? a[c].f : b[c].f); break;
        case 15: { float r = a[0].f * b[0].f + a[1].f * b[1].f; F(c, r); break; }
        case 16: { float r = a[0].f * b[0].f + a[1].f * b[1].f + a[2].f * b[2].f; F(c, r); break; }
        case 17: { float r = a[0].f * b[0].f + a[1].f * b[1].f + a[2].f * b[2].f + a[3].f * b[3].f; F(c, r); break; }
        case 26: F(c, a[c].f - floorf(a[c].f)); break;
        case 64: F(c, rnd_ne(a[c].f)); break;
        case 65: F(c, floorf(a[c].f)); break;
        case 66: F(c, ceilf(a[c].f)); break;
        case 67: F(c, truncf(a[c].f)); break;
        case 68: F(c, 1.0f / sqrtf(a[c].f)); break;
        case 75: F(c, sqrtf(a[c].f)); break;
        case 25: F(c, exp2f(a[c].f)); break;
        case 47: F(c, log2f(a[c].f)); break;
        case 77: { reg s1[4], s2[4]; for (int c = 0; c < 4; c++) { s1[c].f = sinf(a[c].f); s2[c].f = cosf(a[c].f); }
                   if (in->ops[0].type != OP_NULL) write_op(&s, &in->ops[0], s1, in->sat);
                   if (nop > 2 && in->ops[1].type != OP_NULL) { read_op(&s, &in->ops[2], a); for (int c = 0; c < 4; c++) s2[c].f = cosf(a[c].f); write_op(&s, &in->ops[1], s2, in->sat); }
                   pc++; continue; }
        case 24: CMP(c, a[c].f == b[c].f); break;
        case 57: CMP(c, a[c].f != b[c].f); break;
        case 49: CMP(c, a[c].f < b[c].f); break;
        case 29: CMP(c, a[c].f >= b[c].f); break;
        case 27: I(c, a[c].f != a[c].f ? 0 : a[c].f >= 2147483648.0f ? 0x7FFFFFFF : a[c].f <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)a[c].f); break;
        case 28: U(c, a[c].f != a[c].f || a[c].f <= 0 ? 0 : a[c].f >= 4294967296.0f ? 0xFFFFFFFFu : (uint32_t)a[c].f); break;
        case 43: F(c, (float)a[c].i); break;
        case 86: F(c, (float)a[c].u); break;
        case 30: I(c, (int32_t)((uint32_t)a[c].i + (uint32_t)b[c].i)); break;
        case 35: I(c, (int32_t)((uint32_t)a[c].i * (uint32_t)b[c].i + (uint32_t)c4[c].i)); break;
        case 82: U(c, a[c].u * b[c].u + c4[c].u); break;
        case 36: I(c, a[c].i > b[c].i ? a[c].i : b[c].i); break;
        case 37: I(c, a[c].i < b[c].i ? a[c].i : b[c].i); break;
        case 83: U(c, a[c].u > b[c].u ? a[c].u : b[c].u); break;
        case 84: U(c, a[c].u < b[c].u ? a[c].u : b[c].u); break;
        case 40: I(c, (int32_t)(0u - (uint32_t)a[c].i)); break;
        case 41: U(c, a[c].u << (b[c].u & 31)); break;
        case 42: I(c, a[c].i >> (b[c].u & 31)); break;
        case 85: U(c, a[c].u >> (b[c].u & 31)); break;
        case 1:  U(c, a[c].u & b[c].u); break;
        case 60: U(c, a[c].u | b[c].u); break;
        case 87: U(c, a[c].u ^ b[c].u); break;
        case 59: U(c, ~a[c].u); break;
        case 32: CMP(c, a[c].i == b[c].i); break;
        case 39: CMP(c, a[c].i != b[c].i); break;
        case 33: CMP(c, a[c].i >= b[c].i); break;
        case 34: CMP(c, a[c].i < b[c].i); break;
        case 80: CMP(c, a[c].u >= b[c].u); break;
        case 79: CMP(c, a[c].u < b[c].u); break;
        case 38: case 81: {                                                                          /* imul/umul: hi, lo */
            reg hi[4], lo[4];
            for (int c = 0; c < 4; c++) {
                if (in->op == 38) { int64_t r = (int64_t)a[c].i * b[c].i; hi[c].i = (int32_t)(r >> 32); lo[c].i = (int32_t)r; }
                else { uint64_t r = (uint64_t)a[c].u * b[c].u; hi[c].u = (uint32_t)(r >> 32); lo[c].u = (uint32_t)r; }
            }
            if (in->ops[0].type != OP_NULL) write_op(&s, &in->ops[0], hi, 0);
            if (in->ops[1].type != OP_NULL) { read_op(&s, &in->ops[2], a); read_op(&s, &in->ops[3], b); write_op(&s, &in->ops[1], lo, 0); }
            pc++; continue;
        }
        case 78: {                                                                                   /* udiv: quotient, remainder */
            reg q[4], r[4]; read_op(&s, &in->ops[2], a); read_op(&s, &in->ops[3], b);
            for (int c = 0; c < 4; c++) { q[c].u = b[c].u ? a[c].u / b[c].u : 0xFFFFFFFFu; r[c].u = b[c].u ? a[c].u % b[c].u : 0xFFFFFFFFu; }
            if (in->ops[0].type != OP_NULL) write_op(&s, &in->ops[0], q, 0);
            if (in->ops[1].type != OP_NULL) write_op(&s, &in->ops[1], r, 0);
            pc++; continue;
        }
        case 11: case 12: case 108: F(c, 0.0f); break;                                              /* derivatives, lod: flat */
        case 69: case 72: case 73: case 74: {                                                        /* sample[_l/_d/_b] dst, coords, t#, s# */
            uint32_t ts = (uint32_t)in->ops[2].idx[0], ss = (uint32_t)in->ops[3].idx[0];
            float out4[4] = { 1, 1, 1, 1 };
            if (ts < DXBC_SLOTS) dxbc_sample(env->tex[ts], env->wrap[ts], ss < DXBC_SLOTS ? env->linear[ss] : 0, a[0].f, a[1].f, out4);
            /* the resource operand's swizzle picks the components */
            reg tmp[4]; for (int c = 0; c < 4; c++) tmp[c].f = out4[c];
            for (int c = 0; c < 4; c++) d[c] = tmp[in->ops[2].swz[c]];
            break;
        }
        case 45: {                                                                                   /* ld dst, coords(int), t# */
            uint32_t ts = (uint32_t)in->ops[2].idx[0]; const d3d11_texture *t = ts < DXBC_SLOTS ? env->tex[ts] : 0;
            reg tmp[4]; tmp[0].f = tmp[1].f = tmp[2].f = tmp[3].f = 0.0f;
            if (t && t->pixels && a[0].i >= 0 && a[1].i >= 0 && a[0].i < t->w && a[1].i < t->h) {
                uint32_t px = t->pixels[(size_t)a[1].i * (size_t)t->pitch_px + (size_t)a[0].i];
                tmp[0].f = (float)((px >> 16) & 0xFF) / 255.0f; tmp[1].f = (float)((px >> 8) & 0xFF) / 255.0f; tmp[2].f = (float)(px & 0xFF) / 255.0f; tmp[3].f = (float)(px >> 24) / 255.0f;
            }
            for (int c = 0; c < 4; c++) d[c] = tmp[in->ops[2].swz[c]];
            break;
        }
        case 61: {                                                                                   /* resinfo dst, mip, t# */
            uint32_t ts = (uint32_t)in->ops[2].idx[0]; const d3d11_texture *t = ts < DXBC_SLOTS ? env->tex[ts] : 0;
            reg tmp[4];
            float wv = t ? (float)t->w : 0.0f, hv = t ? (float)t->h : 0.0f;
            if (in->ret_type == 2) { tmp[0].u = t ? (uint32_t)t->w : 0; tmp[1].u = t ? (uint32_t)t->h : 0; tmp[2].u = 1; tmp[3].u = 0; }
            else if (in->ret_type == 1) { tmp[0].f = wv ? 1.0f / wv : 0.0f; tmp[1].f = hv ? 1.0f / hv : 0.0f; tmp[2].f = 1.0f; tmp[3].f = 0.0f; }
            else { tmp[0].f = wv; tmp[1].f = hv; tmp[2].f = 1.0f; tmp[3].f = 0.0f; }
            for (int c = 0; c < 4; c++) d[c] = tmp[in->ops[2].swz[c]];
            break;
        }
        case 13: { read_op(&s, &in->ops[0], a); int t = a[0].u != 0; if (t == in->test_nz) discarded = 1; pc++; continue; }   /* discard */
        case 31: { read_op(&s, &in->ops[0], a); int t = a[0].u != 0;                             /* if: taken falls through; else the body is skipped -- to just past the else, or past the endif */
                   pc = t == in->test_nz ? pc + 1 : in->target + 1; continue; }
        case 18: pc = in->target + 1; continue;                                                     /* else: skip to after endif */
        case 21: case 48: pc++; continue;                                                            /* endif, loop */
        case 22: pc = in->target + 1; continue;                                                      /* endloop: back to the loop (its body follows) */
        case 2: pc = in->target + 1; continue;                                                       /* break */
        case 3: { read_op(&s, &in->ops[0], a); int t = a[0].u != 0; if (t == in->test_nz) pc = in->target + 1; else pc++; continue; }
        case 7: pc = in->target + 1; continue;                                                       /* continue */
        case 8: { read_op(&s, &in->ops[0], a); int t = a[0].u != 0; if (t == in->test_nz) pc = in->target + 1; else pc++; continue; }
        case 62: pc = p->n; continue;                                                                /* ret */
        case 63: { read_op(&s, &in->ops[0], a); int t = a[0].u != 0; if (t == in->test_nz) pc = p->n; else pc++; continue; }
        case 58: pc++; continue;
        default: return -1;
        }
        write_op(&s, &in->ops[0], d, in->sat);
        pc++;
    }
    memcpy(o, s.o, sizeof s.o);
    return discarded;
}
