/* Is JIT actually available on this device, right now?
 *
 * The rule from docs/JIT-DESIGN.md: never trust the CS_DEBUGGED flag alone.
 * Since iOS 18.4 a process can carry that flag and still be refused executable
 * pages, which is how several emulators broke silently on iOS 26. The only
 * honest answer comes from writing an instruction and executing it.
 */
#ifndef JITPROBE_H
#define JITPROBE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JIT_UNKNOWN = 0,
    JIT_ABSENT,        /* not debugged; no enabler has attached */
    JIT_FLAG_ONLY,     /* CS_DEBUGGED set but pages would not execute */
    JIT_WORKING,       /* wrote an instruction and ran it */
    JIT_CRASHED,       /* a previous attempt died -- see the note below */
} jit_state;

typedef struct {
    jit_state state;
    int       cs_debugged;      /* raw csops result */
    uint32_t  cs_flags;
    int       remap_ok;         /* the RW/RX alias pair was created */
    char      detail[192];
} jit_result;

/* Runs the full probe. `marker_path` is a file used to detect a probe that
 * crashed on a previous launch: it is created before the risky execute and
 * removed after. Pass NULL to skip that protection. */
void jit_probe(jit_result *out, const char *marker_path);

const char *jit_state_name(jit_state s);

#ifdef __cplusplus
}
#endif
#endif
