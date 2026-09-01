/* Replay x86-64 golden vectors through the interpreter on any host.
 *
 * The vectors carry the exact register and flag state a real CPU produced.
 * Running them on an iPhone answers a question the CI runner cannot: does the
 * interpreter behave the same on ARM64 as it does on x86? Endianness, shift
 * counts at the width boundary, and signed-overflow assumptions all differ in
 * how they misbehave across compilers and architectures, and every one of them
 * would otherwise show up as a game breaking on device only.
 */
#include "xcore/golden.h"
#include "xcore/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cases that touched memory are skipped: their recorded addresses are host
 * pointers from the CI machine and mean nothing here. Whether a case touched
 * memory is recorded at capture time, not guessed from register values -- an
 * earlier version inferred it from "does RSP look like a host pointer?", which
 * was true of every vector and silently skipped all of them. The remaining
 * register-only cases are replayed with their inputs exactly as recorded.
 * Memory behaviour stays covered by difftest on CI and test_arena32 here. */

#define ARENA_SZ (1u << 20)
#define CODE_AT  0x1000u

int xc_selftest(char *report, size_t report_len, int max_report) {
    uint8_t *arena = calloc(ARENA_SZ, 1);
    if (!arena) {
        snprintf(report, report_len, "selftest: out of memory\n");
        return -1;
    }

    static const char *rn[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                 "r8","r9","r10","r11","r12","r13","r14","r15"};
    unsigned ran = 0, skipped = 0;
    int failures = 0, reported = 0;
    size_t off = 0;
    report[0] = 0;

    for (unsigned i = 0; i < xc_golden_count; i++) {
        const golden_vec *v = &xc_golden[i];
        if (v->touches_mem) { skipped++; continue; }

        memset(arena + CODE_AT, 0xCC, 64);
        memcpy(arena + CODE_AT, v->code, v->len);

        xc_mem mem; xc_mem_init_arena(&mem, arena, ARENA_SZ);
        xc_cpu c;   xc_cpu_init(&c, XC_MODE_64, &mem);
        /* Inputs verbatim: these cases never dereference, and several of them
         * (sub sil,dil) compute on the very registers a rewrite would clobber.
         * The arena mapping means a stray access faults instead of reading
         * host memory. */
        memcpy(c.gpr, v->in_gpr, sizeof c.gpr);
        c.rflags = v->in_flags;
        c.rip = CODE_AT;

        int steps = 0; xc_stop st = XC_STOP_NONE;
        while (c.rip != CODE_AT + v->len && steps++ < 10000) {
            st = xc_step(&c);
            if (st != XC_STOP_NONE) break;
        }
        ran++;

        int bad = 0;
        if (c.rip != CODE_AT + v->len) {
            if (reported < max_report)
                off += (size_t)snprintf(report + off, report_len - off,
                    "  %s: stopped %s\n", v->name, xc_stop_name(st));
            bad = 1;
        } else {
            for (int r = 0; r < 16; r++) {
                if (c.gpr[r] != v->out_gpr[r]) {
                    if (reported < max_report && off + 96 < report_len)
                        off += (size_t)snprintf(report + off, report_len - off,
                            "  %s: %s x86=%llx arm=%llx\n", v->name, rn[r],
                            (unsigned long long)v->out_gpr[r], (unsigned long long)c.gpr[r]);
                    bad = 1;
                }
            }
            if ((c.rflags & v->flag_mask) != (v->out_flags & v->flag_mask)) {
                if (reported < max_report && off + 96 < report_len)
                    off += (size_t)snprintf(report + off, report_len - off,
                        "  %s: flags x86=%llx arm=%llx\n", v->name,
                        (unsigned long long)(v->out_flags & v->flag_mask),
                        (unsigned long long)(c.rflags & v->flag_mask));
                bad = 1;
            }
        }
        if (bad) { failures++; reported++; }
    }

    snprintf(report + off, report_len - off,
             "%s  %u vectors replayed, %u skipped (memory cases), %d mismatched\n",
             failures ? "" : "  ", ran, skipped, failures);

    free(arena);
    return failures;
}
