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
 *   160 fxsave area (512 bytes, 16-aligned): x87, MXCSR, XMM0-15
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
    fxsave64  g_hostfx(%rip)          /* keep this process's own FPU state */
    fxrstor64 160(%rdi)
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
    fxsave64  160(%rax)
    fxrstor64 g_hostfx(%rip)
    pushfq                            /* still on the guest stack; harmless */
    popq    128(%rax)
    movq    g_scratch(%rip), %rcx
    movq    %rcx, 0(%rax)

    movq    144(%rax), %rsp
    cld
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
g_saved_rsp: .quad 0
    .align  16
g_hostfx:  .space 512


/* ---------------------------------------------------------------------------
 * native_run32(nstate32 *s, uint32_t stub32) -- run a 32-bit snippet on the
 * real CPU, from this 64-bit process.
 *
 * Linux keeps a 32-bit code segment (selector 0x23) in every process's GDT;
 * a far return into it puts the CPU in compatibility mode, which is exactly
 * how a 32-bit Windows program runs under a 64-bit kernel. Everything the
 * 32-bit side touches -- the state block, the stub, the snippet, its stack
 * and data -- lives below 4 GB (MAP_32BIT). The stub is a template copied
 * into low memory with its absolute addresses patched (see difftest.c).
 *
 * nstate32 layout:
 *   0   gpr[8]   eax ecx edx ebx esp ebp esi edi (32-bit each)
 *   32  eflags
 *   36  code     address of the snippet
 *   64  fxsave area (512 bytes, 16-aligned)
 */
    .text
    .globl  native_run32
    .type   native_run32, @function
native_run32:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rdi, g_state(%rip)
    movq    %rsp, g_saved_rsp(%rip)
    fxsave64  g_hostfx(%rip)
    fxrstor64 64(%rdi)
    /* compat mode needs real data selectors; 64-bit code runs with null ones */
    movl    $0x2b, %eax
    movl    %eax, %ds
    movl    %eax, %es
    pushq   $0x23                    /* __USER32_CS */
    pushq   %rsi                     /* 32-bit stub address */
    lretq
    .globl  native_back64
native_back64:                       /* reached through the landing pad */
    cld                              /* a snippet may leave DF set; the ABI needs it clear */
    movq    g_saved_rsp(%rip), %rsp
    movq    g_state(%rip), %rdi
    fxsave64  64(%rdi)
    fxrstor64 g_hostfx(%rip)
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size   native_run32, .-native_run32

/* 32-bit stub template. Patched at copy time: the two state addresses, the
 * absolute call target (state + 36) and the landing-pad address. Each patch
 * label marks the byte after a 4-byte immediate. */
    .globl  stub32_tmpl, stub32_tmpl_end
    .globl  s32_p_state1, s32_p_code, s32_p_state2, s32_p_land
    .code32
stub32_tmpl:
    movl    $0x11111111, %esi
s32_p_state1:
    movl    16(%esi), %esp           /* first: the 64-bit stack is out of reach here */
    pushl   32(%esi)
    popfl
    movl    0(%esi), %eax
    movl    4(%esi), %ecx
    movl    8(%esi), %edx
    movl    12(%esi), %ebx
    movl    20(%esi), %ebp
    movl    28(%esi), %edi
    movl    24(%esi), %esi
    call    *0x22222222              /* [state + 36] */
s32_p_code:
    pushfl
    pushl   %esi
    movl    $0x11111111, %esi
s32_p_state2:
    movl    %eax, 0(%esi)
    movl    %ecx, 4(%esi)
    movl    %edx, 8(%esi)
    movl    %ebx, 12(%esi)
    movl    %ebp, 20(%esi)
    movl    %edi, 28(%esi)
    popl    24(%esi)
    popl    32(%esi)
    movl    %esp, 16(%esi)
    pushl   $0x33                    /* __USER_CS */
    pushl   $0x33333333              /* landing pad */
s32_p_land:
    lret
stub32_tmpl_end:
    .code64

/* 64-bit landing pad template, also copied below 4 GB (a 32-bit far return
 * can only name a 32-bit offset). Jumps to native_back64. */
    .globl  land64_tmpl, land64_tmpl_end, l64_p_target
land64_tmpl:
    movabs  $0x4444444444444444, %rax
l64_p_target:
    jmp     *%rax
land64_tmpl_end:

    .section .note.GNU-stack,"",@progbits
