# xcore — one CPU core for 32-bit and 64-bit x86

**Where:** `core/`. Portable C11, no OS headers. Decode by [Zydis](https://github.com/zyantific/zydis) (MIT); everything else is ours.

## Why one core covers both

x86-64 is not a different instruction set from x86; it is the same encoding
with a REX prefix, a 64-bit default for addresses, and a 32-bit default for
operands. A decoder that handles long mode already handles compatibility
mode — Zydis is told which mode to decode in and does the rest. So "support
32-bit as well" is not a second emulator. It is a mode flag on one.

What actually differs between the two modes, and where each lives:

| Concern | 64-bit guest | 32-bit guest | Where |
|---|---|---|---|
| Decoding | `ZYDIS_MACHINE_MODE_LONG_64` | `LONG_COMPAT_32` | `interp.c`, one branch |
| Address width | 64 (or 32 with `0x67`) | 32 | masked to `insn.address_width` |
| Stack width | 64 | 32 | `insn.stack_width` |
| Register writes | 32-bit writes zero-extend to 64 | same rule, harmless | `reg_write` |
| **Guest memory** | **identity: guest VA == host VA** | **arena: base + zext32(addr)** | `mem.c` |

The last row is the one that matters on iOS, and it is why this core exists
rather than a port of Box64.

## The memory model, and why it is shaped like this

Darwin reserves the low 4 GB of every process (`__PAGEZERO`), and on arm64 it
cannot be shrunk. A 32-bit guest's pointers are 32-bit, so its memory has to
*be* the low 4 GB — unless every access goes through a base register. Box64's
`box32` mode marshals guest pointers by identity and aborts when one exceeds
32 bits; that design cannot run on iOS at all.

xcore therefore has two mappings behind one call, `xc_mem_ptr()`:

- **Identity** (`xc_mem_init_identity`) for 64-bit guests. A 64-bit program has
  no need of the low 4 GB, so its guest addresses can simply be host addresses.
  Free.
- **Arena** (`xc_mem_init_arena`) for 32-bit guests. A block of up to 4 GB
  anywhere in host VA — which `extended-virtual-addressing` makes cheap to
  reserve — and every access is `base + (addr & 0xFFFFFFFF)`. One add. In the
  interpreter it is a real add; in the dynarec it folds into the ARM64
  addressing mode (`ldr x0, [x_base, w_addr, uxtw]`) and costs nothing.

Bounds are enforced in the arena: a 32-bit address that wraps stays inside the
arena, and an address past the end returns NULL and faults the guest rather than
touching host memory. `tests/test_arena32.c` checks both.

## The interpreter

`core/src/interp.c` is the **reference semantics**, and it will remain so after
a dynarec exists: it is what the dynarec is checked against, and what runs when
JIT is unavailable (see `JIT-DESIGN.md` §5 — the interpreter is never dead code).

Currently implemented: `MOV`/`MOVZX`/`MOVSX`/`MOVSXD`, `LEA`, `XCHG`; the ALU
family (`ADD ADC SUB SBB AND OR XOR CMP TEST INC DEC NEG NOT`); shifts and
rotates; `MUL`/`IMUL` (all forms), `DIV`/`IDIV` with `#DE`; the sign-extension
family (`CBW`…`CQO`); `PUSH`/`POP`/`LEAVE`; `JMP`/`Jcc`/`CALL`/`RET`/`JRCXZ`;
`CMOVcc`/`SETcc`; `MOVS*`/`STOS*` with `REP`; `NOP`/`PAUSE`/`ENDBR`; and
`HLT`/`INT3`/`UD2`/`SYSCALL`/`INT n` as stop conditions the host handles.

Flags are computed eagerly and exactly, including the parts people get wrong:
`INC`/`DEC` preserve `CF`; a 32-bit `CMOVcc` zero-extends the destination even
when the move does not happen; shift-by-zero leaves flags untouched; `NEG`
sets `CF` from the operand rather than the result.

Not yet: SSE/x87, `BT*`, `SHLD`/`SHRD`, `CMPXCHG`, `XADD`, `BSF`/`BSR`,
`LODS`/`SCAS`/`CMPS`, segment-register loads, anything privileged. Each is a
case in the switch and a line in the difftest.

## The test that makes this tractable

`tests/difftest/` is the reason a from-scratch x86 core is a reasonable thing to
attempt. The Linux CI runner is an x86-64 machine, so **the CPU it runs on is
the specification**. For every case:

1. Seed all sixteen registers and the six arithmetic flags from a PRNG, so
   preserved-flag bugs and upper-half bugs have something to corrupt.
2. Run the snippet natively via a trampoline (`native_x64.S`) that loads the
   whole register file, calls the bytes on a private stack, and stores
   everything back.
3. Restore memory, run the same bytes through `xc_step` until it returns.
4. Compare all sixteen GPRs, the flags under a per-case mask, and every byte
   of the data and stack regions.

Six seeds per case. A wrong `AF` or a missed zero-extension shows up as a
one-line diff against silicon rather than as a game crashing three layers up.

The mask exists because Intel leaves some flags architecturally undefined —
`OF` after a shift by more than one, everything but `CF`/`OF` after `MUL`, all
of them after `DIV`. A test comparing those would be comparing against one
particular CPU's habit, not the architecture.

The harness has been mutation-tested: removing `INC`'s `CF` preservation or the
`CMOV` zero-extension is caught on the first seed.

## Golden vectors: testing on hardware that has no x86

The difftest oracle only exists on x86. On an iPhone there is no native CPU to
compare against — so `difftest --emit-golden` records what silicon actually
produced for every case, and `core/src/selftest.c` replays those recordings
anywhere. `xc_selftest()` runs on the device and reports mismatches.

This catches a specific and nasty class of bug: code that is correct on x86-64
with gcc and wrong on ARM64 with clang. Shift counts at the width boundary,
signed-overflow assumptions, and anything endianness-dependent all misbehave
differently across architectures, and every one of them would otherwise appear
as "the game only breaks on device."

Vectors are regenerated by CI, not hand-edited. Memory-touching cases are
skipped on replay (the recorded addresses are host pointers that mean nothing
elsewhere); those stay covered by `difftest` on CI and `test_arena32` locally.

## Adding an instruction

1. Add the case in `xc_step`.
2. Add a `T(...)` line in `difftest.c` with the hand-assembled bytes. If the
   instruction leaves flags undefined, mask them and say why in a comment.
3. If it touches memory, make it touch `[rdi+…]` — `rdi` points at the shared
   data buffer, which is compared byte-for-byte.
4. If it is 32-bit-only or address-width sensitive, add a check to
   `test_arena32.c` too.

## What comes after this

In order, and each is gated by the previous:

1. **Enough instructions to run real code.** SSE2 (every modern compiler emits
   it), `CMPXCHG`, `BT*`, the remaining string ops. Measured by running a
   statically linked x86-64 `hello world` to its exit syscall.
2. **A Linux-syscall or Win32 personality on top of the `XC_STOP_SYSCALL`
   boundary.** Which one is a product decision; the core does not care.
3. **The ARM64 dynarec**, behind the same `xc_cpu`, checked against this
   interpreter the way this interpreter is checked against silicon.
