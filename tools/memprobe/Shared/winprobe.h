/* Run a Windows .exe on the device, capture its stdout, time it, and report
 * how much x87 the dynarec lowered natively while it ran. */
#ifndef WINPROBE_H
#define WINPROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the guest's exit code (2 on a setup failure). `arg1`/`arg2` may be
 * NULL. Any of the out-parameters may be NULL. */
int win_probe_run(const char *exe_path, const char *arg1, const char *arg2,
                  char *out, size_t out_len, uint64_t *ns, uint64_t *x87_native,
                  uint64_t *x87_callout);

#ifdef __cplusplus
}
#endif

#endif
