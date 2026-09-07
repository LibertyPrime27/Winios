/* xc_bench -- how fast does the core actually run, on this machine?
 *
 * Every performance figure in the docs so far comes from qemu-user, where the
 * absolute numbers are meaningless and only ratios say anything. This runs a
 * fixed workload through the interpreter and through the dynarec on whatever
 * CPU it is called on, and reports wall-clock nanoseconds per guest
 * instruction for each. On an iPad or iPhone that is the first real number
 * this project has.
 *
 * Three loops, chosen because they are what a game's hot code is made of:
 *
 *   integer   a dependent ALU chain (add / xor / imul) plus the loop branch
 *   sse2      scalar double arithmetic, the way a 64-bit or /arch:SSE2 build does it
 *   x87       fld / fmul / fadd / fstp against memory in 53-bit precision,
 *             the way a 32-bit MSVC build (Fallout 3, New Vegas) does it
 *
 * The x87 loop deliberately runs with FCW 0x027F, because that is the mode the
 * dynarec lowers onto NEON doubles; at the 8087 default of 64-bit precision it
 * would measure the interpreter callout path instead and say nothing about the
 * fast path. Operand values are chosen to stay finite and normal so no loop
 * spends its time in a guard's slow path -- this measures the fast path, which
 * is the thing worth knowing.
 *
 * The snippets were assembled with GNU as; the source is in the comment above
 * each one, so nobody has to hand-decode ModRM to check them.
 */
#include "xcore/bench.h"
#include "xcore/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ARENA_SZ = 1u << 20, CODE_AT = 0x1000, DATA_AT = 0x2000, STACK_AT = 0x8000 };

static uint64_t now_ns_fwd(void);

/*  add rax, rbx ; xor rdx, rax ; imul rax, rbx ; sub rcx, 1 ; jne ; hlt  */
static const uint8_t BENCH_INT[] = {
    0x48,0x01,0xD8, 0x48,0x31,0xC2, 0x48,0x0F,0xAF,0xC3, 0x48,0x83,0xE9,0x01, 0x75,0xF0, 0xF4 };
/*  addsd xmm0,xmm1 ; mulsd xmm0,xmm2 ; subsd xmm0,xmm1 ; divsd xmm0,xmm2 ; sub rcx,1 ; jne ; hlt  */
static const uint8_t BENCH_SSE[] = {
    0xF2,0x0F,0x58,0xC1, 0xF2,0x0F,0x59,0xC2, 0xF2,0x0F,0x5C,0xC1, 0xF2,0x0F,0x5E,0xC2,
    0x48,0x83,0xE9,0x01, 0x75,0xEA, 0xF4 };
/*  fld qword [rdi] ; fmul qword [rdi] ; fadd st,st(1) ; fstp qword [rsi] ; sub rcx,1 ; jne ; hlt  */
static const uint8_t BENCH_X87[] = {
    0xDD,0x07, 0xDC,0x0F, 0xD8,0xC1, 0xDD,0x1E, 0x48,0x83,0xE9,0x01, 0x75,0xF2, 0xF4 };

typedef struct {
    const char *name;
    const uint8_t *code;
    unsigned len;
    unsigned insns;          /* guest instructions per iteration, the hlt aside */
} bench_case;

static const bench_case CASES[] = {
    { "integer  (add/xor/imul + branch)", BENCH_INT, sizeof BENCH_INT, 5 },
    { "sse2     (add/mul/sub/div sd)",    BENCH_SSE, sizeof BENCH_SSE, 6 },
    { "x87      (fld/fmul/fadd/fstp m64)", BENCH_X87, sizeof BENCH_X87, 6 },
};
#define NCASES ((int)(sizeof CASES / sizeof CASES[0]))

/* A calibration constant: the same shape of work, in plain C, not emulated.
 *
 * Without it the guest figures cannot be compared across machines, because a
 * MIPS number folds together how fast the host is and how good the emulator
 * is. With it they separate. It earned its place immediately: the macOS CI
 * runner interprets at 73 MIPS, an x86 cloud container at 48, and both iOS
 * devices at 9 -- while the same devices run *dynarec* output at 4000 against
 * the runner's 2400. An M3 that is fine at executing JIT code and five times
 * slower than a shared cloud VM at executing C is not a story about core
 * speed, and this line is what will say whether the C around the interpreter
 * is slow on device or whether the interpreter itself is.
 *
 * `volatile` on the sink so the loop cannot be optimised away, and nothing
 * else in it that a compiler can hoist. */
static uint64_t bench_native(void) {
    volatile uint64_t sink = 0;
    uint64_t a = 3, b = 5, d = 0;
    const uint64_t iters = 20000000;
    uint64_t t0 = now_ns_fwd();
    for (uint64_t i = 0; i < iters; i++) {
        a = a + b;
        d = d ^ a;
        a = a * b;
    }
    uint64_t t1 = now_ns_fwd();
    sink = a ^ d;
    (void)sink;
    uint64_t ns = t1 - t0;
    return ns ? iters * 3ull * 1000ull / ns : 0;     /* millions of C operations per second */
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t now_ns_fwd(void) { return now_ns(); }

/* One case, one engine. Returns nanoseconds, or 0 if the guest did not stop
 * at its HLT (which would make the timing meaningless). */
static uint64_t run_one(const bench_case *b, uint8_t *arena, uint64_t iters, int jit, xc_stop *why) {
    memset(arena + CODE_AT, 0xCC, 64);
    memcpy(arena + CODE_AT, b->code, b->len);

    /* [rdi] = 1.0 and [rsi] = 0.0: the x87 loop squares 1.0 and adds ST(1),
     * so every value stays finite, normal and exactly representable. */
    double one = 1.0, zero = 0.0;
    memcpy(arena + DATA_AT, &one, 8);
    memcpy(arena + DATA_AT + 8, &zero, 8);

    xc_mem mem; xc_mem_init_arena(&mem, arena, ARENA_SZ);
    xc_cpu c; xc_cpu_init(&c, XC_MODE_64, &mem);
    c.rip = CODE_AT;
    c.gpr[XC_RSP] = STACK_AT;
    c.gpr[XC_RCX] = iters;
    c.gpr[XC_RAX] = 3; c.gpr[XC_RBX] = 5;
    c.gpr[XC_RDI] = DATA_AT; c.gpr[XC_RSI] = DATA_AT + 8;

    /* xmm0/1/2 = 1.0, 0.0, 1.0 -- add 0, multiply by 1, subtract 0, divide by 1 */
    memcpy(&c.xmm[0].lo, &one, 8);
    memcpy(&c.xmm[1].lo, &zero, 8);
    memcpy(&c.xmm[2].lo, &one, 8);

    /* 53-bit precision, all exceptions masked: the mode Windows runs in and
     * the one the dynarec lowers natively. Two live stack registers. */
    c.fcw = 0x027F;
    c.fsw = (uint16_t)(6 << 11);                 /* TOP = 6, so ST0/ST1 are physical 6 and 7 */
    c.ftag_empty = (uint8_t)~0xC0;
    memcpy(&c.fpr[6], &(xc_f80){ 0x8000000000000000ull, 0x3FFF }, sizeof(xc_f80));   /* 1.0 */
    memcpy(&c.fpr[7], &(xc_f80){ 0, 0 }, sizeof(xc_f80));                            /* 0.0 */

    xc_jit_enable(jit);
    /* A generous step budget: xc_run returns STEPS if it runs out, which the
     * caller sees as a failed case rather than a fast one. */
    uint64_t budget = iters * (b->insns + 2) + 1024;
    uint64_t t0 = now_ns();
    xc_stop st = xc_run(&c, budget);
    uint64_t t1 = now_ns();
    xc_jit_enable(0);

    *why = st;
    if (st != XC_STOP_HLT) return 0;
    return t1 - t0;
}

int xc_bench(char *report, size_t report_len, uint64_t iters) {
    if (!report || report_len == 0) return -1;
    report[0] = 0;
    if (iters == 0) iters = 200000;

    uint8_t *arena = calloc(ARENA_SZ, 1);
    if (!arena) { snprintf(report, report_len, "bench: out of memory\n"); return -1; }

    size_t off = 0;
    int bad = 0;
    int have_jit = xc_jit_available();

    off += (size_t)snprintf(report + off, report_len - off,
        "%llu iterations per case%s\n", (unsigned long long)iters,
        have_jit ? "" : "   (no dynarec on this host: interpreter only)");
    off += (size_t)snprintf(report + off, report_len - off,
        "  native C reference (same work, not emulated): %llu M ops/s\n"
        "      -- compare this across machines before comparing anything below it\n",
        (unsigned long long)bench_native());

    for (int i = 0; i < NCASES && off < report_len; i++) {
        const bench_case *b = &CASES[i];
        uint64_t guest = iters * (uint64_t)b->insns;

        xc_stop why_i = XC_STOP_NONE, why_j = XC_STOP_NONE;
        uint64_t ns_i = run_one(b, arena, iters, 0, &why_i);

        /* The dynarec is fast enough that `iters` finishes in a millisecond or
         * two on an M3 -- too short to measure honestly, and dominated by the
         * one-off cost of compiling the loop. So its pass is calibrated: run
         * once to find the rate, then run again with enough iterations to take
         * about a tenth of a second. Without this the reported figure swings
         * ~10% between runs on the same device for no reason but noise. */
        uint64_t iters_j = iters, ns_j = 0;
        if (have_jit) {
            ns_j = run_one(b, arena, iters, 1, &why_j);
            if (ns_j > 0 && ns_j < 50000000ull) {
                uint64_t scale = 100000000ull / ns_j;
                if (scale > 4096) scale = 4096;
                if (scale > 1) {
                    iters_j = iters * scale;
                    ns_j = run_one(b, arena, iters_j, 1, &why_j);
                }
            }
        }
        uint64_t guest_j = iters_j * (uint64_t)b->insns;

        if (!ns_i) {
            off += (size_t)snprintf(report + off, report_len - off,
                "  %s  FAILED (interpreter stopped: %s)\n", b->name, xc_stop_name(why_i));
            bad++;
            continue;
        }
        if (have_jit && !ns_j) {
            off += (size_t)snprintf(report + off, report_len - off,
                "  %s  FAILED (dynarec stopped: %s)\n", b->name, xc_stop_name(why_j));
            bad++;
            continue;
        }

        /* Millions of guest instructions per second, and ns per instruction. */
        double mips_i = (double)guest * 1000.0 / (double)ns_i;
        if (!have_jit) {
            off += (size_t)snprintf(report + off, report_len - off,
                "  %s\n      interp %6.0f ms  %6.1f MIPS\n",
                b->name, (double)ns_i / 1e6, mips_i);
            continue;
        }
        double mips_j = (double)guest_j * 1000.0 / (double)ns_j;
        off += (size_t)snprintf(report + off, report_len - off,
            "  %s\n      interp %6.0f ms (%5.1f MIPS)   dynarec %6.0f ms (%6.1f MIPS)   %.1fx",
            b->name, (double)ns_i / 1e6, mips_i, (double)ns_j / 1e6, mips_j,
            mips_j / mips_i);
        if (iters_j != iters && off < report_len)
            off += (size_t)snprintf(report + off, report_len - off,
                "   (dynarec ran %llux the iterations, to be measurable)",
                (unsigned long long)(iters_j / iters));
        if (off < report_len) off += (size_t)snprintf(report + off, report_len - off, "\n");
    }

    if (off < report_len) {
        uint64_t blocks = 0, callouts = 0, bytes = 0, x87n = 0, x87c = 0;
        xc_jit_stats(&blocks, &callouts, &bytes);
        xc_jit_x87_stats(&x87n, &x87c);
        off += (size_t)snprintf(report + off, report_len - off,
            "  jit totals: %llu blocks, %llu KB, %llu callouts",
            (unsigned long long)blocks, (unsigned long long)(bytes >> 10),
            (unsigned long long)callouts);
        if (off < report_len && (x87n || x87c))
            off += (size_t)snprintf(report + off, report_len - off,
                "; x87 %llu lowered / %llu called out",
                (unsigned long long)x87n, (unsigned long long)x87c);
        if (off < report_len) off += (size_t)snprintf(report + off, report_len - off, "\n");
    }

    free(arena);
    return bad;
}
