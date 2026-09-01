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
    case JIT_FLAG_ONLY: return "flag set, pages not executable";
    case JIT_WORKING:   return "working";
    case JIT_CRASHED:   return "crashed on a previous attempt";
    default:            return "unknown";
    }
}

void jit_probe(jit_result *r, const char *marker_path) {
    memset(r, 0, sizeof *r);

    /* A probe that faulted last launch must not be repeated blindly, or the
     * app becomes uninstallable-by-crash-loop. */
    if (marker_path) {
        FILE *f = fopen(marker_path, "r");
        if (f) {
            fclose(f);
            r->state = JIT_CRASHED;
            snprintf(r->detail, sizeof r->detail,
                     "A previous probe did not return. Executable memory faulted. "
                     "Delete the marker in the app's container to retry.");
            return;
        }
    }

    uint32_t flags = 0;
    int rc = csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags);
    r->cs_flags = flags;
    r->cs_debugged = (rc == 0) && (flags & CS_DEBUGGED) != 0;

    if (!r->cs_debugged) {
        r->state = JIT_ABSENT;
        snprintf(r->detail, sizeof r->detail,
                 "CS_DEBUGGED is clear (csops rc=%d, flags=0x%08x). No debugger has "
                 "attached. Arm JIT with StikDebug, then relaunch.", rc, flags);
        return;
    }

    /* Dual mapping: one physical page, two virtual aliases -- write through RW,
     * execute through RX. mmap+mprotect(PROT_EXEC) is refused on iOS 18.4+ even
     * when CS_DEBUGGED is set, which is why this shape is necessary. */
    const size_t page = (size_t)getpagesize();
    void *rw = mmap(NULL, page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rw == MAP_FAILED) {
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail, "mmap of the writable alias failed");
        return;
    }

    mach_vm_address_t rx = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &rx, page, 0,
                                     VM_FLAGS_ANYWHERE, mach_task_self(),
                                     (mach_vm_address_t)rw, /*copy=*/FALSE,
                                     &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        munmap(rw, page);
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail,
                 "mach_vm_remap failed (kr=%d). The RW/RX alias pair could not be "
                 "created, so unsigned code cannot be executed here.", kr);
        return;
    }
    r->remap_ok = 1;

    kr = vm_protect(mach_task_self(), (vm_address_t)rx, page, FALSE,
                    VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        munmap(rw, page);
        r->state = JIT_FLAG_ONLY;
        snprintf(r->detail, sizeof r->detail,
                 "vm_protect(RX) on the alias failed (kr=%d). This is the iOS 18.4+ "
                 "behaviour: the flag is set but executable memory is still refused.", kr);
        return;
    }

    /* AArch64 `ret`. Written through RW, executed through RX. */
    const uint32_t ret_insn = 0xD65F03C0u;
    memcpy(rw, &ret_insn, sizeof ret_insn);

    /* Mandatory on arm64: without this the icache may hold stale bytes and the
     * failure is intermittent and unreproducible. */
    sys_icache_invalidate((void *)(uintptr_t)rx, sizeof ret_insn);

    if (marker_path) {
        FILE *f = fopen(marker_path, "w");
        if (f) { fputs("probing\n", f); fflush(f); fclose(f); }
    }

    ((void (*)(void))(uintptr_t)rx)();      /* the actual test */

    if (marker_path) remove(marker_path);   /* survived */

    munmap(rw, page);
    r->state = JIT_WORKING;
    snprintf(r->detail, sizeof r->detail,
             "Wrote an instruction through the RW alias and executed it through the "
             "RX alias. Dynarec output will run on this device.");
}
