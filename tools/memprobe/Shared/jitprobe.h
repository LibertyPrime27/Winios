/* Is JIT actually available on this device, right now?
 *
 * The rule from docs/JIT-DESIGN.md: never trust the CS_DEBUGGED flag alone.
 * Since iOS 18.4 a process can carry that flag and still be refused executable
 * pages, which is how several emulators broke silently on iOS 26.
 *
 * The probe is split in two on purpose. Everything up to the moment of
 * execution is safe and tells us most of what we need; only the final jump can
 * fault, and a fault that reports nothing is worse than no probe at all. So the
 * safe half runs first and records how far it got, and the execute half is
 * opt-in and leaves a breadcrumb that survives the process dying.
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
    JIT_FLAG_ONLY,     /* CS_DEBUGGED set, but the RX mapping was refused */
    JIT_MAPPED,        /* RW/RX alias pair exists; execution not yet attempted */
    JIT_WORKING,       /* wrote an instruction and ran it */
    JIT_CRASHED,       /* a previous execute attempt did not return */
} jit_state;

typedef struct {
    jit_state state;
    int       cs_debugged;
    uint32_t  cs_flags;
    int       remap_kr;         /* mach_vm_remap result */
    int       protect_kr;       /* vm_protect result */
    char      last_step[64];    /* how far the previous attempt got */
    char      detail[256];
} jit_result;

/* Safe: csops, the RW/RX alias pair, and the RX protection change. Never jumps.
 * Also reports the breadcrumb from any previous execute attempt. */
void jit_probe_safe(jit_result *out, const char *marker_path);

/* Risky: repeats the mapping and executes an instruction from it. Writes a
 * breadcrumb before each step so a fault is diagnosable on the next launch. */
void jit_probe_execute(jit_result *out, const char *marker_path);

/* Clears the breadcrumb so the execute probe can be retried. Deleting a file
 * inside an app container is not something a user can do on iOS, so this has
 * to be reachable from the UI. */
void jit_probe_reset(const char *marker_path);

const char *jit_state_name(jit_state s);

#ifdef __cplusplus
}
#endif
#endif
