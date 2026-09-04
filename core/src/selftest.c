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

#include <math.h>
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
#define STACK_AT 0x80000u

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

        /* Both modes replay over the arena: 64-bit vectors exercise the
         * arena as a bounds-checked window, 32-bit ones exercise it as the
         * real thing -- a nonzero base under 32-bit guest addresses, which is
         * how every 32-bit game will run on iOS. */
        xc_mem mem; xc_mem_init_arena(&mem, arena, ARENA_SZ);
        xc_cpu c;   xc_cpu_init(&c, v->mode == 32 ? XC_MODE_32 : XC_MODE_64, &mem);
        /* Inputs verbatim: these cases never dereference, and several of them
         * (sub sil,dil) compute on the very registers a rewrite would clobber.
         * The arena mapping means a stray access faults instead of reading
         * host memory. */
        memcpy(c.gpr, v->in_gpr, sizeof c.gpr);
        /* RSP is recorded as zero: on the capture machine it was the
         * trampoline's own stack pointer, which is meaningless here. These
         * snippets provably never touch memory, so any valid value does. */
        c.gpr[XC_RSP] = STACK_AT;
        if (v->in_xmm) memcpy(c.xmm, v->in_xmm, sizeof c.xmm);
        if (v->in_x87) {
            c.fcw = (uint16_t)v->in_x87[0]; c.fsw = (uint16_t)v->in_x87[1]; c.ftag_empty = (uint8_t)~v->in_x87[2];
            int top = (c.fsw >> 11) & 7;
            for (int r = 0; r < 8; r++) { c.fpr[(top + r) & 7].mant = v->in_x87[3 + 2 * r]; c.fpr[(top + r) & 7].se = (uint16_t)v->in_x87[4 + 2 * r]; }
        }
        c.rflags = v->in_flags | 0x2;   /* recorded masked; bit 1 is always set */
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
            for (int r = 0; r < (v->mode == 32 ? 8 : 16); r++) {
                if (r == XC_RSP) continue;          /* not recorded; see above */
                uint64_t got = v->mode == 32 ? (c.gpr[r] & 0xFFFFFFFFull) : c.gpr[r];
                if (got != v->out_gpr[r]) {
                    if (reported < max_report && off + 96 < report_len)
                        off += (size_t)snprintf(report + off, report_len - off,
                            "  %s: %s x86=%llx arm=%llx\n", v->name, rn[r],
                            (unsigned long long)v->out_gpr[r], (unsigned long long)got);
                    bad = 1;
                }
            }
            if (v->out_xmm) for (int r = 0; r < 16; r++) {
                if (c.xmm[r].lo != v->out_xmm[2 * r] || c.xmm[r].hi != v->out_xmm[2 * r + 1]) {
                    if (reported < max_report && off + 128 < report_len)
                        off += (size_t)snprintf(report + off, report_len - off,
                            "  %s: xmm%d x86=%llx_%llx arm=%llx_%llx\n", v->name, r,
                            (unsigned long long)v->out_xmm[2 * r + 1], (unsigned long long)v->out_xmm[2 * r],
                            (unsigned long long)c.xmm[r].hi, (unsigned long long)c.xmm[r].lo);
                    bad = 1;
                }
            }
            if (v->out_x87) {
                const uint64_t *o = v->out_x87;
                int top = (c.fsw >> 11) & 7;
                if (c.fcw != o[0] || (c.fsw & v->fsw_mask) != (o[1] & v->fsw_mask) || (uint8_t)~c.ftag_empty != o[2]) {
                    if (reported < max_report && off + 128 < report_len)
                        off += (size_t)snprintf(report + off, report_len - off,
                            "  %s: x87 fcw/fsw/ftw x86=%llx/%llx/%llx arm=%x/%x/%x\n", v->name,
                            (unsigned long long)o[0], (unsigned long long)(o[1] & v->fsw_mask), (unsigned long long)o[2],
                            c.fcw, c.fsw & v->fsw_mask, (uint8_t)~c.ftag_empty);
                    bad = 1;
                } else for (int r = 0; r < 8; r++) {
                    int p = (top + r) & 7;
                    if (c.ftag_empty & (1u << p)) continue;
                    uint64_t em = c.fpr[p].mant, xm = o[3 + 2 * r]; unsigned es = c.fpr[p].se, xsx = (unsigned)o[4 + 2 * r];
                    int same = em == xm && es == xsx;
                    if (!same && v->fuzzy) {
                        /* transcendental: recorded to 32 significant bits; compare loosely */
                        double a = (double)ldexp((double)(xm >> 11), (int)(xsx & 0x7FFF) - 16383 - 52), b = (double)ldexp((double)(em >> 11), (int)(es & 0x7FFF) - 16383 - 52);
                        if ((xsx ^ es) & 0x8000) same = 0; else same = fabs(a - b) <= 1e-9 * fabs(a) + 1e-300;   /* recorded to 32 bits */
                    }
                    if (!same) {
                        if (reported < max_report && off + 128 < report_len)
                            off += (size_t)snprintf(report + off, report_len - off,
                                "  %s: st(%d) x86=%04x_%016llx arm=%04x_%016llx\n", v->name, r, xsx, (unsigned long long)xm, es, (unsigned long long)em);
                        bad = 1;
                    }
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
