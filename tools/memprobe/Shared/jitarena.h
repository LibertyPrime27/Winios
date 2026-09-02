/* An executable arena that actually runs on iOS 26 TXM hardware.
 *
 * This is the allocation the dynarec will eventually emit into, so the API is
 * shaped for that rather than for the one-off test: create one large arena up
 * front, write through `rw`, execute at `rx`.
 *
 * The ordering below is forced by TXM and is not negotiable:
 *
 *   1. ask StikDebug to attach            (URL scheme, from Swift)
 *   2. wait until CS_DEBUGGED appears
 *   3. mmap the RX region
 *   4. bless it -- debugger writes one byte per 16 KB page   <-- the missing step
 *   5. make the RW alias
 *   6. detach
 *   7. only now write code and execute
 *
 * Regions created after the detach can never be blessed by that session, which
 * is why the arena is allocated once and sub-allocated from, and why a JIT that
 * wants more memory later must reconnect.
 */
#ifndef JITARENA_H
#define JITARENA_H

#include <stddef.h>
#include "jitprobe.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void   *rx;        /* execute here  (R-X) */
    void   *rw;        /* write here    (RW-), same physical pages */
    size_t  size;
    int     blessed;   /* the debugger authorised the pages */
} jit_arena;

/* Steps 3-6. Requires CS_DEBUGGED already set and a script-capable debugger
 * still attached. `marker_path` records progress so a fault is diagnosable.
 * Returns 1 on success. */
int jit_arena_create(jit_arena *a, size_t size, jit_result *r, const char *marker_path);

/* Step 7: copy `len` bytes into the arena and run them. The buffer must end in
 * a `ret`. Returns 1 if it returned. */
int jit_arena_run(jit_arena *a, const void *code, size_t len, jit_result *r,
                  const char *marker_path);

void jit_arena_free(jit_arena *a);

#ifdef __cplusplus
}
#endif
#endif
