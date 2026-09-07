/* Structured exception handling: the dispatch itself.
 *
 * Nothing here faults. Every exception is raised by RaiseException, which is
 * not a lesser case than a fault -- 32-bit MSVC compiles C++ `throw` into
 * RaiseException(0xE06D7363) and catches it through this same chain, so a
 * game built with MSVC has this on its critical path without containing a
 * single __try. It is also the half that works identically through the
 * interpreter and the dynarec, because a software exception arrives through a
 * stub with the guest's registers already spilled. Faults are in faulttest.
 *
 * The registration records are pushed by hand in inline asm rather than with
 * __try, because that is what the thing being tested actually is: MSVC's
 * __try compiles to exactly these four instructions, and writing them out
 * means the test does not depend on which exception model the compiler that
 * built it happens to use.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static volatile LONG g_valid = 0x5A5A;
static int g_order[16], g_norder;
static void note(int tag) { if (g_norder < 16) g_order[g_norder++] = tag; }

#ifndef _WIN64
/* The record __try pushes: previous record, handler. */
typedef struct sframe { struct sframe *prev; void *handler; } sframe;

#define SEH_PUSH(rec, h) do { \
        (rec).handler = (void *)(h); \
        __asm__ volatile ("movl %%fs:0, %0" : "=r"((rec).prev)); \
        __asm__ volatile ("movl %0, %%fs:0" :: "r"(&(rec)) : "memory"); \
    } while (0)
#define SEH_POP(rec) __asm__ volatile ("movl %0, %%fs:0" :: "r"((rec).prev) : "memory")

static EXCEPTION_DISPOSITION __cdecl
inner(EXCEPTION_RECORD *er, void *frame, CONTEXT *ctx, void *disp) {
    (void)frame; (void)ctx; (void)disp;
    if (er->ExceptionFlags & EXCEPTION_UNWINDING) return ExceptionContinueSearch;
    note(2);
    printf("  inner: passing %#lx on\n", (unsigned long)er->ExceptionCode);
    return ExceptionContinueSearch;
}
static EXCEPTION_DISPOSITION __cdecl
outer(EXCEPTION_RECORD *er, void *frame, CONTEXT *ctx, void *disp) {
    (void)frame; (void)disp; (void)ctx;
    if (er->ExceptionFlags & EXCEPTION_UNWINDING) return ExceptionContinueSearch;
    note(3);
    printf("  outer: taking %#lx with parameters", (unsigned long)er->ExceptionCode);
    for (DWORD i = 0; i < er->NumberParameters; i++)
        printf(" %#lx", (unsigned long)er->ExceptionInformation[i]);
    printf("\n");
    return ExceptionContinueExecution;
}

/* The inner frame goes in its own function on purpose. Windows requires the
 * records to run strictly up the stack -- one of the checks that stops a
 * corrupt list from sending control somewhere arbitrary -- and two records in
 * one stack frame can land in either order, which compiled code never does:
 * MSVC gives a function one record however many __try levels it has. So the
 * nesting here is real nesting, and the walk has to cross a call frame to
 * reach the outer handler. */
__attribute__((noinline)) static void raise_from_inner(void) {
    sframe i; SEH_PUSH(i, inner);
    ULONG_PTR args[2] = { 0x1111, 0x2222 };
    RaiseException(0xE0001234u, 0, 2, args);
    printf("RaiseException returned, so a handler continued execution\n");
    SEH_POP(i);
}
static void test_chain(void) {
    sframe o; SEH_PUSH(o, outer);
    raise_from_inner();
    SEH_POP(o);
}
#endif /* !_WIN64 */

static LONG CALLBACK vectored(EXCEPTION_POINTERS *ep) {
    note(4);
    printf("  vectored: saw %#lx before any frame handler\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI last_resort(EXCEPTION_POINTERS *ep) {
    note(5);
    printf("  unhandled filter: %#lx reached the end of the chain\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_EXECUTION;         /* keep the test alive */
}

static void test_unhandled(void) {
    SetUnhandledExceptionFilter(last_resort);
    RaiseException(0xE0009999u, 0, 0, NULL);
    printf("RaiseException returned after the filter continued execution\n");
    SetUnhandledExceptionFilter(NULL);
}

static void test_bad_pointers(void) {
    printf("IsBadReadPtr(NULL, 4): %d (want 1)\n", (int)IsBadReadPtr(NULL, 4));
    printf("IsBadReadPtr(&g_valid, 4): %d (want 0)\n", (int)IsBadReadPtr((void *)&g_valid, 4));
    printf("IsBadReadPtr(&g_valid, 0): %d (want 0)\n", (int)IsBadReadPtr((void *)&g_valid, 0));
}

/* Nobody takes it and no filter is installed: the run ends and says what
 * happened. Recorded separately, because the exit code is the result. */
static void raise_unhandled(void) {
    printf("raising with nothing to catch it\n");
    fflush(stdout);
    RaiseException(0xE000DEADu, 0, 0, NULL);
    printf("FAILED - RaiseException returned from an unhandled exception\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "die")) { raise_unhandled(); return 0; }
    printf("%d-bit\n", (int)(8 * sizeof(void *)));

#ifndef _WIN64
    printf("\ntwo frames, inner passes it on:\n");
    test_chain();
#else
    printf("\nthe frame list is a 32-bit mechanism; skipping the chain test\n");
#endif

    printf("\na vectored handler runs before the frame list:\n");
    void *h = AddVectoredExceptionHandler(1, vectored);
    ULONG_PTR one = 0x4242;
    SetUnhandledExceptionFilter(last_resort);
    RaiseException(0xE000ABCDu, 0, 1, &one);
    printf("back after the vectored handler declined and the filter continued\n");
    SetUnhandledExceptionFilter(NULL);
    RemoveVectoredExceptionHandler(h);

    printf("\nnothing takes it:\n");
    test_unhandled();

    printf("\n");
    test_bad_pointers();

    printf("\nhandlers ran in this order:");
    for (int i = 0; i < g_norder; i++) printf(" %d", g_order[i]);
    printf("\n  (2 inner, 3 outer, 4 vectored, 5 unhandled filter)\n");
    return 0;
}
