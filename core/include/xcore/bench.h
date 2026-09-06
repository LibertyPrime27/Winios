/* xc_bench -- a fixed workload timed through both engines on the host CPU.
 *
 * The docs' performance table comes from qemu-user, where absolute numbers
 * mean nothing. This is how the device build gets real ones: integer, SSE2 and
 * x87 loops, run through the interpreter and the dynarec, reported as
 * milliseconds and millions of guest instructions per second.
 *
 * The dynarec pass needs executable memory, so on iOS this must be called
 * after xc_jit_set_code() has been pointed at a blessed arena; without one it
 * reports interpreter figures only.
 */
#ifndef XCORE_BENCH_H
#define XCORE_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a human-readable report into `report`. `iters` is the loop count per
 * case (0 picks a default). Returns the number of cases that failed to run,
 * 0 when every figure in the report is real. */
int xc_bench(char *report, size_t report_len, uint64_t iters);

#ifdef __cplusplus
}
#endif

#endif
