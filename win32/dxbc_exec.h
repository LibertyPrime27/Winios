/* Running a Direct3D 10/11 shader: the tokenised bytecode in a DXBC container's
 * SHDR/SHEX chunk, executed one invocation at a time on the CPU. See dxbc_exec.c. */
#ifndef W32_DXBC_EXEC_H
#define W32_DXBC_EXEC_H
#include <stddef.h>
#include <stdint.h>
#include "w32.h"

typedef struct dxbc_prog dxbc_prog;

/* Decode a program. NULL when it uses something not executed here; `why`
 * says what, so the report can name it. */
dxbc_prog *dxbc_prog_new(const uint32_t *code, uint32_t words, char *why, size_t why_len);
void       dxbc_prog_free(dxbc_prog *p);
int        dxbc_prog_stage(const dxbc_prog *p);      /* 0 pixel, 1 vertex, 2+ other */

enum { DXBC_SLOTS = 4, DXBC_REGS = 32 };
/* What a shader can see beyond its inputs: constant buffers (float4 arrays),
 * textures with their sampler's addressing and filter. */
typedef struct {
    const float         *cb[DXBC_SLOTS]; uint32_t cb_n[DXBC_SLOTS];   /* float4 count */
    const d3d11_texture *tex[DXBC_SLOTS];
    int                  wrap[DXBC_SLOTS], linear[DXBC_SLOTS];
} dxbc_env;

/* One invocation: inputs in v[reg][comp], outputs left in o[reg][comp].
 * Returns 0 normally, 1 if the pixel was discarded, -1 on a fault (an
 * instruction limit, mostly). */
int dxbc_exec(const dxbc_prog *p, const dxbc_env *env, const float v[DXBC_REGS][4], float o[DXBC_REGS][4]);

/* Point or bilinear sample of a BGRA texture at (u, v) in texture space, as
 * 0..1 floats -- integer arithmetic inside, shared with the rasterizer. */
void dxbc_sample(const d3d11_texture *t, int wrap, int linear, float u, float v, float out[4]);
#endif
