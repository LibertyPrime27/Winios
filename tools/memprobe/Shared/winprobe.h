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

/* The last frame the guest presented through d3d9, as X8R8G8B8 rows (B,G,R,X
 * in memory), owned here and valid until the next win_probe_run. NULL if the
 * guest presented nothing. Only safe once the run has finished. */
const void *win_probe_frame(int *width, int *height, int *pitch);

/* --- watching a guest that is still running ---
 *
 * A guest drawing frames runs on its own thread inside win_probe_run, while
 * the display refreshes on another. Rather than calling back into the UI from
 * the guest's thread, every presented frame is copied under a lock and given
 * a sequence number; the display side asks for anything newer than what it
 * last drew. That keeps the guest free-running and the frame rate honest --
 * frames the display never got to are dropped, not waited for. */

/* Copy the newest frame into `dst` if its sequence is past *seq (which is
 * then updated). Returns 1 if a frame was copied, 0 if there is nothing
 * newer or `dst` is too small for it. */
int win_probe_copy_frame(uint64_t *seq, void *dst, size_t dst_len,
                         int *width, int *height, int *pitch);

/* Ask a presenting guest to stop: its next Present returns D3DERR_DEVICELOST.
 * Cleared automatically when the next run starts. */
void win_probe_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif
