/* 32-bit guest in an arena -- the configuration iOS forces on us.
 *
 * There is no native oracle for this on a 64-bit host, so these are direct
 * semantic checks: the arena is placed wherever malloc puts it, guest addresses
 * are 32-bit offsets into it, and the interpreter must never compute a host
 * pointer outside it. That last property is the one that matters most: a
 * 32-bit address that wraps must wrap inside the arena, not escape it.
 */
#include "xcore/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA (64u << 20)
#define CODE  0x1000u
#define DATA  0x2000u
#define STACK 0x100000u

static int fail(const char *w) { fprintf(stderr, "FAIL: %s\n", w); return 1; }

static void put(uint8_t *a, uint32_t at, uint32_t v) { memcpy(a + at, &v, 4); }
static uint32_t get(uint8_t *a, uint32_t at) { uint32_t v; memcpy(&v, a + at, 4); return v; }

int main(void) {
    uint8_t *arena = calloc(ARENA, 1);
    if (!arena) return fail("calloc");

    xc_mem mem; xc_mem_init_arena(&mem, arena, ARENA);
    xc_cpu c;   xc_cpu_init(&c, XC_MODE_32, &mem);

    /* --- 1. basic 32-bit execution: loads, stores, stack, hlt ----------- */
    static const uint8_t prog[] = {
        0xA1, 0x00, 0x20, 0x00, 0x00,        /* mov eax, [0x2000]      */
        0x01, 0xD8,                          /* add eax, ebx           */
        0xA3, 0x04, 0x20, 0x00, 0x00,        /* mov [0x2004], eax      */
        0x50,                                /* push eax               */
        0x59,                                /* pop ecx                */
        0x89, 0xE2,                          /* mov edx, esp           */
        0xF4,                                /* hlt                    */
    };
    memcpy(arena + CODE, prog, sizeof prog);
    put(arena, DATA, 100);
    c.rip = CODE; c.gpr[XC_RBX] = 5; c.gpr[XC_RSP] = STACK;

    xc_stop s = xc_run(&c, 100);
    if (s != XC_STOP_HLT)             return fail("expected HLT stop");
    if (c.gpr[XC_RAX] != 105)         return fail("eax != 105");
    if (get(arena, DATA + 4) != 105)  return fail("store to [0x2004] missing");
    if (c.gpr[XC_RCX] != 105)         return fail("push/pop through 32-bit stack");
    if (c.gpr[XC_RDX] != STACK)       return fail("esp not restored after pop");
    if (c.gpr[XC_RSP] != STACK)       return fail("esp width");

    /* --- 2. 32-bit address wrap stays inside the arena ------------------ */
    static const uint8_t wrap[] = {
        0x8B, 0x47, 0x06,                    /* mov eax, [edi+6]  ; edi=0xFFFFFFFE -> 0x4 */
        0xF4,
    };
    memcpy(arena + CODE, wrap, sizeof wrap);
    put(arena, 4, 0xCAFEBABE);
    xc_cpu_init(&c, XC_MODE_32, &mem);
    c.rip = CODE; c.gpr[XC_RDI] = 0xFFFFFFFEu; c.gpr[XC_RSP] = STACK;
    if (xc_run(&c, 10) != XC_STOP_HLT) return fail("wrap: expected HLT");
    if (c.gpr[XC_RAX] != 0xCAFEBABEu)  return fail("32-bit EA did not wrap to 0x4");

    /* --- 3. an address past the arena faults instead of escaping -------- */
    static const uint8_t oob[] = {
        0xA1, 0xF0, 0xFF, 0xFF, 0x7F,        /* mov eax, [0x7FFFFFF0] */
        0xF4,
    };
    memcpy(arena + CODE, oob, sizeof oob);
    xc_cpu_init(&c, XC_MODE_32, &mem);
    c.rip = CODE; c.gpr[XC_RSP] = STACK;
    if (xc_run(&c, 10) != XC_STOP_FAULT)   return fail("out-of-arena access did not fault");
    if (c.fault_addr != 0x7FFFFFF0u)       return fail("fault_addr not reported");

    /* --- 4. 32-bit register writes do not touch upper halves ----------- */
    static const uint8_t hi[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,        /* mov eax, 1 */
        0xF4,
    };
    memcpy(arena + CODE, hi, sizeof hi);
    xc_cpu_init(&c, XC_MODE_32, &mem);
    c.rip = CODE; c.gpr[XC_RSP] = STACK;
    c.gpr[XC_RAX] = 0xDEADBEEF00000000ull;
    if (xc_run(&c, 10) != XC_STOP_HLT) return fail("hi: HLT");
    /* In 32-bit mode there is no upper half to observe, but the emulator's
     * state must be well-defined: writes zero-extend to 64 so that a later
     * mode switch or a 64-bit host reading the state sees a canonical value. */
    if (c.gpr[XC_RAX] != 1)            return fail("32-bit write not canonical");

    /* --- 5. INT 0x80 surfaces as a syscall stop ------------------------- */
    static const uint8_t sc[] = { 0xCD, 0x80, 0xF4 };
    memcpy(arena + CODE, sc, sizeof sc);
    xc_cpu_init(&c, XC_MODE_32, &mem);
    c.rip = CODE; c.gpr[XC_RSP] = STACK;
    if (xc_run(&c, 10) != XC_STOP_SYSCALL) return fail("INT 0x80 not surfaced");
    if (c.syscall_vector != 0x80)          return fail("syscall vector");
    if (c.rip != CODE + 2)                 return fail("rip not advanced past INT");

    printf("arena32: all checks passed\n");
    free(arena);
    return 0;
}
