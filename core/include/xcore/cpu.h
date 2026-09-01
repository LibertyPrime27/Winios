/* xcore -- a single x86 CPU core for both 32-bit and 64-bit guests.
 *
 * One decoder, one register file, one interpreter. What differs between modes
 * is default operand size, address width, and how guest memory is mapped:
 *
 *   64-bit guest  -> identity: guest VA == host VA. Darwin reserves the low
 *                    4 GB but 64-bit code never needs it, so this is free.
 *   32-bit guest  -> arena: a 4 GB block anywhere in host VA, and every access
 *                    is base + zext32(addr). This is the ONLY way to run 32-bit
 *                    code on iOS, where __PAGEZERO cannot be shrunk. One add per
 *                    access; the dynarec later folds it into the addressing mode.
 *
 * Decode is delegated to Zydis (MIT). The value here is the memory model, the
 * execution semantics, and the differential test harness that keeps them honest.
 */
#ifndef XCORE_CPU_H
#define XCORE_CPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { XC_MODE_32 = 32, XC_MODE_64 = 64 } xc_mode;

/* RFLAGS bits we model. */
#define XC_CF (1u << 0)
#define XC_PF (1u << 2)
#define XC_AF (1u << 4)
#define XC_ZF (1u << 6)
#define XC_SF (1u << 7)
#define XC_DF (1u << 10)
#define XC_OF (1u << 11)
#define XC_ARITH_FLAGS (XC_CF | XC_PF | XC_AF | XC_ZF | XC_SF | XC_OF)

enum { XC_RAX, XC_RCX, XC_RDX, XC_RBX, XC_RSP, XC_RBP, XC_RSI, XC_RDI,
       XC_R8,  XC_R9,  XC_R10, XC_R11, XC_R12, XC_R13, XC_R14, XC_R15 };

/* Why execution stopped. */
typedef enum {
    XC_STOP_NONE = 0,
    XC_STOP_STEPS,        /* step budget exhausted; resume freely */
    XC_STOP_HLT,          /* HLT executed */
    XC_STOP_SYSCALL,      /* SYSCALL / INT 0x80 -- host decides, then resume */
    XC_STOP_BREAKPOINT,   /* INT3 */
    XC_STOP_UNDEFINED,    /* UD2 or an instruction we do not implement yet */
    XC_STOP_DECODE,       /* bytes at RIP did not decode */
    XC_STOP_FAULT,        /* memory outside the guest arena, #DE, etc. */
} xc_stop;

typedef struct xc_mem {
    xc_mode  mode;
    uint8_t *base;        /* arena base for 32-bit; unused for identity */
    uint64_t size;        /* arena size (32-bit) */
} xc_mem;

typedef struct xc_cpu {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    uint64_t fs_base, gs_base;
    xc_mode  mode;
    xc_mem  *mem;

    /* Diagnostics for the last stop. */
    xc_stop  stop;
    uint64_t fault_addr;
    int      syscall_vector;   /* 0x80 for INT 80, -1 for SYSCALL */
    char     last_insn[64];    /* disassembly, when the formatter is built in */
} xc_cpu;

/* Memory model -------------------------------------------------------- */

/* Identity mapping: guest VA == host VA. Only meaningful for 64-bit guests. */
void xc_mem_init_identity(xc_mem *m);

/* Arena mapping for 32-bit guests. `base` must be at least `size` bytes. */
void xc_mem_init_arena(xc_mem *m, void *base, uint64_t size);

/* Translate a guest address for a `len`-byte access. NULL if out of range. */
void *xc_mem_ptr(const xc_mem *m, uint64_t gaddr, size_t len);

/* CPU ------------------------------------------------------------------ */

void xc_cpu_init(xc_cpu *c, xc_mode mode, xc_mem *mem);

/* Execute up to `max_steps` instructions. Returns the stop reason; 
 * XC_STOP_STEPS means nothing went wrong. */
xc_stop xc_run(xc_cpu *c, uint64_t max_steps);

/* Execute exactly one instruction. */
xc_stop xc_step(xc_cpu *c);

const char *xc_stop_name(xc_stop s);

#ifdef __cplusplus
}
#endif
#endif
