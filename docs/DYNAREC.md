# The dynarec: x86 blocks → ARM64 code

**Where:** `core/src/jit/` — `a64.h` (instruction encoder), `jit.c` (compiler,
dispatcher, helpers). Builds on every host; generates code only on AArch64.
`XCORE_JIT=0` in the environment forces the interpreter.

This is the piece that turns "runs busybox at 51 MIPS" into something a game
can live on. It compiles the same basic blocks the block cache
(`core/src/cache.c`) already decodes, one block at a time on first execution,
into native ARM64 that runs until the block's end and then hands control back
to a small dispatcher loop with `RIP` set.

## The shape of it

There is no intermediate representation. Each x86 instruction — already in the
pre-resolved `xop` operand form — lowers directly to a few ARM64 instructions.
Guest registers RAX..R15 have a fixed home in host registers (x8–x17, x19–x24;
x18 is Apple's platform register and is never touched) and are loaded from the
CPU struct on first use in a block and written back at the block's exits, so a
block only touches the registers it names. x25 holds the arena base, x26 the
CPU struct, x27 the dispatcher's address.

Whatever the compiler does not handle natively is a **callout**: the block
spills its registers, calls the interpreter for that one instruction, reloads,
and carries on. Coverage grows by moving instructions from the callout path to
native lowering; correctness never depends on how far that has got. Today the
native set is the integer core — moves, LEA, the ALU group, INC/DEC/NEG/NOT,
shifts and rotates, IMUL, PUSH/POP/CALL/RET/LEAVE, JMP/Jcc/SETcc/CMOVcc, the
sign-extension group — in all four operand widths, both address modes, both
guest modes. SSE, x87, string ops, DIV/MUL and the rare things call out.

## Flags

x86 sets six flags per arithmetic instruction; computing them eagerly costs
more than the arithmetic. Two mechanisms avoid that:

- **Lazy flags.** A flag-setting instruction records `(kind, width, a, b, r)`
  in the CPU struct; `xc_flags_sync()` turns that into `rflags` on demand using
  the interpreter's own flag functions, so the two paths agree bit for bit.
  Everything that reads `rflags` — the interpreter on a callout, the host at a
  syscall, PUSHF — syncs first.
- **Native conditions.** Within a block the compiler tracks which ARM
  condition codes are still valid from the last `ADDS`/`SUBS`/`ANDS` it
  emitted, so `cmp; jcc` becomes `subs; cset/cbnz` with no flag
  materialisation at all. The x86→ARM condition mapping depends on the
  producing operation (x86 CF after SUB is ARM's `!C`; after ADD it is `C`;
  after a logic op it is 0), which the tracker handles. Anything it cannot map
  natively — a condition at the top of a block, parity, CF after INC — goes to a
  helper that syncs and evaluates.
- **Liveness.** A backwards pass over the block finds flag writes that are
  overwritten before anything reads them (most of them) and drops the lazy
  store.

Narrow operands (8/16-bit) are shifted to the top of a 32-bit register before
`ADDS`/`SUBS` so N, Z, C and V come out right for the narrow width.

## Memory

Every guest access is `ldr/str Rt, [x25, Rn{, uxtw}]`. For a 32-bit guest x25
is the arena base and the `uxtw` is the `base + zext32(addr)` of the memory
model, at zero cost; for a 64-bit guest x25 is 0 and the same form is the
identity mapping. When a 32-bit arena is smaller than 4 GB (a memory-constrained
configuration, or a test), every access carries a bounds check that faults the
way the interpreter does; a full-size arena needs none. An access to an
unmapped page *inside* the arena is a host fault today; a signal handler that
turns it into a guest fault is the runtime's job later.

## Self-modifying code

Handled by the block cache, not the JIT: before a block runs, its code bytes
are compared with memory (one `memcmp` per block execution); a mismatch drops
the block, and the next lookup decodes and compiles afresh.

## Verification

Three layers, from cheapest to most authoritative:

1. **Golden vectors.** The on-device self-test replays every recorded silicon
   post-state through the JIT (`xc_run`) as well as the interpreter. 2364
   vectors; a divergence is a one-line diff naming the instruction.
2. **qemu-user on the Linux runner.** CI cross-builds for aarch64 and runs the
   self-test and the guest programs (i386 and x86-64, musl and glibc) through
   the JIT under `qemu-aarch64`. This is also the local development loop on an
   x86 machine: `cmake -S . -B build-a64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux.cmake`.
3. **Apple silicon.** The macOS CI job runs the self-test natively; MemProbe
   runs it on the iPad and iPhone inside a debugger-blessed arena
   (`xc_jit_set_code`), which is the real target environment.

   One rule on iOS 26 / TXM hardware: **one bless per launch.** Blessing ends
   with the debugger detaching, and any `brk #0xf00d` after that is an
   unserviced breakpoint -- the kernel delivers SIGTRAP and the process dies
   (`"esr": "(Breakpoint) brk 61453"` in the crash log). The first MemProbe
   build with the dynarec pass created a second arena for it and crashed
   exactly there. `jit_arena_shared()` now owns the single arena, the execute
   probe and the dynarec pass both draw from it, and `jit_arena_create()`
   refuses a second attempt in the same process rather than trapping. Real
   dynarec code memory therefore has to be sized up front (or the app has to
   re-attach StikDebug to grow it), which is why the code cache is one arena
   sub-allocated by xcore rather than a pool of mappings.

Debugging aids: `XCORE_JIT_TRACE=1` prints each block entry; `XCORE_JIT_DUMP=dir`
writes every compiled block's bytes for `objdump -D -b binary -m aarch64`.

## Measured

Under `qemu-aarch64` (so absolute numbers are meaningless, only the ratio
matters): a 30 M-iteration integer loop runs 8.7× faster through the JIT than
through the interpreter's block cache. Real numbers come from the devices.

## What comes next, in order

1. **Block chaining.** Today every block returns to the dispatcher (a hash
   lookup) and reloads its registers. Patching direct jumps between compiled
   blocks, and keeping registers in their host homes across the link, removes
   most of the remaining overhead for hot loops.
2. **SSE natively.** Packed integer and scalar/packed float on NEON; x87 stays
   in the interpreter (SoftFloat) — its exactness is the point, and 32-bit
   games' float math runs through D3D9's 24-bit mode where speed matters less
   than the rounding being right.
3. **Faults.** Map host SIGSEGV/SIGBUS inside guest code to guest faults with
   the interpreter's `XC_STOP_FAULT` semantics.
4. **A code cache on disk**, keyed by the block's bytes, so later launches
   start compiled (see `CPU-CORE.md`, "Could we just compile it instead").
