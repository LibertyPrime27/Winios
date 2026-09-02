#include "jitprobe.h"

#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* csops() is not in a public iOS header. It is the only way to read the
 * process's code-signing status, and it is what every JIT-capable app uses. */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000u

/* mach_vm.h is not shipped in the iOS SDK; the symbol is. */
extern kern_return_t mach_vm_remap(vm_map_t target_task, mach_vm_address_t *target_address,
                                   mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                                   vm_map_t src_task, mach_vm_address_t src_address,
                                   boolean_t copy, vm_prot_t *cur_protection,
                                   vm_prot_t *max_protection, vm_inherit_t inheritance);

const char *jit_state_name(jit_state s) {
    switch (s) {
    case JIT_ABSENT:    return "not available";
    case JIT_FLAG_ONLY: return "flag set, executable mapping refused";
    case JIT_MAPPED:    return "mapping OK, execution untested";
    case JIT_WORKING:   return "working";
    case JIT_CRASHED:   return "previous execute attempt did not return";
    default:            return "unknown";
    }
}

/* Breadcrumbs. Written and flushed before each risky step, so if the process
 * dies the next launch can say which step killed it. */
static void crumb(const char *path, const char *step) {
    if (!path || !*path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(step, f);
    fflush(f);
    fclose(f);
}

static int read_crumb(const char *path, char *out, size_t n) {
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    return 1;
}

void jit_probe_reset(const char *marker_path) {
    if (marker_path && *marker_path) remove(marker_path);
}

/* Reads code-signing status. Always safe. */
static void read_cs(jit_result *r) {
    uint32_t flags = 0;
    int rc = csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags);
    r->cs_flags = flags;
    r->cs_debugged = (rc == 0) && (flags & CS_DEBUGGED) != 0;
}

/* Builds the RW/RX alias pair. Returns 1 on success and hands back both
 * addresses; the caller decides whether to jump into it. */
static int build_alias(jit_result *r, void **rw_out, mach_vm_address_t *rx_out, size_t *len_out) {
    const size_t page = (size_t)getpagesize();

    void *rw = mmap(NULL, page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rw == MAP_FAILED) {
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail, "mmap of the writable alias failed");
        return 0;
    }

    /* One physical page, two virtual addresses. mmap+mprotect(PROT_EXEC) on a
     * single mapping is refused on iOS 18.4+ even with CS_DEBUGGED set, which
     * is why the aliasing is necessary rather than merely tidy. */
    mach_vm_address_t rx = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &rx, page, 0,
                                     VM_FLAGS_ANYWHERE, mach_task_self(),
                                     (mach_vm_address_t)rw, /*copy=*/FALSE,
                                     &cur, &max, VM_INHERIT_NONE);
    r->remap_kr = (int)kr;
    if (kr != KERN_SUCCESS) {
        munmap(rw, page);
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail,
                 "mach_vm_remap failed (kr=%d). The RW/RX alias pair could not be "
                 "created, so unsigned code cannot be executed here.", kr);
        return 0;
    }

    kr = vm_protect(mach_task_self(), (vm_address_t)rx, page, FALSE,
                    VM_PROT_READ | VM_PROT_EXECUTE);
    r->protect_kr = (int)kr;
    if (kr != KERN_SUCCESS) {
        munmap(rw, page);
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail,
                 "vm_protect(RX) failed (kr=%d, max_prot=0x%x). This is the iOS 18.4+ "
                 "behaviour: the flag is set but executable memory is still refused.",
                 kr, max);
        return 0;
    }

    *rw_out = rw; *rx_out = rx; *len_out = page;
    return 1;
}

void jit_probe_safe(jit_result *r, const char *marker_path) {
    memset(r, 0, sizeof *r);
    read_cs(r);

    /* Report any previous fault, but do not let it suppress the safe checks --
     * knowing the mapping still works is exactly what narrows down the cause. */
    char prev[64] = {0};
    int had_crumb = read_crumb(marker_path, prev, sizeof prev);
    if (had_crumb) snprintf(r->last_step, sizeof r->last_step, "%s", prev);

    if (!r->cs_debugged) {
        r->state = JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail,
                 "CS_DEBUGGED is clear (flags=0x%08x). No debugger is attached. "
                 "Arm JIT with StikDebug and relaunch.", r->cs_flags);
        return;
    }

    void *rw; mach_vm_address_t rx; size_t len;
    if (!build_alias(r, &rw, &rx, &len)) return;
    munmap(rw, len);

    r->state = had_crumb ? JIT_CRASHED : JIT_MAPPED;
    if (had_crumb) {
        snprintf(r->detail, sizeof r->detail,
                 "Mapping works (remap kr=0, protect kr=0) but a previous execute "
                 "attempt stopped at \"%s\". Reset the marker to retry.", prev);
    } else {
        snprintf(r->detail, sizeof r->detail,
                 "CS_DEBUGGED set, RW/RX alias created and made executable. "
                 "Everything short of jumping into it works. Run the execute test "
                 "to confirm.");
    }
}

void jit_probe_execute(jit_result *r, const char *marker_path) {
    memset(r, 0, sizeof *r);
    read_cs(r);

    char prev[64] = {0};
    if (read_crumb(marker_path, prev, sizeof prev)) {
        r->state = JIT_CRASHED;
        snprintf(r->last_step, sizeof r->last_step, "%s", prev);
        snprintf(r->detail, sizeof r->detail,
                 "A previous attempt stopped at \"%s\" and never cleared. Reset the "
                 "marker before retrying.", prev);
        return;
    }

    if (!r->cs_debugged) {
        r->state = JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail,
                 "CS_DEBUGGED is clear (flags=0x%08x); execution would certainly "
                 "fault. Arm JIT first.", r->cs_flags);
        return;
    }

    crumb(marker_path, "build_alias");
    void *rw; mach_vm_address_t rx; size_t len;
    if (!build_alias(r, &rw, &rx, &len)) { jit_probe_reset(marker_path); return; }

    crumb(marker_path, "write+icache");
    const uint32_t ret_insn = 0xD65F03C0u;      /* AArch64 `ret` */
    memcpy(rw, &ret_insn, sizeof ret_insn);
    /* Mandatory on arm64. Skipping it gives intermittent, unreproducible
     * failures that look like a JIT bug for weeks. */
    sys_icache_invalidate((void *)(uintptr_t)rx, sizeof ret_insn);

    crumb(marker_path, "execute");
    ((void (*)(void))(uintptr_t)rx)();          /* the actual test */

    jit_probe_reset(marker_path);               /* survived */
    munmap(rw, len);

    r->state = JIT_WORKING;
    snprintf(r->detail, sizeof r->detail,
             "Wrote an instruction through the RW alias and executed it through the "
             "RX alias. Dynarec output will run on this device.");
}
