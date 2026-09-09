/* The Direct3D 9 shader interpreter, instruction by instruction.
 *
 * Token streams are assembled here from the format's own encoding -- the
 * version word, the opcode token with its length, the parameter tokens with
 * their register type split across two bit fields, masks, swizzles and
 * modifiers -- so what is under test is the decoder and the arithmetic, not
 * a compiler. Results are checked to the bit where the arithmetic is exact,
 * because a shader is arithmetic and "close" is a frame that differs
 * between two machines.
 *
 * The last section runs a vertex shader and a pixel shader through the same
 * rasterizer d3d9.c uses, on the same quad d3dshader.exe draws, and prints
 * the frame's checksum: that number is what tests/win32/run.sh expects from
 * the guest, so the two pipelines are held to the same pixels. */
#include "d3d9_shader.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ok(int c, const char *what) { checks++; if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } }

/* ---- a tiny assembler for the token format ---- */
static uint32_t code[1024]; static int nw, op_at;
static void w(uint32_t v) { code[nw++] = v; }
static void begin(int vs, int major, int minor) { nw = 0; w((vs ? 0xFFFE0000u : 0xFFFF0000u) | (uint32_t)major << 8 | (uint32_t)minor); }
static const uint32_t *finish(void) { w(0x0000FFFF); return code; }
static void op(uint32_t opcode) { op_at = nw; w(opcode); }
static void opc(uint32_t opcode, int ctrl) { op_at = nw; w(opcode | (uint32_t)ctrl << 16); }
static void end(void) { code[op_at] |= (uint32_t)(nw - op_at - 1) << 24; }
static uint32_t regtok(int type, int reg) { return 0x80000000u | (uint32_t)(type & 7) << 28 | (uint32_t)((type >> 3) & 3) << 11 | (uint32_t)reg; }
static void dst(int type, int reg, int mask) { w(regtok(type, reg) | (uint32_t)mask << 16); }
static void dst_sat(int type, int reg, int mask) { w(regtok(type, reg) | (uint32_t)mask << 16 | 1u << 20); }
static uint32_t swz(int x, int y, int z, int q) { return (uint32_t)(x | y << 2 | z << 4 | q << 6); }
static void src(int type, int reg, uint32_t sw) { w(regtok(type, reg) | sw << 16); }
static void srcm(int type, int reg, uint32_t sw, int mod) { w(regtok(type, reg) | sw << 16 | (uint32_t)mod << 24); }
static void src_rel(int type, int reg, uint32_t sw, int rel_type, int rel_comp) { w(regtok(type, reg) | sw << 16 | 1u << 13); w(regtok(rel_type, 0) | swz(rel_comp, rel_comp, rel_comp, rel_comp) << 16); }
static void dcl(int usage, int index, int type, int reg) { op(31); w(0x80000000u | (uint32_t)usage | (uint32_t)index << 16); dst(type, reg, 0xF); end(); }
static void dcl_sampler(int reg) { op(31); w(0x80000000u | 2u << 27); dst(10, reg, 0xF); end(); }
static void def(int reg, float a, float b, float c, float d) { op(81); dst(2, reg, 0xF); float f[4] = { a, b, c, d }; uint32_t u[4]; memcpy(u, f, 16); for (int i = 0; i < 4; i++) w(u[i]); end(); }
static void defi(int reg, int a, int b, int c, int d) { op(48); dst(7, reg, 0xF); w((uint32_t)a); w((uint32_t)b); w((uint32_t)c); w((uint32_t)d); end(); }
static void defb(int reg, int v) { op(47); dst(14, reg, 0xF); w((uint32_t)v); end(); }
#define XYZW swz(0,1,2,3)
#define S(x,y,z,q) swz(x,y,z,q)
enum { R = 0, V = 1, C = 2, A0 = 3, T = 3, OPOS = 4, OD = 5, OT = 6, O = 6, I = 7, OC = 8, SMP = 10, B = 14, AL = 15, MISC = 17, LBL = 18 };
enum { MOV = 1, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, SLT, SGE, EXP, LOG, LIT, DST, LRP, FRC, M4x4, M4x3, M3x4, M3x3, M3x2, CALL, CALLNZ,
       LOOP, RET, ENDLOOP, LABEL, DCL, POW, CRS, SGN, ABS, NRM, SINCOS, REP, ENDREP, IF, IFC, ELSE, ENDIF, BREAK, BREAKC, MOVA,
       TEXKILL = 65, TEXLD = 66, CMP = 88, DP2ADD = 90 };

static float cf[256][4]; static int32_t ci[16][4]; static uint32_t cb;
static sm3_env env;
static float vin[16][4], pos[4], var[SM3_VARY][4], color[4];
static void reset_env(void) { memset(cf, 0, sizeof cf); memset(ci, 0, sizeof ci); cb = 0; memset(&env, 0, sizeof env); env.cf = cf; env.cf_n = 256; env.ci = ci; memset(vin, 0, sizeof vin); }
static int feq(float a, float b) { return fabsf(a - b) < 1e-6f; }
static int v4(const float *v, float a, float b, float c, float d) { return feq(v[0], a) && feq(v[1], b) && feq(v[2], c) && feq(v[3], d); }
static sm3_prog *make(char *why, size_t n) { return sm3_prog_new(code, (uint32_t)nw, why, n); }

int main(void) {
    char why[128];
    reset_env();

    /* --- refusals --- */
    begin(0, 1, 4); op(MOV); dst(R, 0, 0xF); src(V, 0, XYZW); end(); finish();
    ok(make(why, sizeof why) == 0 && strstr(why, "ps_1_x"), "ps_1_4 is refused by name");
    begin(1, 1, 1); op(MOV); dst(R, 0, 0xF); src(V, 0, XYZW); end(); finish();
    ok(make(why, sizeof why) == 0 && strstr(why, "vs_1_1"), "vs_1_1 is refused by name");
    begin(1, 2, 0); op(MOV | 1u << 28); dst(R, 0, 0xF); src(V, 0, XYZW); end(); finish();
    ok(make(why, sizeof why) == 0 && strstr(why, "predicated"), "a predicated instruction is refused by name");
    begin(1, 2, 0); op(IF); src(B, 0, XYZW); end(); finish();
    ok(make(why, sizeof why) == 0 && strstr(why, "left open"), "an unclosed if is refused");

    /* --- vs_2_0: mov/add/mul/mad through a swizzle, a mask and a negate --- */
    begin(1, 2, 0);
    dcl(0, 0, V, 0);
    op(MOV); dst(R, 0, 0xF); src(V, 0, S(3, 2, 1, 0)); end();               /* r0 = v0.wzyx */
    op(ADD); dst(R, 1, 0x3); src(R, 0, XYZW); src(C, 0, XYZW); end();      /* r1.xy = r0 + c0 */
    op(MUL); dst(R, 1, 0xC); srcm(R, 0, XYZW, 1); src(C, 0, XYZW); end();  /* r1.zw = -r0 * c0 */
    op(MAD); dst(OPOS, 0, 0xF); src(R, 1, XYZW); src(C, 1, S(0, 0, 0, 0)); src(V, 0, XYZW); end();   /* oPos = r1 * c1.x + v0 */
    op(MOV); dst(OD, 0, 0xF); src(R, 0, XYZW); end();
    op(MOV); dst(OT, 3, 0xF); src(C, 0, XYZW); end();
    finish();
    sm3_prog *p = make(why, sizeof why);
    ok(p != 0, "a vs_2_0 decodes");
    ok(sm3_prog_is_vs(p) && sm3_prog_version(p) == 20, "  and knows it is vs_2_0");
    { const sm3_decl *d; int n = sm3_prog_inputs(p, &d); ok(n == 1 && d[0].reg == 0 && d[0].usage == 0 && d[0].rtype == SM3_RT_INPUT, "  dcl_position v0 is recorded"); }
    vin[0][0] = 1; vin[0][1] = 2; vin[0][2] = 3; vin[0][3] = 4;
    cf[0][0] = 10; cf[0][1] = 20; cf[0][2] = 30; cf[0][3] = 40;
    cf[1][0] = 2;
    ok(sm3_exec_vs(p, &env, vin, pos, var) == 0, "  runs");
    /* r0 = (4,3,2,1); r1.xy = (14, 23); r1.zw = (-2*30, -1*40) = (-60, -40); oPos = r1*2 + v0 = (29, 48, -117, -76) */
    ok(v4(pos, 29, 48, -117, -76), "  swizzle, mask, negate, mad: oPos is right to the bit");
    ok(v4(var[0], 4, 3, 2, 1), "  oD0 arrives in varying slot 0");
    ok(v4(var[5], 10, 20, 30, 40), "  oT3 arrives in varying slot 5");
    sm3_prog_free(p);

    /* --- def/defi/defb, relative addressing through a0 and aL, loop and rep, if/else --- */
    reset_env();
    begin(1, 3, 0);
    dcl(0, 0, V, 0); dcl(0, 0, O, 0); dcl(5, 0, O, 1);
    def(10, 1.0f, 2.0f, 3.0f, 4.0f);
    defi(0, 3, 1, 1, 0);                                   /* loop three times, aL from 1 step 1 */
    defb(0, 1);
    op(MOVA); dst(A0, 0, 0x1); src(C, 10, S(2, 2, 2, 2)); end();      /* a0.x = round(3.0) = 3 */
    op(MOV); dst(R, 0, 0xF); src_rel(C, 20, XYZW, A0, 0); end();       /* r0 = c[20 + a0.x] = c23 */
    op(MOV); dst(R, 1, 0xF); src(C, 10, S(3, 3, 3, 3)); end();         /* r1 = 4 */
    op(LOOP); src(AL, 0, XYZW); src(I, 0, XYZW); end();
    op(ADD); dst(R, 1, 0xF); src(R, 1, XYZW); src_rel(C, 30, XYZW, AL, 0); end();   /* r1 += c[30 + aL] for aL = 1, 2, 3 */
    op(ENDLOOP); end();
    op(REP); src(I, 0, XYZW); end();
    op(ADD); dst(R, 1, 0x1); src(R, 1, XYZW); src(C, 10, S(0, 0, 0, 0)); end();     /* r1.x += 1, three times */
    op(ENDREP); end();
    op(IF); src(B, 0, XYZW); end();
    op(MOV); dst(O, 0, 0xF); src(R, 1, XYZW); end();
    op(ELSE); end();
    op(MOV); dst(O, 0, 0xF); src(R, 0, XYZW); end();
    op(ENDIF); end();
    op(IF); srcm(B, 0, XYZW, 13); end();                               /* if !b0 */
    op(MOV); dst(O, 1, 0xF); src(C, 10, XYZW); end();
    op(ELSE); end();
    op(MOV); dst(O, 1, 0xF); src(R, 0, XYZW); end();
    op(ENDIF); end();
    finish();
    p = make(why, sizeof why);
    ok(p != 0, "a vs_3_0 with loop, rep, if/else and relative addressing decodes");
    sm3_prog_apply_defs(p, cf, 256, ci, &cb); env.cb = cb;
    ok(v4(cf[10], 1, 2, 3, 4) && ci[0][0] == 3 && ci[0][1] == 1 && (cb & 1), "  def, defi and defb land in the device's constants");
    for (int i = 0; i < 4; i++) { cf[23][i] = 100.0f + (float)i; cf[31][i] = 1; cf[32][i] = 10; cf[33][i] = 100; }
    ok(sm3_exec_vs(p, &env, vin, pos, var) == 0, "  runs");
    /* r1 = 4 + 1 + 10 + 100 = 115 each; then r1.x += 3 -> 118 */
    ok(v4(pos, 118, 115, 115, 115), "  loop with aL-relative constants, rep, and the taken branch of an if");
    ok(v4(var[2], 100, 101, 102, 103), "  a0-relative constant read, and the else branch of if !b0");
    sm3_prog_free(p);

    /* --- ifc, breakc, call/ret, m4x4, dp3, dp4, min/max, slt/sge, frc, rcp, rsq, lrp, cmp, abs, sgn, dp2add --- */
    reset_env();
    begin(1, 3, 0);
    dcl(0, 0, V, 0); dcl(0, 0, O, 0); dcl(5, 0, O, 1); dcl(5, 1, O, 2); dcl(10, 0, O, 3);
    defi(0, 10, 0, 1, 0);
    for (int i = 0; i < 4; i++) { cf[i][0] = i == 0; cf[i][1] = i == 1 ? 2 : 0; cf[i][2] = i == 2 ? 3 : 0; cf[i][3] = i == 3 ? 1 : 0; }   /* diag(1,2,3,1) */
    cf[4][0] = 0.5f; cf[4][1] = -2.5f; cf[4][2] = 4.0f; cf[4][3] = 9.0f;
    cf[5][0] = 0; cf[5][1] = 1; cf[5][2] = 2; cf[5][3] = 3;
    op(M4x4); dst(R, 0, 0xF); src(V, 0, XYZW); src(C, 0, XYZW); end();             /* r0 = v0 * diag */
    op(MOV); dst(R, 1, 0xF); src(C, 5, XYZW); end();
    op(LOOP); src(AL, 0, XYZW); src(I, 0, XYZW); end();
    op(ADD); dst(R, 1, 0xF); src(R, 1, XYZW); src(C, 5, S(1, 1, 1, 1)); end();     /* r1 += 1 */
    opc(BREAKC, 3); src(R, 1, S(0, 0, 0, 0)); src(C, 5, S(3, 3, 3, 3)); end();     /* break when r1.x >= 3 */
    op(ENDLOOP); end();
    opc(IFC, 1); src(R, 1, S(0, 0, 0, 0)); src(C, 5, S(2, 2, 2, 2)); end();        /* if r1.x > 2 */
    op(CALL); src(LBL, 0, XYZW); end();
    op(ENDIF); end();
    op(DP3); dst(R, 2, 0x1); src(R, 0, XYZW); src(C, 4, XYZW); end();              /* (1*0.5 + 4*-2.5 + 9*4) = 26.5 */
    op(DP4); dst(R, 2, 0x2); src(R, 0, XYZW); src(C, 4, XYZW); end();              /* 26.5 + 9*9 = 107.5 */
    op(MIN); dst(R, 2, 0x4); src(R, 0, XYZW); src(C, 4, XYZW); end();              /* min(9, 4) = 4 */
    op(MAX); dst(R, 2, 0x8); src(R, 0, XYZW); src(C, 4, XYZW); end();              /* max(4, 9) = 9 */
    op(MOV); dst(O, 0, 0xF); src(R, 1, XYZW); end();
    op(MOV); dst(O, 1, 0xF); src(R, 2, XYZW); end();
    op(SLT); dst(R, 3, 0x1); src(C, 4, XYZW); src(C, 5, XYZW); end();              /* 0.5 < 0 ? 0 */
    op(SGE); dst(R, 3, 0x2); src(C, 4, XYZW); src(C, 5, XYZW); end();              /* -2.5 >= 1 ? 0 */
    op(FRC); dst(R, 3, 0x4); src(C, 4, S(1, 1, 1, 1)); end();                      /* frc(-2.5) = 0.5 */
    op(RCP); dst(R, 3, 0x8); src(C, 4, S(2, 2, 2, 2)); end();                      /* 1/4 */
    op(MOV); dst(O, 2, 0xF); src(R, 3, XYZW); end();
    op(RSQ); dst(R, 4, 0x1); src(C, 4, S(3, 3, 3, 3)); end();                      /* 1/3 */
    op(LRP); dst(R, 4, 0x2); src(C, 4, S(0, 0, 0, 0)); src(C, 4, S(3, 3, 3, 3)); src(C, 4, S(2, 2, 2, 2)); end();   /* 0.5*(9-4)+4 = 6.5 */
    op(CMP); dst(R, 4, 0x4); src(C, 4, S(1, 1, 1, 1)); src(C, 4, S(2, 2, 2, 2)); src(C, 4, S(3, 3, 3, 3)); end();   /* -2.5 >= 0 ? 4 : 9 -> 9 */
    op(DP2ADD); dst(R, 4, 0x8); src(C, 4, XYZW); src(C, 5, XYZW); src(C, 4, S(2, 2, 2, 2)); end();               /* 0.5*0 + -2.5*1 + 4 = 1.5 */
    op(MOV); dst(O, 3, 0xF); src(R, 4, XYZW); end();
    op(RET); end();
    op(LABEL); src(LBL, 0, XYZW); end();
    op(ADD); dst(R, 1, 0xF); src(R, 1, XYZW); src(C, 5, S(3, 3, 3, 3)); end();     /* the subroutine adds 3 */
    op(RET); end();
    finish();
    p = make(why, sizeof why);
    ok(p != 0, "a vs_3_0 with breakc, ifc, call/ret and the arithmetic decodes");
    sm3_prog_apply_defs(p, cf, 256, ci, &cb); env.cb = cb;
    vin[0][0] = 1; vin[0][1] = 2; vin[0][2] = 3; vin[0][3] = 9;
    ok(p && sm3_exec_vs(p, &env, vin, pos, var) == 0, "  runs");
    /* r1 starts (0,1,2,3); the loop adds 1 until r1.x >= 3: three passes -> (3,4,5,6); ifc 3 > 2 calls: +3 -> (6,7,8,9) */
    ok(v4(pos, 6, 7, 8, 9), "  breakc leaves the loop when it should, and call/ret adds through a subroutine");
    ok(v4(var[2], 26.5f, 107.5f, 4, 9), "  m4x4 then dp3, dp4, min, max");
    ok(v4(var[3], 0, 0, 0.5f, 0.25f), "  slt, sge, frc, rcp");
    ok(feq(var[0][0], 1.0f / 3.0f) && v4(var[0] + 1, 6.5f, 9, 1.5f, 0) , "  rsq, lrp, cmp, dp2add");
    sm3_prog_free(p);

    /* --- ps_2_0: texld through a sampler, tint by v0, texkill, saturate --- */
    reset_env();
    static uint32_t px[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0x80808080u };    /* blue, green, red, half grey */
    static d3d11_texture tex = { px, 2, 2, 2 };
    env.tex[0] = &tex; env.wrap[0] = 1; env.linear[0] = 0;
    begin(0, 2, 0);
    dcl(10, 0, V, 0); dcl(5, 0, T, 0); dcl(5, 1, T, 1); dcl_sampler(0);
    op(TEXLD); dst(R, 0, 0xF); src(T, 0, XYZW); src(SMP, 0, XYZW); end();
    op(MUL); dst(R, 0, 0xF); src(R, 0, XYZW); src(V, 0, XYZW); end();
    op(TEXKILL); dst(T, 1, 0xF); end();
    op(ADD); dst_sat(OC, 0, 0xF); src(R, 0, XYZW); src(R, 0, XYZW); end();          /* saturate(2 * tinted) */
    finish();
    p = make(why, sizeof why);
    ok(p != 0 && !sm3_prog_is_vs(p) && sm3_prog_version(p) == 20, "a ps_2_0 with texld and texkill decodes");
    ok(sm3_prog_samplers(p) == 1, "  dcl_2d s0 is recorded");
    memset(var, 0, sizeof var);
    var[0][0] = 0.25f; var[0][1] = 1.0f; var[0][2] = 1.0f; var[0][3] = 0.5f;      /* v0: tint */
    var[2][0] = 0.75f; var[2][1] = 0.25f;                                          /* t0: the top-right texel (green) */
    var[3][0] = 1.0f; var[3][1] = 1.0f; var[3][2] = 1.0f;                          /* t1: not killed */
    float vpos[4] = { 0.5f, 0.5f, 0, 1 };
    int r = sm3_exec_ps(p, &env, var, vpos, color);
    ok(r == 0, "  runs and keeps the pixel");
    ok(v4(color, 0, 1, 0, 1), "  texel green, tinted, doubled and saturated: (0, 1, 0, 1)");
    var[3][1] = -0.001f;
    r = sm3_exec_ps(p, &env, var, vpos, color);
    ok(r == 1, "  texkill on a negative coordinate kills the pixel");
    sm3_prog_free(p);

    /* --- ps_3_0: inputs by dcl, vPos --- */
    reset_env();
    begin(0, 3, 0);
    dcl(5, 2, V, 0); dcl(10, 1, V, 1); dcl(0, 0, MISC, 0);
    op(ADD); dst(R, 0, 0xF); src(V, 0, XYZW); src(V, 1, XYZW); end();
    op(MOV); dst(R, 0, 0x8); src(MISC, 0, S(0, 0, 0, 0)); end();                   /* .w = vPos.x */
    op(MOV); dst(OC, 0, 0xF); src(R, 0, XYZW); end();
    finish();
    p = make(why, sizeof why);
    ok(p != 0 && sm3_prog_version(p) == 30, "a ps_3_0 decodes");
    memset(var, 0, sizeof var);
    var[4][0] = 1; var[4][1] = 2; var[4][2] = 3; var[4][3] = 4;                     /* TEXCOORD2 -> slot 4 */
    var[1][0] = 10; var[1][1] = 20; var[1][2] = 30; var[1][3] = 40;                /* COLOR1 -> slot 1 */
    vpos[0] = 123.5f;
    ok(sm3_exec_ps(p, &env, var, vpos, color) == 0 && v4(color, 11, 22, 33, 123.5f), "  dcl_texcoord2 v0 and dcl_color1 v1 read their slots; vPos reads the position");
    sm3_prog_free(p);

    printf("test_sm3: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
