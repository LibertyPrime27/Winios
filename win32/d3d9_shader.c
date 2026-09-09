/* A shader interpreter for Direct3D 9.
 *
 * A D3D9 game hands CreateVertexShader and CreatePixelShader the token
 * stream the HLSL compiler emits for vs_2_0 .. vs_3_0 and ps_2_0 .. ps_3_0
 * -- a version word, then instructions each made of an opcode token and its
 * operand tokens, then an end marker. Until this file, d3d9.c accepted those
 * and drew with a fixed-function reading of the vertex (texture times vertex
 * colour), which is right for a sprite and wrong for everything the shader
 * itself decides: GameMaker Studio's runner multiplies every vertex by a
 * matrix it keeps in c0..c3, and its pixel shaders tint, fade, and cut out.
 *
 * What is here is the whole of the two instruction sets as compilers use
 * them: the arithmetic (mov add sub mad mul rcp rsq dp3 dp4 dp2add min max
 * slt sge exp log lit dst lrp frc pow crs sgn abs nrm sincos expp logp cmp
 * cnd), the matrix macros (m4x4 m4x3 m3x4 m3x3 m3x2), the flow control (if
 * ifc else endif loop endloop rep endrep break breakc call callnz label ret),
 * relative addressing through a0 and aL, def/defi/defb, dcl, and texturing
 * (texld with its projected and biased forms, texldl, texkill). Not here:
 * ps_1_x and vs_1_1, whose model is different enough to be another file,
 * predication (setp, breakp, predicated instructions), and the ps_1_x
 * texture macros. A program using any of them is refused by name so the
 * report can say why the fixed-function reading was used instead.
 *
 * Interpreted, on the CPU, per vertex and per pixel -- the same position as
 * dxbc_exec.c, for the same reason, and with the same discipline: the
 * arithmetic is compiled with contraction off so a frame is the same on
 * every machine and the checksums keep meaning something. Sampling goes
 * through dxbc_sample, so both APIs read a texture the same way.
 */
#include "d3d9_shader.h"
#include "dxbc_exec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D3DSHADER_PARAM_REGISTER_TYPE */
enum { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3 /* a0 in a vs, t# in a ps */, RT_RASTOUT = 4, RT_ATTROUT = 5,
       RT_TEXCRDOUT = 6 /* oT# below vs_3_0, o# in it */, RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10,
       RT_CONST2 = 11, RT_CONST3 = 12, RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPFLOAT16 = 16, RT_MISCTYPE = 17,
       RT_LABEL = 18, RT_PREDICATE = 19 };
/* D3DSHADER_INSTRUCTION_OPCODE_TYPE */
enum { OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT, OP_SGE,
       OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL, OP_CALLNZ,
       OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP,
       OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI,
       OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_TEXBEM, OP_TEXBEML, OP_TEXREG2AR, OP_TEXREG2GB, OP_TEXM3x2PAD, OP_TEXM3x2TEX,
       OP_TEXM3x3PAD, OP_TEXM3x3TEX, OP_RESERVED0, OP_TEXM3x3SPEC, OP_TEXM3x3VSPEC, OP_EXPP, OP_LOGP, OP_CND, OP_DEF,
       OP_TEXREG2RGB, OP_TEXDP3TEX, OP_TEXM3x2DEPTH, OP_TEXDP3, OP_TEXM3x3, OP_TEXDEPTH, OP_CMP, OP_BEM, OP_DP2ADD, OP_DSX,
       OP_DSY, OP_TEXLDD, OP_SETP, OP_TEXLDL, OP_BREAKP, OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF };
/* source modifiers (D3DSHADER_PARAM_SRCMOD_TYPE >> 24) */
enum { SM_NONE = 0, SM_NEG, SM_BIAS, SM_BIASNEG, SM_SIGN, SM_SIGNNEG, SM_COMP, SM_X2, SM_X2NEG, SM_DZ, SM_DW, SM_ABS, SM_ABSNEG, SM_NOT };
enum { MAX_INSN = 4096, MAX_DECL = 32, MAX_DEF = 256, MAX_LOOP = 8, MAX_CALL = 8, NCONST = 256 };

typedef struct {
    uint8_t  type;
    uint16_t reg;
    uint8_t  swz[4];                 /* source: component per component */
    uint8_t  mask;                   /* destination */
    uint8_t  mod;                    /* source modifier */
    uint8_t  rel, rel_type, rel_comp; uint16_t rel_reg;
} operand;
typedef struct {
    uint16_t op; uint8_t ctrl, nsrc, sat, has_dst;
    operand dst, src[4];
    int32_t target;                  /* the matching else/endif, endloop/endrep, loop/rep; a label's insn for call */
} insn;
typedef struct { uint8_t kind, reg; uint32_t v[4]; } def;   /* kind 0 float, 1 int, 2 bool */
struct sm3_prog {
    int is_vs, version;
    insn *code; int n;
    sm3_decl in[MAX_DECL]; int nin;
    sm3_decl out[MAX_DECL]; int nout;
    uint32_t samplers;
    def defs[MAX_DEF]; int ndef;
    int labels[32];                  /* insn index of label n, or -1 */
};

/* ---- decoding ------------------------------------------------------------------ */
static int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static void decode_operand(uint32_t t, operand *o, int is_dst) {
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)reg_type(t);
    o->reg = (uint16_t)(t & 0x7FF);
    if (is_dst) { o->mask = (uint8_t)((t >> 16) & 0xF); }
    else {
        for (int c = 0; c < 4; c++) o->swz[c] = (uint8_t)((t >> (16 + 2 * c)) & 3);
        o->mod = (uint8_t)((t >> 24) & 0xF);
    }
    o->rel = (t >> 13) & 1;
    /* c2048.. are the same constants continued: the type says which quarter */
    if (o->type == RT_CONST2) { o->type = RT_CONST; o->reg = (uint16_t)(o->reg + 2048); }
    else if (o->type == RT_CONST3) { o->type = RT_CONST; o->reg = (uint16_t)(o->reg + 4096); }
    else if (o->type == RT_CONST4) { o->type = RT_CONST; o->reg = (uint16_t)(o->reg + 6144); }
}
static void refuse(char *why, size_t n, const char *what) { if (why && n) snprintf(why, n, "%s", what); }

int sm3_vary_slot(int usage, int index) {
    switch (usage) {
    case 10: return index < 2 ? index : -1;         /* D3DDECLUSAGE_COLOR */
    case 5:  return index < 8 ? 2 + index : -1;     /* TEXCOORD */
    case 0: case 9: return -1;                      /* POSITION, POSITIONT */
    case 3:  return 10;                             /* NORMAL */
    default: return 11;
    }
}

sm3_prog *sm3_prog_new(const uint32_t *tk, uint32_t words, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    if (!tk || words < 2) { refuse(why, why_len, "an empty shader"); return 0; }
    uint32_t ver = tk[0];
    int is_vs = (ver >> 16) == 0xFFFE, is_ps = (ver >> 16) == 0xFFFF;
    int major = (int)((ver >> 8) & 0xFF), minor = (int)(ver & 0xFF);
    if (!is_vs && !is_ps) { refuse(why, why_len, "not a D3D9 shader (no version token)"); return 0; }
    if (major < 2) { refuse(why, why_len, is_vs ? "vs_1_1 (only vs_2_0 and later run here)" : "ps_1_x (only ps_2_0 and later run here)"); return 0; }
    if (major > 3) { refuse(why, why_len, "a shader model above 3"); return 0; }
    sm3_prog *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->is_vs = is_vs; p->version = major * 10 + minor;
    p->code = calloc(MAX_INSN, sizeof *p->code);
    if (!p->code) { free(p); return 0; }
    for (int i = 0; i < 32; i++) p->labels[i] = -1;
    int stack[MAX_LOOP + 16], sp = 0;          /* open if/loop/rep, as insn indices */
    uint32_t pos = 1;
    while (pos < words) {
        uint32_t t = tk[pos];
        if (t == OP_END) break;
        if ((t & 0xFFFF) == OP_COMMENT) { pos += 1 + ((t >> 16) & 0x7FFF); continue; }
        uint32_t op = t & 0xFFFF, len = (t >> 24) & 0xF;
        if (t & (1u << 28)) { refuse(why, why_len, "a predicated instruction"); goto fail; }
        if (pos + 1 + len > words) { refuse(why, why_len, "an instruction running past the end of the shader"); goto fail; }
        const uint32_t *a = tk + pos + 1;
        pos += 1 + len;
        if (op == OP_NOP || op == OP_PHASE) continue;
        if (op == OP_DCL) {
            /* usage token, then the register */
            if (len < 2) { refuse(why, why_len, "a truncated dcl"); goto fail; }
            operand o; decode_operand(a[1], &o, 1);
            sm3_decl d = { (uint8_t)o.reg, (uint8_t)(a[0] & 0x1F), (uint8_t)((a[0] >> 16) & 0xF), o.type };
            if (o.type == RT_SAMPLER) { if (o.reg < 16) p->samplers |= 1u << o.reg; }
            else if (o.type == RT_INPUT || o.type == RT_ADDR || o.type == RT_MISCTYPE) { if (p->nin < MAX_DECL) p->in[p->nin++] = d; }
            else if (o.type == RT_TEXCRDOUT || o.type == RT_ATTROUT || o.type == RT_RASTOUT) { if (p->nout < MAX_DECL) p->out[p->nout++] = d; }
            continue;
        }
        if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB) {
            operand o; decode_operand(a[0], &o, 1);
            if (p->ndef < MAX_DEF) {
                def *d = &p->defs[p->ndef++];
                d->kind = op == OP_DEF ? 0 : op == OP_DEFI ? 1 : 2; d->reg = (uint8_t)o.reg;
                for (int c = 0; c < 4; c++) d->v[c] = c + 1 < (int)len ? a[c + 1] : 0;
            }
            continue;
        }
        if (p->n >= MAX_INSN) { refuse(why, why_len, "a shader longer than 4096 instructions"); goto fail; }
        insn *in = &p->code[p->n];
        memset(in, 0, sizeof *in);
        in->op = (uint16_t)op; in->ctrl = (uint8_t)((t >> 16) & 0xFF); in->target = -1;
        /* which instructions have no destination */
        int nodst = op == OP_IF || op == OP_IFC || op == OP_ELSE || op == OP_ENDIF || op == OP_LOOP || op == OP_ENDLOOP
                 || op == OP_REP || op == OP_ENDREP || op == OP_BREAK || op == OP_BREAKC || op == OP_CALL || op == OP_CALLNZ
                 || op == OP_RET || op == OP_LABEL || op == OP_TEXKILL;
        uint32_t k = 0;
        if (!nodst && len > 0) {
            decode_operand(a[0], &in->dst, 1); in->has_dst = 1; k = 1;
            in->sat = (a[0] >> 20) & 1;
            if (in->dst.rel) { if (k >= len) { refuse(why, why_len, "a truncated operand"); goto fail; } operand r; decode_operand(a[k++], &r, 0); in->dst.rel_type = r.type; in->dst.rel_reg = r.reg; in->dst.rel_comp = r.swz[0]; }
        }
        if (op == OP_TEXKILL && len > 0) { decode_operand(a[0], &in->dst, 1); in->has_dst = 1; k = 1; }   /* texkill names its register as a destination */
        while (k < len && in->nsrc < 4) {
            operand *s = &in->src[in->nsrc++];
            decode_operand(a[k++], s, 0);
            if (s->rel) { if (k >= len) { refuse(why, why_len, "a truncated operand"); goto fail; } operand r; decode_operand(a[k++], &r, 0); s->rel_type = r.type; s->rel_reg = r.reg; s->rel_comp = r.swz[0]; }
            if (s->type == RT_PREDICATE) { refuse(why, why_len, "the predicate register"); goto fail; }
        }
        switch (op) {
        case OP_MOV: case OP_ADD: case OP_SUB: case OP_MAD: case OP_MUL: case OP_RCP: case OP_RSQ: case OP_DP3: case OP_DP4:
        case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE: case OP_EXP: case OP_LOG: case OP_LIT: case OP_DST: case OP_LRP:
        case OP_FRC: case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: case OP_POW: case OP_CRS: case OP_SGN:
        case OP_ABS: case OP_NRM: case OP_SINCOS: case OP_MOVA: case OP_EXPP: case OP_LOGP: case OP_CND: case OP_CMP: case OP_DP2ADD:
        case OP_DSX: case OP_DSY: case OP_TEXKILL: case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: case OP_RET:
            break;
        case OP_LABEL:
            if (in->nsrc == 1 && in->src[0].reg < 32) p->labels[in->src[0].reg] = p->n;
            break;
        case OP_CALL: case OP_CALLNZ:
            break;
        case OP_IF: case OP_IFC: case OP_LOOP: case OP_REP:
            if (sp >= (int)(sizeof stack / sizeof stack[0])) { refuse(why, why_len, "flow control nested too deep"); goto fail; }
            stack[sp++] = p->n;
            break;
        case OP_ELSE:
            if (!sp) { refuse(why, why_len, "an else without an if"); goto fail; }
            p->code[stack[sp - 1]].target = p->n;            /* the if jumps past the else when not taken */
            stack[sp - 1] = p->n;                             /* the else will jump to the endif */
            break;
        case OP_ENDIF:
            if (!sp) { refuse(why, why_len, "an endif without an if"); goto fail; }
            p->code[stack[--sp]].target = p->n;
            break;
        case OP_ENDLOOP: case OP_ENDREP:
            if (!sp) { refuse(why, why_len, "an endloop without a loop"); goto fail; }
            p->code[stack[sp - 1]].target = p->n;             /* loop -> its end, to skip a zero-count loop */
            in->target = stack[--sp];                         /* end -> its loop */
            break;
        case OP_BREAK: case OP_BREAKC:
            /* resolved at run time from the loop stack */
            break;
        case OP_SETP: case OP_BREAKP: refuse(why, why_len, "predication (setp/breakp)"); goto fail;
        case OP_TEXCOORD: case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXM3x2PAD: case OP_TEXM3x2TEX:
        case OP_TEXM3x3PAD: case OP_TEXM3x3TEX: case OP_TEXM3x3SPEC: case OP_TEXM3x3VSPEC: case OP_TEXREG2RGB: case OP_TEXDP3TEX:
        case OP_TEXM3x2DEPTH: case OP_TEXDP3: case OP_TEXM3x3: case OP_TEXDEPTH: case OP_BEM:
            refuse(why, why_len, "a ps_1_x texture instruction"); goto fail;
        default: {
            static char buf[64]; snprintf(buf, sizeof buf, "opcode %u", op); refuse(why, why_len, buf); goto fail;
        }
        }
        p->n++;
    }
    if (sp) { refuse(why, why_len, "an if or loop left open"); goto fail; }
    return p;
fail:
    sm3_prog_free(p);
    return 0;
}
void sm3_prog_free(sm3_prog *p) { if (!p) return; free(p->code); free(p); }
int sm3_prog_is_vs(const sm3_prog *p) { return p ? p->is_vs : 0; }
int sm3_prog_version(const sm3_prog *p) { return p ? p->version : 0; }
int sm3_prog_inputs(const sm3_prog *p, const sm3_decl **d) { if (d) *d = p ? p->in : 0; return p ? p->nin : 0; }
int sm3_prog_outputs(const sm3_prog *p, const sm3_decl **d) { if (d) *d = p ? p->out : 0; return p ? p->nout : 0; }
uint32_t sm3_prog_samplers(const sm3_prog *p) { return p ? p->samplers : 0; }
void sm3_prog_apply_defs(const sm3_prog *p, float cf[][4], int cf_n, int32_t ci[16][4], uint32_t *cb) {
    if (!p) return;
    for (int i = 0; i < p->ndef; i++) {
        const def *d = &p->defs[i];
        if (d->kind == 0 && cf && d->reg < cf_n) memcpy(cf[d->reg], d->v, 16);
        else if (d->kind == 1 && ci && d->reg < 16) memcpy(ci[d->reg], d->v, 16);
        else if (d->kind == 2 && cb && d->reg < 32) { if (d->v[0]) *cb |= 1u << d->reg; else *cb &= ~(1u << d->reg); }
    }
}

/* ---- execution ------------------------------------------------------------------------- */
typedef struct {
    const sm3_prog *p; const sm3_env *env;
    float r[SM3_REGS][4], v[16][4], t[8][4];
    int32_t a0[4], aL;
    float opos[4], od[2][4], ot[8][4], o[SM3_VARY + 4][4], oc[4][4];
    float vpos[4], vface;
    int killed;
    struct { int count, step, body_pc; int32_t al_outer; int is_loop; } loops[MAX_LOOP]; int nloop;
    int calls[MAX_CALL]; int ncall;
} state;

static int32_t rel_of(const state *s, const operand *o) {
    if (!o->rel) return 0;
    if (o->rel_type == RT_LOOP) return s->aL;
    return s->a0[o->rel_comp & 3];
}
static void read_src(const state *s, const operand *o, float out[4]) {
    float base[4] = { 0, 0, 0, 0 };
    int32_t reg = (int32_t)o->reg + rel_of(s, o);
    const sm3_env *e = s->env;
    switch (o->type) {
    case RT_TEMP:    if ((uint32_t)reg < SM3_REGS) memcpy(base, s->r[reg], 16); break;
    case RT_INPUT:   if ((uint32_t)reg < 16) memcpy(base, s->v[reg], 16); break;
    case RT_CONST:   if (e->cf && reg >= 0 && reg < e->cf_n) memcpy(base, e->cf[reg], 16); break;
    case RT_ADDR:    if (s->p->is_vs) { for (int c = 0; c < 4; c++) base[c] = (float)s->a0[c]; }
                     else if ((uint32_t)reg < 8) memcpy(base, s->t[reg], 16);
                     break;
    case RT_CONSTINT: if (e->ci && (uint32_t)reg < 16) for (int c = 0; c < 4; c++) base[c] = (float)e->ci[reg][c]; break;
    case RT_CONSTBOOL: base[0] = base[1] = base[2] = base[3] = (uint32_t)reg < 32 && (e->cb >> reg & 1) ? 1.0f : 0.0f; break;
    case RT_LOOP:    base[0] = base[1] = base[2] = base[3] = (float)s->aL; break;
    case RT_MISCTYPE: if (reg == 0) memcpy(base, s->vpos, 16); else { base[0] = base[1] = base[2] = base[3] = s->vface; } break;
    case RT_COLOROUT: if ((uint32_t)reg < 4) memcpy(base, s->oc[reg], 16); break;
    case RT_TEXCRDOUT: if (s->p->version >= 30) { if ((uint32_t)reg < SM3_VARY + 4) memcpy(base, s->o[reg], 16); } else if ((uint32_t)reg < 8) memcpy(base, s->ot[reg], 16); break;
    case RT_ATTROUT: if ((uint32_t)reg < 2) memcpy(base, s->od[reg], 16); break;
    default: break;
    }
    float sw[4];
    for (int c = 0; c < 4; c++) sw[c] = base[o->swz[c]];
    switch (o->mod) {
    case SM_NEG:     for (int c = 0; c < 4; c++) sw[c] = -sw[c]; break;
    case SM_BIAS:    for (int c = 0; c < 4; c++) sw[c] = sw[c] - 0.5f; break;
    case SM_BIASNEG: for (int c = 0; c < 4; c++) sw[c] = -(sw[c] - 0.5f); break;
    case SM_SIGN:    for (int c = 0; c < 4; c++) sw[c] = sw[c] * 2.0f - 1.0f; break;
    case SM_SIGNNEG: for (int c = 0; c < 4; c++) sw[c] = -(sw[c] * 2.0f - 1.0f); break;
    case SM_COMP:    for (int c = 0; c < 4; c++) sw[c] = 1.0f - sw[c]; break;
    case SM_X2:      for (int c = 0; c < 4; c++) sw[c] = sw[c] * 2.0f; break;
    case SM_X2NEG:   for (int c = 0; c < 4; c++) sw[c] = -(sw[c] * 2.0f); break;
    case SM_DZ:      { float z = base[2] != 0.0f ? base[2] : 1.0f; sw[0] = base[0] / z; sw[1] = base[1] / z; } break;
    case SM_DW:      { float q = base[3] != 0.0f ? base[3] : 1.0f; sw[0] = base[0] / q; sw[1] = base[1] / q; } break;
    case SM_ABS:     for (int c = 0; c < 4; c++) sw[c] = fabsf(sw[c]); break;
    case SM_ABSNEG:  for (int c = 0; c < 4; c++) sw[c] = -fabsf(sw[c]); break;
    case SM_NOT:     for (int c = 0; c < 4; c++) sw[c] = sw[c] == 0.0f ? 1.0f : 0.0f; break;
    default: break;
    }
    memcpy(out, sw, 16);
}
static void write_dst(state *s, const operand *o, const float val[4], int sat) {
    float *dst = 0;
    int32_t reg = (int32_t)o->reg + rel_of(s, o);
    switch (o->type) {
    case RT_TEMP:    if ((uint32_t)reg < SM3_REGS) dst = s->r[reg]; break;
    case RT_RASTOUT: if (reg == 0) dst = s->opos; break;                   /* oFog and oPts are not drawn */
    case RT_ATTROUT: if ((uint32_t)reg < 2) dst = s->od[reg]; break;
    case RT_TEXCRDOUT: if (s->p->version >= 30) { if ((uint32_t)reg < SM3_VARY + 4) dst = s->o[reg]; } else if ((uint32_t)reg < 8) dst = s->ot[reg]; break;
    case RT_COLOROUT: if ((uint32_t)reg < 4) dst = s->oc[reg]; break;
    case RT_ADDR:    if (s->p->is_vs) { for (int c = 0; c < 4; c++) if (o->mask & (1 << c)) s->a0[c] = (int32_t)val[c]; } return;
    default: return;                                                       /* oDepth, and anything unwritable: dropped */
    }
    if (!dst) return;
    for (int c = 0; c < 4; c++) if (o->mask & (1 << c)) {
        float x = val[c];
        if (sat) { if (x != x) x = 0.0f; else if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f; }
        dst[c] = x;
    }
}
static float rnd_ne(float x) { float f = floorf(x), d = x - f; if (d > 0.5f || (d == 0.5f && fmodf(f, 2.0f) != 0.0f)) f += 1.0f; return f; }
static int compare(int cc, float a, float b) {
    switch (cc) {
    case 1: return a > b; case 2: return a == b; case 3: return a >= b;
    case 4: return a < b; case 5: return a != b; case 6: return a <= b;
    default: return 0;
    }
}
static void sample(const state *s, const operand *smp, const float coord[4], int project, float out[4]) {
    uint32_t sn = smp->reg;
    out[0] = out[1] = out[2] = out[3] = 1.0f;
    if (sn >= 16 || !s->env->tex[sn]) return;
    float u = coord[0], v = coord[1];
    if (project && coord[3] != 0.0f) { u /= coord[3]; v /= coord[3]; }
    dxbc_sample(s->env->tex[sn], s->env->wrap[sn], s->env->linear[sn], u, v, out);
}
/* one loop level: break out of it */
static int break_out(state *s, int *pc) {
    if (!s->nloop) return 0;
    const insn *L = &s->p->code[s->loops[s->nloop - 1].body_pc - 1];    /* the loop/rep instruction */
    *pc = L->target + 1;
    if (s->loops[s->nloop - 1].is_loop) s->aL = s->loops[s->nloop - 1].al_outer;
    s->nloop--;
    return 1;
}
#define F(c, expr) for (int c = 0; c < 4; c++) { d[c] = (expr); }
static int run(state *s) {
    const sm3_prog *p = s->p;
    int pc = 0, steps = 0;
    while (pc < p->n) {
        if (++steps > 200000) return -1;
        const insn *in = &p->code[pc];
        float a[4], b[4], c4[4], d[4];
        if (in->nsrc > 0) read_src(s, &in->src[0], a);
        if (in->nsrc > 1) read_src(s, &in->src[1], b);
        if (in->nsrc > 2) read_src(s, &in->src[2], c4);
        switch (in->op) {
        case OP_MOV:  memcpy(d, a, 16); break;
        case OP_ADD:  F(c, a[c] + b[c]); break;
        case OP_SUB:  F(c, a[c] - b[c]); break;
        case OP_MUL:  F(c, a[c] * b[c]); break;
        case OP_MAD:  F(c, a[c] * b[c] + c4[c]); break;                                   /* two roundings, contraction off */
        case OP_RCP:  F(c, a[c] == 0.0f ? INFINITY : 1.0f / a[c]); break;
        case OP_RSQ:  F(c, a[c] == 0.0f ? INFINITY : 1.0f / sqrtf(fabsf(a[c]))); break;
        case OP_DP3:  { float r = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; F(c, r); break; }
        case OP_DP4:  { float r = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; F(c, r); break; }
        case OP_DP2ADD: { float r = a[0] * b[0] + a[1] * b[1] + c4[0]; F(c, r); break; }
        case OP_MIN:  F(c, a[c] < b[c] ? a[c] : b[c]); break;
        case OP_MAX:  F(c, a[c] > b[c] ? a[c] : b[c]); break;
        case OP_SLT:  F(c, a[c] < b[c] ? 1.0f : 0.0f); break;
        case OP_SGE:  F(c, a[c] >= b[c] ? 1.0f : 0.0f); break;
        case OP_EXP: case OP_EXPP: F(c, exp2f(a[c])); break;
        case OP_LOG: case OP_LOGP: F(c, a[c] == 0.0f ? -INFINITY : log2f(fabsf(a[c]))); break;
        case OP_LIT: {
            d[0] = 1.0f; d[1] = a[0] > 0.0f ? a[0] : 0.0f; d[3] = 1.0f;
            float pw = a[3] < -127.9961f ? -127.9961f : a[3] > 127.9961f ? 127.9961f : a[3];
            d[2] = a[0] > 0.0f && a[1] > 0.0f ? powf(a[1], pw) : 0.0f;
            break;
        }
        case OP_DST:  d[0] = 1.0f; d[1] = a[1] * b[1]; d[2] = a[2]; d[3] = b[3]; break;
        case OP_LRP:  F(c, a[c] * (b[c] - c4[c]) + c4[c]); break;
        case OP_FRC:  F(c, a[c] - floorf(a[c])); break;
        case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
            /* dst.c = dot(src0, src1[c]) for as many rows as the name says */
            int rows = in->op == OP_M4x4 || in->op == OP_M3x4 ? 4 : in->op == OP_M4x3 || in->op == OP_M3x3 ? 3 : 2;
            int cols = in->op == OP_M4x4 || in->op == OP_M4x3 ? 4 : 3;
            d[0] = d[1] = d[2] = d[3] = 0.0f;
            for (int r = 0; r < rows; r++) {
                operand row = in->src[1]; row.reg = (uint16_t)(row.reg + r);
                float m[4]; read_src(s, &row, m);
                float acc = a[0] * m[0] + a[1] * m[1] + a[2] * m[2];
                if (cols == 4) acc += a[3] * m[3];
                d[r] = acc;
            }
            break;
        }
        case OP_POW:  F(c, powf(fabsf(a[c]), b[c])); break;
        case OP_CRS:  d[0] = a[1] * b[2] - a[2] * b[1]; d[1] = a[2] * b[0] - a[0] * b[2]; d[2] = a[0] * b[1] - a[1] * b[0]; d[3] = 0.0f; break;
        case OP_SGN:  F(c, a[c] > 0.0f ? 1.0f : a[c] < 0.0f ? -1.0f : 0.0f); break;
        case OP_ABS:  F(c, fabsf(a[c])); break;
        case OP_NRM: {
            float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            float inv = len != 0.0f ? 1.0f / len : INFINITY;
            F(c, a[c] * inv);
            break;
        }
        case OP_SINCOS: d[0] = cosf(a[0]); d[1] = sinf(a[0]); d[2] = 0.0f; d[3] = 0.0f; break;
        case OP_MOVA: {
            /* a0 = round to nearest; written through the address register path */
            float rd[4]; F(c, rnd_ne(a[c])); memcpy(rd, d, 16);
            for (int c = 0; c < 4; c++) if (in->dst.mask & (1 << c)) s->a0[c] = (int32_t)rd[c];
            pc++; continue;
        }
        case OP_CND:  F(c, a[c] > 0.5f ? b[c] : c4[c]); break;
        case OP_CMP:  F(c, a[c] >= 0.0f ? b[c] : c4[c]); break;
        case OP_DSX: case OP_DSY: F(c, 0.0f); break;                                          /* flat: one pixel at a time */
        case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
            /* texld dst, coords, s#; ctrl 1 is texldp, 2 texldb */
            if (in->nsrc < 2) { pc++; continue; }
            sample(s, &in->src[1], a, in->op == OP_TEX && (in->ctrl & 0xF) == 1, d);
            break;
        }
        case OP_TEXKILL: {
            /* kills on any of x, y, z below zero -- and w too from ps_3_0 */
            float k[4]; operand src = in->dst; src.swz[0] = 0; src.swz[1] = 1; src.swz[2] = 2; src.swz[3] = 3; src.mod = 0;
            read_src(s, &src, k);
            if (k[0] < 0.0f || k[1] < 0.0f || k[2] < 0.0f || (p->version >= 30 && k[3] < 0.0f)) s->killed = 1;
            pc++; continue;
        }
        case OP_IF:   pc = a[0] != 0.0f ? pc + 1 : in->target + 1; continue;
        case OP_IFC:  pc = compare(in->ctrl & 7, a[0], b[0]) ? pc + 1 : in->target + 1; continue;
        case OP_ELSE: pc = in->target + 1; continue;
        case OP_ENDIF: pc++; continue;
        case OP_LOOP: case OP_REP: {
            /* loop aL, i#: count, start, step in i#; rep i#: count */
            if (s->nloop >= MAX_LOOP) return -1;
            const operand *io = &in->src[in->op == OP_LOOP ? 1 : 0];
            int32_t cnt = 0, start = 0, step = 0;
            if (io->type == RT_CONSTINT && s->env->ci && io->reg < 16) { cnt = s->env->ci[io->reg][0]; start = s->env->ci[io->reg][1]; step = s->env->ci[io->reg][2]; }
            if (cnt <= 0) { pc = in->target + 1; continue; }
            s->loops[s->nloop].count = cnt; s->loops[s->nloop].step = step; s->loops[s->nloop].body_pc = pc + 1;
            s->loops[s->nloop].is_loop = in->op == OP_LOOP; s->loops[s->nloop].al_outer = s->aL;
            if (in->op == OP_LOOP) s->aL = start;
            s->nloop++;
            pc++; continue;
        }
        case OP_ENDLOOP: case OP_ENDREP: {
            if (!s->nloop) { pc++; continue; }
            int top = s->nloop - 1;
            if (--s->loops[top].count > 0) { if (s->loops[top].is_loop) s->aL += s->loops[top].step; pc = s->loops[top].body_pc; continue; }
            if (s->loops[top].is_loop) s->aL = s->loops[top].al_outer;
            s->nloop--;
            pc++; continue;
        }
        case OP_BREAK:  if (!break_out(s, &pc)) pc++; continue;
        case OP_BREAKC: if (compare(in->ctrl & 7, a[0], b[0])) { if (!break_out(s, &pc)) pc++; } else pc++; continue;
        case OP_CALL: case OP_CALLNZ: {
            int taken = in->op == OP_CALL || (in->nsrc > 1 && b[0] != 0.0f);
            int lbl = in->nsrc > 0 && in->src[0].reg < 32 ? p->labels[in->src[0].reg] : -1;
            if (!taken || lbl < 0) { pc++; continue; }
            if (s->ncall >= MAX_CALL) return -1;
            s->calls[s->ncall++] = pc + 1;
            pc = lbl + 1; continue;
        }
        case OP_LABEL:
            /* reached by falling through: the subroutines follow the main body's ret */
            pc = p->n; continue;
        case OP_RET:
            if (s->ncall) { pc = s->calls[--s->ncall]; continue; }
            pc = p->n; continue;
        default: return -1;
        }
        if (in->has_dst) write_dst(s, &in->dst, d, in->sat);
        pc++;
    }
    return 0;
}

int sm3_exec_vs(const sm3_prog *p, const sm3_env *env, const float v[16][4], float pos[4], float var[SM3_VARY][4]) {
    static _Thread_local state s;                        /* big; one per thread, not on the stack */
    memset(&s, 0, sizeof s);
    s.p = p; s.env = env;
    memcpy(s.v, v, sizeof s.v);
    int r = run(&s);
    if (r < 0) return -1;
    memset(pos, 0, 16);
    for (int k = 0; k < SM3_VARY; k++) memset(var[k], 0, 16);
    if (p->version >= 30) {
        for (int i = 0; i < p->nout; i++) {
            const sm3_decl *d = &p->out[i];
            if (d->reg >= SM3_VARY + 4) continue;
            if (d->usage == 0 || d->usage == 9) { if (d->index == 0) memcpy(pos, s.o[d->reg], 16); continue; }
            int slot = sm3_vary_slot(d->usage, d->index);
            if (slot >= 0) memcpy(var[slot], s.o[d->reg], 16);
        }
    } else {
        memcpy(pos, s.opos, 16);
        memcpy(var[0], s.od[0], 16); memcpy(var[1], s.od[1], 16);
        for (int k = 0; k < 8; k++) memcpy(var[2 + k], s.ot[k], 16);
    }
    return 0;
}
int sm3_exec_ps(const sm3_prog *p, const sm3_env *env, const float var[SM3_VARY][4], const float vpos[4], float color[4]) {
    static _Thread_local state s;
    memset(&s, 0, sizeof s);
    s.p = p; s.env = env;
    memcpy(s.vpos, vpos, 16); s.vface = 1.0f;
    if (p->version >= 30) {
        for (int i = 0; i < p->nin; i++) {
            const sm3_decl *d = &p->in[i];
            if (d->reg >= 16 || d->rtype != RT_INPUT) continue;    /* vPos and vFace are their own registers */
            int slot = sm3_vary_slot(d->usage, d->index);
            if (slot >= 0) memcpy(s.v[d->reg], var[slot], 16);
            else if (d->usage == 0 || d->usage == 9) memcpy(s.v[d->reg], vpos, 16);
        }
    } else {
        memcpy(s.v[0], var[0], 16); memcpy(s.v[1], var[1], 16);
        for (int k = 0; k < 8; k++) memcpy(s.t[k], var[2 + k], 16);
    }
    int r = run(&s);
    if (r < 0) return -1;
    memcpy(color, s.oc[0], 16);
    return s.killed ? 1 : 0;
}
