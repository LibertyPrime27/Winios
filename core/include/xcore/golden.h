/* Golden vectors: x86-64 silicon post-states, replayed on any host.
 *
 * On the CI runner we compare the interpreter against the real CPU. On an
 * iPhone there is no x86 to compare against -- so difftest records what
 * silicon did, and the device replays it. A divergence here means the
 * interpreter behaves differently on ARM64 than on x86, which is exactly the
 * class of bug (host-endianness, shift-count UB, signed-overflow UB) that
 * would otherwise surface as a game misbehaving on device only.
 */
#ifndef XCORE_GOLDEN_H
#define XCORE_GOLDEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char    *name;
    uint64_t       seed;
    uint64_t       in_gpr[16];
    uint64_t       in_flags;
    uint64_t       out_gpr[16];
    uint64_t       out_flags;
    uint64_t       flag_mask;
    const uint8_t *code;
    size_t         len;
} golden_vec;

extern const golden_vec xc_golden[];
extern const unsigned   xc_golden_count;

/* Replay every vector through the interpreter.
 * Writes a one-line summary and up to `max_report` diffs into `report`.
 * Returns the number of failures (0 == the core matches silicon here). */
int xc_selftest(char *report, size_t report_len, int max_report);

#ifdef __cplusplus
}
#endif
#endif
