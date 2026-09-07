/* Reading and moving the host PC in a signal context.
 *
 * The dynarec's fault recovery works by moving the host PC: a SIGSEGV at a
 * guest memory access the compiler emitted is turned into a jump to that
 * instruction's recovery stub (see xc_jit_fault_stub in cpu.h). Doing that
 * means reaching into the ucontext the signal handler was handed, which is
 * the one genuinely per-platform thing the runtime does -- so it lives here,
 * once, rather than in each of the two places that need it.
 *
 * Two Darwin details worth stating, because both are easy to get wrong and
 * neither shows up on Linux:
 *
 *   - <ucontext.h> is the deprecated spelling there, and its functions are
 *     marked unavailable on iOS. Nothing here calls them -- only the structure
 *     the handler is given is wanted -- so <sys/ucontext.h> is the include,
 *     which is what <ucontext.h> pulls in anyway.
 *
 *   - The arm64 thread state presents its PC as an *opaque* field when
 *     pointer authentication is in play, with accessor macros beside it. Using
 *     the macros where the SDK defines them is the portable spelling, not a
 *     workaround: a signed PC written as a plain integer would be wrong on
 *     arm64e, and the field is not even named __pc there.
 *
 * XC_HOST_PC(uctx)        the PC, or 0 where the layout is unknown
 * XC_HOST_PC_SET(uctx, v) move it; XC_HAVE_HOST_PC_SET says whether it exists
 *
 * Only ARM64 gets a setter, because only ARM64 has compiled code to recover
 * into. On an x86 host the interpreter is the only thing running guest
 * instructions and it is left through longjmp instead.
 */
#ifndef XCORE_HOSTPC_H
#define XCORE_HOSTPC_H

#include <stdint.h>
#include <signal.h>

#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
  #ifdef __darwin_arm_thread_state64_get_pc
    #define XC_HOST_PC(u) \
        ((uint64_t)(uintptr_t)__darwin_arm_thread_state64_get_pc(((ucontext_t *)(u))->uc_mcontext->__ss))
    #define XC_HOST_PC_SET(u, v) \
        __darwin_arm_thread_state64_set_pc_fptr(((ucontext_t *)(u))->uc_mcontext->__ss, \
                                                (void *)(uintptr_t)(v))
  #else
    #define XC_HOST_PC(u)        ((uint64_t)((ucontext_t *)(u))->uc_mcontext->__ss.__pc)
    #define XC_HOST_PC_SET(u, v) (((ucontext_t *)(u))->uc_mcontext->__ss.__pc = (uint64_t)(v))
  #endif
  #define XC_HAVE_HOST_PC_SET 1

#elif defined(__linux__) && defined(__aarch64__)
  #define XC_HOST_PC(u)        ((uint64_t)((ucontext_t *)(u))->uc_mcontext.pc)
  #define XC_HOST_PC_SET(u, v) (((ucontext_t *)(u))->uc_mcontext.pc = (uint64_t)(v))
  #define XC_HAVE_HOST_PC_SET 1

#elif defined(__APPLE__) && defined(__x86_64__)
  #define XC_HOST_PC(u) ((uint64_t)((ucontext_t *)(u))->uc_mcontext->__ss.__rip)

#elif defined(__linux__) && defined(__x86_64__)
  /* mcontext_t's gregs need _GNU_SOURCE, which every file including this on
   * Linux already defines; REG_RIP comes from the same place. */
  #define XC_HOST_PC(u) ((uint64_t)((ucontext_t *)(u))->uc_mcontext.gregs[REG_RIP])
#endif

#ifndef XC_HOST_PC
  #define XC_HOST_PC(u) ((void)(u), (uint64_t)0)      /* unknown layout: say so */
#endif
#ifndef XC_HAVE_HOST_PC_SET
  #define XC_HAVE_HOST_PC_SET 0
  #define XC_HOST_PC_SET(u, v) ((void)(u), (void)(v))
#endif

#endif
