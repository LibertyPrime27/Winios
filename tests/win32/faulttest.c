/* A CPU fault, turned into the exception Windows would have raised.
 *
 * Two faults, each repaired by the handler and then *re-executed*: the handler
 * fixes the register that was wrong and returns ExceptionContinueExecution.
 * Repairing a register rather than stepping Eip past the instruction is what
 * makes this test worth having -- it needs no knowledge of how long the
 * instruction was, so it says nothing about the compiler's encoding choices,
 * and it only passes if the CONTEXT was written from the real register file,
 * read back into it, and execution really did resume at the faulting
 * instruction.
 *
 * The registration record is pushed by hand in inline asm because that is
 * what is being tested: MSVC's __try compiles to exactly these instructions,
 * and writing them out means the test does not depend on which exception
 * model the compiler that built it happens to use.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static volatile LONG g_valid = 0x5A5A;

#ifndef _WIN64
typedef struct sframe { struct sframe *prev; void *handler; } sframe;
#define SEH_PUSH(rec, h) do { \
        (rec).handler = (void *)(h); \
        __asm__ volatile ("movl %%fs:0, %0" : "=r"((rec).prev)); \
        __asm__ volatile ("movl %0, %%fs:0" :: "r"(&(rec)) : "memory"); \
    } while (0)
#define SEH_POP(rec) __asm__ volatile ("movl %0, %%fs:0" :: "r"((rec).prev) : "memory")

static EXCEPTION_DISPOSITION __cdecl
fixer(EXCEPTION_RECORD *er, void *frame, CONTEXT *ctx, void *disp) {
    (void)frame; (void)disp;
    if (er->ExceptionFlags & EXCEPTION_UNWINDING) return ExceptionContinueSearch;
    printf("  fixer: code %#lx, %lu parameters", (unsigned long)er->ExceptionCode,
           (unsigned long)er->NumberParameters);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        printf(", %s at %#lx", er->ExceptionInformation[0] ? "write" : "read",
               (unsigned long)er->ExceptionInformation[1]);
    printf("\n");
    /* Not the value of Eip -- that is an address in this binary, and printing
     * it would make the recording brittle for no gain. That it points at the
     * faulting instruction is what the repair below proves. */
    printf("  fixer: context has eip set: %s, eax %#lx, ecx %#lx\n",
           ctx->Eip ? "yes" : "no", (unsigned long)ctx->Eax, (unsigned long)ctx->Ecx);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ctx->Eax = (DWORD)(ULONG_PTR)&g_valid;      /* the pointer is now good */
        return ExceptionContinueExecution;
    }
    if (er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        ctx->Ecx = 2;                               /* the divisor is now good */
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}

static void test_access_violation(void) {
    sframe rec; SEH_PUSH(rec, fixer);
    LONG got = 0;
    /* eax deliberately null; the handler makes it point at g_valid */
    __asm__ volatile ("movl (%%eax), %%ecx" : "=c"(got) : "a"(0));
    SEH_POP(rec);
    printf("read through a repaired pointer: %#lx (want 0x5a5a)\n", (unsigned long)got);
}

static void test_divide_by_zero(void) {
    sframe rec; SEH_PUSH(rec, fixer);
    unsigned q = 0;
    /* 10 / 0; the handler makes the divisor 2 */
    __asm__ volatile ("xorl %%edx, %%edx\n\tdivl %%ecx"
                      : "=a"(q) : "a"(10u), "c"(0u) : "edx");
    SEH_POP(rec);
    printf("divide with a repaired divisor: %u (want 5)\n", q);
}
#endif /* !_WIN64 */

/* No handler anywhere: the run has to end, with the exception named. That is
 * the other half of the behaviour and worth recording too -- a layer that
 * silently carried on past an unhandled access violation would be worse than
 * one with no exception handling at all. */
static int let_it_through(void) {
    printf("no handler is installed; this fault should end the run\n");
    fflush(stdout);
    volatile int *p = (int *)0;
    return *p;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "die")) return let_it_through();
    printf("%d-bit\n", (int)(8 * sizeof(void *)));
#ifndef _WIN64
    printf("\naccess violation, repaired by the handler:\n");
    test_access_violation();
    printf("\ninteger divide by zero, repaired by the handler:\n");
    test_divide_by_zero();
#else
    /* 64-bit __except needs the image's unwind tables, not a list on the
     * stack -- see the end of win32/seh.c. Repairing a fault from a handler
     * has nothing to hook onto until that exists. */
    printf("\nrepairing a fault needs a frame handler, which is 32-bit only here\n");
#endif
    return 0;
}
