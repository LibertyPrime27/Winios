/* Block cache internals shared with the dynarec (core/src/jit/jit.c). */
#ifndef XCORE_CACHE_H
#define XCORE_CACHE_H

#include "xcore/cpu.h"
#include "xop.h"
#include <Zydis/Zydis.h>

typedef struct {
    ZydisDecodedInstruction in;
    uint64_t rip;
    uint32_t op_first;          /* index into the operand pool */
} dinsn;

typedef struct {
    uint64_t rip;               /* 0 = empty slot, ~0 = tombstone */
    uint32_t first, count;      /* into the instruction pool */
    uint32_t bytes;             /* into the code-image pool */
    uint16_t len;               /* its length */
    uint8_t  mode;
    void    *code;              /* compiled native code, or NULL */
} block;

enum { MAX_BLOCK = 64 };

/* Find (building if needed) the block at c->rip, after checking its code
 * bytes are unchanged. NULL with c->stop set if the first instruction cannot
 * be fetched or decoded. */
block *xc_cache_lookup(xc_cpu *c);
const dinsn *xc_cache_insns(const block *b);
const xop *xc_cache_ops(const dinsn *d);

int xc_decode_at(xc_cpu *c, uint64_t rip, ZydisDecodedInstruction *in, ZydisDecodedOperand *ops);
xc_stop xc_exec_decoded(xc_cpu *c, const ZydisDecodedInstruction *in_, const xop *ops);

/* dynarec entry points (jit.c) */
xc_stop xc_run_jit(xc_cpu *c, uint64_t max_steps);
void xc_jit_code_reset(void);       /* the cache was flushed: all block->code pointers are gone */

#endif
