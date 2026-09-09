/* Running a Direct3D 9 shader: vs_2_0 to vs_3_0 and ps_2_0 to ps_3_0 bytecode,
 * the token stream D3DXCompileShader and fxc /Tvs_3_0 emit, executed one
 * vertex or one pixel at a time on the CPU. See d3d9_shader.c. */
#ifndef W32_D3D9_SHADER_H
#define W32_D3D9_SHADER_H
#include <stddef.h>
#include <stdint.h>
#include "w32.h"

typedef struct sm3_prog sm3_prog;

/* Decode a program. NULL when it uses something not executed here (ps_1_x,
 * vs_1_1, predication); `why` says what, so the report can name it. */
sm3_prog *sm3_prog_new(const uint32_t *tokens, uint32_t words, char *why, size_t why_len);
void      sm3_prog_free(sm3_prog *p);
int       sm3_prog_is_vs(const sm3_prog *p);         /* 1 vertex, 0 pixel */
int       sm3_prog_version(const sm3_prog *p);       /* major * 10 + minor: 20, 21, 30 */

/* A dcl: which register carries which semantic. Inputs of a vertex shader
 * are what the vertex fetch has to supply; outputs of a vs_3_0 and inputs
 * of a ps_3_0 say how the varyings are wired. */
typedef struct { uint8_t reg, usage, index, rtype; } sm3_decl;   /* rtype: 1 for v#, 3 for a ps_2_x t#, 17 for vPos/vFace */
enum { SM3_RT_INPUT = 1, SM3_RT_TEXTURE = 3, SM3_RT_MISC = 17 };
int      sm3_prog_inputs(const sm3_prog *p, const sm3_decl **d);
int      sm3_prog_outputs(const sm3_prog *p, const sm3_decl **d);
uint32_t sm3_prog_samplers(const sm3_prog *p);          /* bit n: sampler n is declared */

/* The constants a program defines for itself (def, defi, defb), written into
 * the device's constant registers -- which is what SetVertexShader does on
 * real hardware, and why a later SetVertexShaderConstantF still wins. */
void sm3_prog_apply_defs(const sm3_prog *p, float cf[][4], int cf_n, int32_t ci[16][4], uint32_t *cb);

/* Where a semantic travels between the stages: one slot per varying, agreed
 * by both sides. COLOR n is slot n (n < 2), TEXCOORD n is 2 + n (n < 8),
 * NORMAL is 10, anything else 11. -1 for POSITION, which is not a varying. */
enum { SM3_VARY = 12, SM3_REGS = 32 };
int sm3_vary_slot(int usage, int index);

/* What a shader can see beyond its inputs. */
typedef struct {
    const float   (*cf)[4]; int cf_n;                   /* c# */
    const int32_t (*ci)[4];                             /* i#, sixteen */
    uint32_t        cb;                                 /* b#, one bit each */
    const d3d11_texture *tex[16]; int wrap[16], linear[16];   /* s# */
} sm3_env;

/* One vertex: inputs by register, the clip-space position and the varyings
 * out. 0 normally, -1 on a fault (an instruction limit). */
int sm3_exec_vs(const sm3_prog *p, const sm3_env *env, const float v[16][4], float pos[4], float var[SM3_VARY][4]);
/* One pixel: the varyings and the screen position in, the colour out.
 * 0 normally, 1 if the pixel was killed, -1 on a fault. */
int sm3_exec_ps(const sm3_prog *p, const sm3_env *env, const float var[SM3_VARY][4], const float vpos[4], float color[4]);
#endif
