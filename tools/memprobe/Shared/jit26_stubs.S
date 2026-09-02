/* Breakpoint stubs for the iOS 26 TXM JIT protocol.
 *
 * On A15+/M2+ the Trusted Execution Monitor gates execution per page, and a
 * page only becomes executable once an attached debugger has WRITTEN to it.
 * The app cannot do that to itself, so StikDebug's script and the app talk
 * through a breakpoint: the app executes `brk #0xf00d` with a selector in x16,
 * the script catches SIGTRAP, reads x16/x0/x1, performs the operation, sets PC
 * past the brk and continues.
 *
 *   x16 = 0   detach            -- ends the session; no further brk is safe
 *   x16 = 1   prepare region    -- x0 = addr, x1 = len; debugger writes one
 *                                  byte per 16 KB page, authorising execution
 *
 * These MUST NOT be called unless a script-capable debugger is attached: an
 * unhandled brk terminates the process.
 *
 * Written in assembly rather than as naked C functions so the register
 * contract is exact and nothing the compiler emits can disturb x16 or the
 * argument registers.
 */
    .text
    .p2align 2
    .globl _jit26_detach
_jit26_detach:
    mov     x16, #0
    brk     #0xf00d
    ret

    .p2align 2
    .globl _jit26_prepare_region
_jit26_prepare_region:              /* (void *addr, size_t len) -> void* */
    mov     x16, #1
    brk     #0xf00d
    ret
