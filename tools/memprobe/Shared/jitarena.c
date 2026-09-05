#include "jitarena.h"

#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000u

extern kern_return_t mach_vm_remap(vm_map_t, mach_vm_address_t *, mach_vm_size_t,
                                   mach_vm_offset_t, int, vm_map_t, mach_vm_address_t,
                                   boolean_t, vm_prot_t *, vm_prot_t *, vm_inherit_t);

/* Implemented in jit26_stubs.S. Fatal if no script-capable debugger is attached. */
extern void  jit26_detach(void);
extern void *jit26_prepare_region(void *addr, size_t len);

/* TXM pages are 16 KB, which is also the bless granularity. */
#define TXM_PAGE 16384u

static void crumb(const char *path, const char *step) {
    if (!path || !*path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(step, f);
    fflush(f);
    fclose(f);
}

static int is_debugged(uint32_t *flags_out) {
    uint32_t flags = 0;
    int rc = csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags);
    if (flags_out) *flags_out = flags;
    return rc == 0 && (flags & CS_DEBUGGED) != 0;
}

/* The debugger session is single-use: the create below ends with a detach,
 * and any brk after that is unserviced and fatal (SIGTRAP, "brk 61453" in the
 * crash log). So the process gets exactly one bless attempt, enforced here
 * where the brk lives rather than trusted to every caller. */
static int g_session_used;
static jit_arena g_shared;
static int g_shared_ok;

int jit_arena_create(jit_arena *a, size_t size, jit_result *r, const char *marker_path) {
    memset(a, 0, sizeof *a);
    memset(r, 0, sizeof *r);

    if (g_session_used) {
        is_debugged(&r->cs_flags);
        r->cs_debugged = 1;
        r->state = g_shared_ok ? JIT_MAPPED : JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail,
                 "The debugger session was already used (and detached) by an earlier "
                 "arena in this launch; a second bless would trap. Reuse the shared "
                 "arena (jit_arena_shared) or relaunch.");
        return 0;
    }

    if (!is_debugged(&r->cs_flags)) {
        r->state = JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail,
                 "CS_DEBUGGED is clear (flags=0x%08x). Attach StikDebug with the "
                 "universal script first -- calling the bless breakpoint without a "
                 "script attached would terminate the app.", r->cs_flags);
        return 0;
    }
    r->cs_debugged = 1;

    size = (size + TXM_PAGE - 1) & ~((size_t)TXM_PAGE - 1);

    /* Allocate R-X directly. The writable view comes later as a separate
     * mapping, so no virtual address is ever both writable and executable. */
    void *rx = mmap(NULL, size, PROT_READ | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rx == MAP_FAILED) {
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail, "mmap(PROT_READ|PROT_EXEC, %zu) failed", size);
        return 0;
    }

    /* The step that was missing. TXM refuses the instruction fetch on pages no
     * debugger has touched -- remap and vm_protect both succeed regardless,
     * which is why their success says nothing about whether code will run. */
    crumb(marker_path, "bless (brk #0xf00d, x16=1)");
    g_session_used = 1;                  /* from here on, no second attempt */
    void *got = jit26_prepare_region(rx, size);
    if (got && got != rx) rx = got;      /* the script may relocate it */

    crumb(marker_path, "remap RW alias");
    mach_vm_address_t rw = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &rw, size, 0,
                                     VM_FLAGS_ANYWHERE, mach_task_self(),
                                     (mach_vm_address_t)rx, FALSE,
                                     &cur, &max, VM_INHERIT_NONE);
    r->remap_kr = (int)kr;
    if (kr != KERN_SUCCESS) {
        munmap(rx, size);
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail, "mach_vm_remap failed (kr=%d)", kr);
        return 0;
    }

    kr = vm_protect(mach_task_self(), (vm_address_t)rw, size, FALSE,
                    VM_PROT_READ | VM_PROT_WRITE);
    r->protect_kr = (int)kr;
    if (kr != KERN_SUCCESS) {
        munmap(rx, size);
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail,
                 "vm_protect(RW) on the alias failed (kr=%d, max=0x%x)", kr, max);
        return 0;
    }

    /* Detach last. After this no further brk is safe, and no new region can be
     * blessed without a fresh attach -- hence one big arena rather than many. */
    crumb(marker_path, "detach (brk #0xf00d, x16=0)");
    jit26_detach();

    if (marker_path && *marker_path) remove(marker_path);

    a->rx = rx;
    a->rw = (void *)(uintptr_t)rw;
    a->size = size;
    a->blessed = 1;

    r->state = JIT_MAPPED;
    snprintf(r->detail, sizeof r->detail,
             "Arena ready: %zu KB blessed, RX %p / RW %p, debugger detached. "
             "Execution should now be permitted.", size / 1024, a->rx, a->rw);
    return 1;
}

int jit_arena_run(jit_arena *a, const void *code, size_t len, jit_result *r,
                  const char *marker_path) {
    if (!a->blessed || len > a->size) {
        r->state = JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail, "no blessed arena to run in");
        return 0;
    }

    memcpy(a->rw, code, len);
    /* Mandatory on arm64: the icache may otherwise hold stale bytes, giving
     * intermittent failures that look like a JIT bug for weeks. */
    sys_icache_invalidate(a->rx, len);

    crumb(marker_path, "execute blessed arena");
    ((void (*)(void))a->rx)();
    if (marker_path && *marker_path) remove(marker_path);

    r->state = JIT_WORKING;
    snprintf(r->detail, sizeof r->detail,
             "Executed %zu bytes from the blessed arena and returned. Dynarec output "
             "will run on this device.", len);
    return 1;
}

void jit_arena_free(jit_arena *a) {
    if (a == &g_shared) return;          /* the shared arena lives until exit */
    if (a->rx) munmap(a->rx, a->size);
    memset(a, 0, sizeof *a);
}

jit_arena *jit_arena_shared(size_t size, jit_result *r, const char *marker_path, int *fresh) {
    if (fresh) *fresh = 0;
    if (g_session_used) {
        memset(r, 0, sizeof *r);
        is_debugged(&r->cs_flags);
        r->cs_debugged = 1;
        if (g_shared_ok) {
            r->state = JIT_MAPPED;
            snprintf(r->detail, sizeof r->detail,
                     "Reusing the arena blessed earlier this launch: %zu KB, RX %p / RW %p.",
                     g_shared.size / 1024, g_shared.rx, g_shared.rw);
            return &g_shared;
        }
        r->state = JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail,
                 "The one bless attempt this launch did not produce an arena; relaunch to retry.");
        return NULL;
    }
    int ok = jit_arena_create(&g_shared, size, r, marker_path);
    if (!ok) return NULL;
    g_shared_ok = 1;
    if (fresh) *fresh = 1;
    return &g_shared;
}
