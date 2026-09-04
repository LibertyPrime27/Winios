/* Decoded-block cache: xc_run without re-decoding every instruction.
 *
 * Decoding is most of what an interpreter step costs -- Zydis has to walk
 * prefixes, opcode tables and ModRM for every instruction, every time. Code
 * does not change between executions (almost -- see below), so each basic
 * block is decoded once and kept: a run of instructions from a start address
 * up to and including the first one that can leave the straight line. xc_run
 * looks the block up by address, then executes its instructions through
 * xc_exec_decoded until one of them sends RIP somewhere other than the next
 * instruction. This is the same block structure the dynarec compiles from
 * later; the two share the lookup, the invalidation, and the test suite.
 *
 * Self-modifying code (DRM unpackers, some old engines): every instruction
 * keeps a copy of its bytes and is compared against memory before it runs.
 * A mismatch drops the block and decodes afresh. Fifteen bytes of memcmp is
 * cheap next to a decode, and it makes the cache safe without write
 * tracking; the dynarec will need real write tracking, and this is the
 * reference it is checked against.
 *
 * One cache, single core, no locking. When it fills up it is flushed whole:
 * simple, and the working set of a game's hot code is far smaller than the
 * pools below.
 */
#include "cache.h"

#include <stdlib.h>
#include <string.h>

enum {
    MAX_INSNS  = 1u << 18,      /* 262144 decoded instructions (~110 MB with operands) */
    MAX_OPS    = MAX_INSNS * 3,
    MAX_BYTES  = MAX_INSNS * 5, /* code images; x86 averages under 5 bytes an instruction */
    HASH_BITS  = 16,
    HASH_SIZE  = 1u << HASH_BITS,
};

static dinsn *g_insns; static uint32_t g_ninsns;
static xop *g_ops; static uint32_t g_nops;
static uint8_t *g_bytes; static uint32_t g_nbytes;
static block *g_blocks;         /* open-addressed by rip */
static uint32_t g_nblocks;
static uint64_t g_stat_hits, g_stat_builds, g_stat_flushes, g_stat_smc;

static void cache_init(void) {
    if (g_insns) return;
    g_insns = malloc(sizeof(dinsn) * MAX_INSNS);
    g_ops = malloc(sizeof(xop) * MAX_OPS);
    g_bytes = malloc(MAX_BYTES);
    g_blocks = calloc(HASH_SIZE, sizeof(block));
}

void xc_cache_flush(void) {
    if (!g_blocks) return;
    memset(g_blocks, 0, sizeof(block) * HASH_SIZE);
    g_ninsns = g_nops = g_nbytes = g_nblocks = 0;
    g_stat_flushes++;
    xc_jit_code_reset();
}

/* Blocks whose code lies in [lo, hi) are dropped (a hole in the hash table
 * needs a rehash of what follows it, so entries are removed by marking the
 * address invalid and letting lookups miss). */
void xc_cache_invalidate(uint64_t lo, uint64_t hi) {
    if (!g_blocks) return;
    for (uint32_t i = 0; i < HASH_SIZE; i++) {
        block *b = &g_blocks[i];
        if (!b->rip) continue;
        if (b->rip < hi && b->rip + b->len > lo) b->rip = ~0ull;   /* tombstone: never matches a real address */
    }
}

static inline uint32_t hash_rip(uint64_t rip) {
    uint64_t h = rip * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> (64 - HASH_BITS));
}

static int ends_block(const ZydisDecodedInstruction *in) {
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_RET: case ZYDIS_CATEGORY_SYSCALL: case ZYDIS_CATEGORY_SYSRET:
    case ZYDIS_CATEGORY_INTERRUPT:
        return 1;
    default:
        return in->mnemonic == ZYDIS_MNEMONIC_HLT || in->mnemonic == ZYDIS_MNEMONIC_UD2 ||
               in->mnemonic == ZYDIS_MNEMONIC_SYSENTER;
    }
}

/* Decode a block starting at rip. Returns NULL (with c->stop set) only if the
 * very first instruction cannot be fetched or decoded. */
static block *build(xc_cpu *c, uint64_t rip) {
    if (g_nblocks >= HASH_SIZE * 3 / 4 || g_ninsns + MAX_BLOCK > MAX_INSNS ||
        g_nops + MAX_BLOCK * ZYDIS_MAX_OPERAND_COUNT > MAX_OPS || g_nbytes + MAX_BLOCK * 15 > MAX_BYTES)
        xc_cache_flush();
    uint32_t first = g_ninsns, count = 0, bytes = g_nbytes;
    uint64_t at = rip;
    for (; count < MAX_BLOCK; count++) {
        dinsn *d = &g_insns[g_ninsns];
        ZydisDecodedOperand tmp[ZYDIS_MAX_OPERAND_COUNT];
        if (!xc_decode_at(c, at, &d->in, tmp)) {
            if (count == 0) return 0;               /* c->stop says why */
            c->stop = XC_STOP_NONE;                 /* the block just ends here; the next lookup reports it */
            break;
        }
        d->rip = at;
        d->op_first = g_nops;
        xop_convert(&d->in, tmp, at, c->mode, g_ops + g_nops);
        g_nops += d->in.operand_count;
        memcpy(g_bytes + g_nbytes, xc_mem_ptr(c->mem, at, d->in.length), d->in.length);
        g_nbytes += d->in.length;
        g_ninsns++;
        at += d->in.length;
        if (ends_block(&d->in)) { count++; break; }
    }
    uint32_t h = hash_rip(rip);
    while (g_blocks[h].rip && g_blocks[h].rip != rip) h = (h + 1) & (HASH_SIZE - 1);
    block *b = &g_blocks[h];
    if (!b->rip) g_nblocks++;
    b->rip = rip; b->first = first; b->count = count; b->mode = (uint8_t)c->mode;
    b->bytes = bytes; b->len = (uint16_t)(at - rip); b->code = 0;
    g_stat_builds++;
    return b;
}

static block *lookup(xc_cpu *c, uint64_t rip) {
    uint32_t h = hash_rip(rip);
    for (;;) {
        block *b = &g_blocks[h];
        if (!b->rip) return build(c, rip);
        if (b->rip == rip && b->mode == (uint8_t)c->mode) { g_stat_hits++; return b; }
        h = (h + 1) & (HASH_SIZE - 1);
    }
}

/* Lookup plus the self-modifying-code check, once per block execution: the
 * bytes we decoded must still be there. A block is contiguous, so this is
 * one memcmp; a mismatch tombstones the block and rebuilds it. */
block *xc_cache_lookup(xc_cpu *c) {
    cache_init();
    for (;;) {
        block *b = lookup(c, c->rip);
        if (!b) return 0;
        const void *now = xc_mem_ptr(c->mem, b->rip, b->len);
        if (now && !memcmp(now, g_bytes + b->bytes, b->len)) return b;
        g_stat_smc++;
        b->rip = ~0ull;
    }
}
const dinsn *xc_cache_insns(const block *b) { return &g_insns[b->first]; }
const xop *xc_cache_ops(const dinsn *d) { return g_ops + d->op_first; }

xc_stop xc_run(xc_cpu *c, uint64_t max_steps) {
    if (xc_jit_enabled()) return xc_run_jit(c, max_steps);
    cache_init();
    while (max_steps) {
        block *b = xc_cache_lookup(c);
        if (!b) return c->stop;
        const dinsn *d = &g_insns[b->first], *end = d + b->count;
        for (; d < end; d++) {
            xc_stop st = xc_exec_decoded(c, &d->in, g_ops + d->op_first);
            max_steps--;
            if (st != XC_STOP_NONE) return st;
            if (c->rip != d->rip + d->in.length) break;      /* left the straight line */
            if (max_steps == 0) break;
        }
    }
    c->stop = XC_STOP_STEPS;
    return XC_STOP_STEPS;
}

void xc_cache_stats(uint64_t *hits, uint64_t *builds, uint64_t *flushes, uint64_t *smc) {
    if (hits) *hits = g_stat_hits;
    if (builds) *builds = g_stat_builds;
    if (flushes) *flushes = g_stat_flushes;
    if (smc) *smc = g_stat_smc;
}
