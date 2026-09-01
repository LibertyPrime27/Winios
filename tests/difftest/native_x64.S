/* native_run(nstate *s) -- execute a code snippet on the real CPU.
 *
 * Loads every GPR and the flags from `s`, calls s->code on the guest stack
 * (s->gpr[RSP]), and stores everything back. The snippet must end with `ret`.
 *
 * This is the oracle for the emulator. Nothing here is clever on purpose.
 *
 * struct layout (see difftest.c):
 *   0   gpr[16]     rax rcx rdx rbx rsp rbp rsi rdi r8..r15
 *   128 rflags
 *   136 code
 *   144 saved_rsp
 */
    .text
    .globl  native_run
    .type   native_run, @function
native_run:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rdi, g_state(%rip)
    movq    %rsp, 144(%rdi)
    movq    136(%rdi), %rax
    movq    %rax, g_code(%rip)

    /* flags first, while we still have a scratch register */
    pushq   128(%rdi)
    popfq

    movq    0(%rdi),   %rax
    movq    8(%rdi),   %rcx
    movq    16(%rdi),  %rdx
    movq    24(%rdi),  %rbx
    movq    40(%rdi),  %rbp
    movq    48(%rdi),  %rsi
    movq    64(%rdi),  %r8
    movq    72(%rdi),  %r9
    movq    80(%rdi),  %r10
    movq    88(%rdi),  %r11
    movq    96(%rdi),  %r12
    movq    104(%rdi), %r13
    movq    112(%rdi), %r14
    movq    120(%rdi), %r15
    movq    32(%rdi),  %rsp          /* switch to the guest stack */
    movq    56(%rdi),  %rdi          /* rdi last -- it was our state pointer */

    call    *g_code(%rip)            /* pushes return address onto the guest stack */

    /* Back. Save rax before we need a register for the state pointer. */
    movq    %rax, g_scratch(%rip)
    movq    g_state(%rip), %rax
    movq    %rcx, 8(%rax)
    movq    %rdx, 16(%rax)
    movq    %rbx, 24(%rax)
    movq    %rsp, 32(%rax)
    movq    %rbp, 40(%rax)
    movq    %rsi, 48(%rax)
    movq    %rdi, 56(%rax)
    movq    %r8,  64(%rax)
    movq    %r9,  72(%rax)
    movq    %r10, 80(%rax)
    movq    %r11, 88(%rax)
    movq    %r12, 96(%rax)
    movq    %r13, 104(%rax)
    movq    %r14, 112(%rax)
    movq    %r15, 120(%rax)
    pushfq                            /* still on the guest stack; harmless */
    popq    128(%rax)
    movq    g_scratch(%rip), %rcx
    movq    %rcx, 0(%rax)

    movq    144(%rax), %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size   native_run, .-native_run

    .bss
    .align  8
g_state:   .quad 0
g_code:    .quad 0
g_scratch: .quad 0

    .section .note.GNU-stack,"",@progbits
