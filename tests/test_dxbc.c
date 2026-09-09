/* The shader interpreter, instruction by instruction.
 *
 * Token streams are assembled here from the format's own encoding -- the
 * opcode word, the operand words with their masks, swizzles and modifiers --
 * so what is under test is the decoder and the arithmetic, not a compiler.
 * Results are checked to the bit: a shader is arithmetic, and "close" is a
 * frame that differs between two machines. */
#include "dxbc_exec.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ok(int c, const char *what) { checks++; if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } }

/* ---- a tiny assembler for the token format ---- */
static uint32_t code[512]; static int nw;
static void w(uint32_t v) { code[nw++] = v; }
static int op_at;
static void op(uint32_t opcode, int sat) { op_at = nw; w(opcode | (sat ? 1u << 13 : 0)); }
static void op_nz(uint32_t opcode, int test_nz) { op_at = nw; w(opcode | ((uint32_t)test_nz << 18)); }
static void end(void) { code[op_at] |= (uint32_t)(nw - op_at) << 24; }
static uint32_t optok(int type, int ncomp, int mode, int sel, int ndim) { return (uint32_t)ncomp | (uint32_t)mode << 2 | (uint32_t)sel << 4 | (uint32_t)type << 12 | (uint32_t)ndim << 20; }
static uint32_t swz(int x, int y, int z, int q) { return (uint32_t)(x | y << 2 | z << 4 | q << 6); }
/* destination with a mask; sources with a swizzle */
static void dst(int type, int idx, int mask) { w(optok(type, 2, 0, mask, 1)); w((uint32_t)idx); }
static void src(int type, int idx, uint32_t sw) { w(optok(type, 2, 1, (int)sw, 1)); w((uint32_t)idx); }
static void src_neg(int type, int idx, uint32_t sw) { w(optok(type, 2, 1, (int)sw, 1) | 1u << 31); w(1u | 1u << 6); w((uint32_t)idx); }
static void src_cb(int slot, int idx, uint32_t sw) { w(optok(8, 2, 1, (int)sw, 2)); w((uint32_t)slot); w((uint32_t)idx); }
static void src_cb_rel(int slot, int base, int treg, uint32_t sw) { w(optok(8, 2, 1, (int)sw, 2) | 3u << 25); w((uint32_t)slot); w((uint32_t)base); w(optok(0, 1, 0, 0, 1) | 1u << 4); w((uint32_t)treg); }
static void imm4(float a, float b, float c, float d) { w(optok(4, 2, 0, 0, 0)); float f[4] = { a, b, c, d }; uint32_t u[4]; memcpy(u, f, 16); for (int i = 0; i < 4; i++) w(u[i]); }
static void imm1(uint32_t v) { w(optok(4, 1, 0, 0, 0)); w(v); }
static void src1(int type, int idx, int comp) { w(optok(type, 2, 2, comp, 1)); w((uint32_t)idx); }
static void res(int idx) { w(optok(7, 2, 1, (int)swz(0, 1, 2, 3), 1)); w((uint32_t)idx); }
static void smp(int idx) { w(optok(6, 0, 0, 0, 1)); w((uint32_t)idx); }
static void begin(int stage) { nw = 0; w((uint32_t)stage << 16 | 0x40); w(0); }
static const uint32_t *finish(void) { code[1] = (uint32_t)nw; return code; }
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
enum { R = 0, V = 1, O = 2, X = 3 };
enum { XYZW = 0xF, ALL = 0 };
#define S(x,y,z,q) swz(x,y,z,q)

int main(void) {
    char why[128];
    float vin[DXBC_REGS][4], vout[DXBC_REGS][4];
    dxbc_env env; memset(&env, 0, sizeof env);
    static const float cb0[] = { 10, 20, 30, 40, 1, 2, 3, 4, 0.5f, 0.25f, 0.125f, 1.0f };
    env.cb[0] = cb0; env.cb_n[0] = 3;

    /* add o0, v0, cb0[0]; mov o1, v1.zyxw; ret */
    begin(1);
    op(0, 0); dst(O, 0, XYZW); src(V, 0, S(0,1,2,3)); src_cb(0, 0, S(0,1,2,3)); end();
    op(54, 0); dst(O, 1, XYZW); src(V, 1, S(2,1,0,3)); end();
    op(62, 0); end();
    dxbc_prog *p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(p && dxbc_prog_stage(p) == 1, "a vertex shader decodes");
    memset(vin, 0, sizeof vin);
    vin[0][0] = 1; vin[0][1] = 2; vin[0][2] = 3; vin[0][3] = 4;
    vin[1][0] = 7; vin[1][1] = 8; vin[1][2] = 9; vin[1][3] = 6;
    int r = dxbc_exec(p, &env, vin, vout);
    ok(r == 0 && vout[0][0] == 11 && vout[0][1] == 22 && vout[0][2] == 33 && vout[0][3] == 44, "add o0, v0, cb0[0] reads the constant buffer");
    ok(vout[1][0] == 9 && vout[1][1] == 8 && vout[1][2] == 7 && vout[1][3] == 6, "mov with a .zyxw swizzle");
    dxbc_prog_free(p);

    /* masks, negation, saturate, mad, dp4, movc, ftoi/itof */
    begin(0);
    op(54, 0); dst(R, 0, XYZW); imm4(0.5f, -2.0f, 3.0f, 1.0f); end();
    op(50, 1); dst(O, 0, 0x3); src(R, 0, S(0,1,2,3)); imm4(2, 2, 2, 2); imm4(0.25f, 0.25f, 0.25f, 0.25f); end();          /* o0.xy = sat(r0*2+0.25) = (1, 0) */
    op(54, 0); dst(O, 0, 0xC); src_neg(R, 0, S(2,2,2,2)); end();                                                              /* o0.zw = -3 */
    op(17, 0); dst(O, 1, 0x1); src(R, 0, S(0,1,2,3)); src_cb(0, 1, S(0,1,2,3)); end();                                       /* o1.x = dp4 = 0.5-4+9+4 = 9.5 */
    op(55, 0); dst(O, 1, 0x2); imm1(0); imm4(5, 5, 5, 5); imm4(6, 6, 6, 6); end();                                            /* movc with 0 -> 6 */
    op(27, 0); dst(R, 1, XYZW); src(R, 0, S(0,1,2,3)); end();                                                                 /* ftoi: 0,-2,3,1 */
    op(43, 0); dst(O, 1, 0x4); src1(R, 1, 1); end();                                                                          /* itof(r1.y) = -2 */
    op(62, 0); end();
    p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(p && dxbc_prog_stage(p) == 0, "a pixel shader decodes");
    r = dxbc_exec(p, &env, vin, vout);
    ok(r == 0 && vout[0][0] == 1.0f && vout[0][1] == 0.0f, "mad with saturate through an .xy mask");
    ok(vout[0][2] == -3.0f && vout[0][3] == -3.0f, "a negated .zzzz source through a .zw mask");
    ok(vout[1][0] == 9.5f, "dp4 against cb0[1]");
    ok(vout[1][1] == 6.0f, "movc takes the second source when the condition is zero");
    ok(vout[1][2] == -2.0f, "ftoi then itof");
    dxbc_prog_free(p);

    /* control flow: a loop summing 1..5 with a break, an if/else on the result */
    begin(0);
    op(54, 0); dst(R, 0, XYZW); imm4(0, 0, 0, 0); end();            /* r0.x = i (int 0), r0.y = sum (int 0) */
    op(48, 0); end();                                                 /* loop */
    op(30, 0); dst(R, 0, 0x1); src1(R, 0, 0); imm1(1); end();         /*   i++ */
    op(30, 0); dst(R, 0, 0x2); src1(R, 0, 1); src1(R, 0, 0); end();   /*   sum += i */
    op(33, 0); dst(R, 1, 0x1); src1(R, 0, 0); imm1(5); end();         /*   r1.x = i >= 5 */
    op_nz(3, 1); src1(R, 1, 0); end();                                /*   breakc_nz r1.x */
    op(22, 0); end();                                                 /* endloop */
    op(43, 0); dst(O, 0, 0x1); src1(R, 0, 1); end();                  /* o0.x = float(sum) = 15 */
    op(32, 0); dst(R, 1, 0x1); src1(R, 0, 1); imm1(15); end();        /* r1.x = sum == 15 */
    op_nz(31, 1); src1(R, 1, 0); end();                               /* if_nz */
    op(54, 0); dst(O, 0, 0x2); imm4(1, 1, 1, 1); end();               /*   o0.y = 1 */
    op(18, 0); end();                                                 /* else */
    op(54, 0); dst(O, 0, 0x2); imm4(2, 2, 2, 2); end();               /*   o0.y = 2 */
    op(21, 0); end();                                                 /* endif */
    op(62, 0); end();
    p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(p != 0, why[0] ? why : "loop/break/if/else decode");
    r = dxbc_exec(p, &env, vin, vout);
    ok(r == 0 && vout[0][0] == 15.0f, "a loop with breakc sums 1..5");
    ok(vout[0][1] == 1.0f, "if_nz takes the then-branch and skips the else");
    dxbc_prog_free(p);

    /* relative constant-buffer indexing, immediate constant buffer, discard */
    begin(0);
    op(54, 0); dst(R, 0, XYZW); imm1(2); end();                       /* r0 = 2 (int) */
    op(54, 0); dst(O, 0, XYZW); src_cb_rel(0, 0, 0, S(0,1,2,3)); end();   /* o0 = cb0[0 + r0.x] = cb0[2] */
    op_nz(13, 0); imm1(0); end();                                     /* discard_z 0: discards */
    op(62, 0); end();
    p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(p != 0, why[0] ? why : "relative indexing decodes");
    r = dxbc_exec(p, &env, vin, vout);
    ok(vout[0][0] == 0.5f && vout[0][3] == 1.0f, "cb0[r0.x] with a relative index");
    ok(r == 1, "discard_z on zero discards the pixel");
    dxbc_prog_free(p);

    /* sampling: a 2x2 texture, point and bilinear, wrap and clamp */
    static const uint32_t px[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };   /* blue, green, red, white (ARGB) */
    d3d11_texture t = { px, 2, 2, 2 };
    env.tex[0] = &t; env.wrap[0] = 1; env.linear[0] = 0;
    begin(0);
    op(69, 0); dst(O, 0, XYZW); src(V, 0, S(0,1,2,3)); res(0); smp(0); end();
    op(62, 0); end();
    p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(p != 0, why[0] ? why : "sample decodes");
    vin[0][0] = 0.75f; vin[0][1] = 0.25f;                            /* the top-right texel: green */
    dxbc_exec(p, &env, vin, vout);
    ok(vout[0][0] == 0.0f && vout[0][1] == 1.0f && vout[0][2] == 0.0f && vout[0][3] == 1.0f, "point sampling picks the texel (r,g,b,a order)");
    vin[0][0] = 1.75f;                                                 /* wraps to the same texel */
    dxbc_exec(p, &env, vin, vout);
    ok(vout[0][1] == 1.0f && vout[0][0] == 0.0f, "wrap addressing");
    env.wrap[0] = 0; dxbc_exec(p, &env, vin, vout);
    ok(vout[0][1] == 1.0f, "clamp addressing keeps the edge texel");
    env.linear[0] = 1; vin[0][0] = 0.5f; vin[0][1] = 0.5f;           /* the centre: an even mix of all four */
    dxbc_exec(p, &env, vin, vout);
    { int r8 = (int)(vout[0][0] * 255 + 0.5f), g8 = (int)(vout[0][1] * 255 + 0.5f), b8 = (int)(vout[0][2] * 255 + 0.5f);
      char what[96]; snprintf(what, sizeof what, "bilinear at the centre mixes evenly (got %d %d %d, want 128 128 128)", r8, g8, b8);
      ok(r8 == 128 && g8 == 128 && b8 == 128, what); }
    dxbc_prog_free(p);

    /* refusals name what they refuse */
    begin(0); op(76, 0); imm1(0); end(); op(62, 0); end();
    p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(!p && strstr(why, "switch"), "a switch statement is refused by name");
    begin(2); op(62, 0); end();
    p = dxbc_prog_new(finish(), (uint32_t)nw, why, sizeof why);
    ok(!p && strstr(why, "geometry"), "a geometry shader is refused by name");

    printf("test_dxbc: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
