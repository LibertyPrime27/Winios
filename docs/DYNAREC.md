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
shifts and rotates, IMUL, BSWAP, PUSH/POP/CALL/RET/LEAVE, JMP/Jcc/SETcc/CMOVcc,
the sign-extension group — in all four operand widths, both address modes,
both guest modes; REP MOVS/STOS through a helper that memmoves host memory;
and most of SSE/SSE2 on NEON, and x87 arithmetic in 53-bit precision (both
below). DIV/MUL, CMPXCHG, BSF/BSR, CPUID and the rare things call out.
`XCORE_JIT_CALLOUTS=1` prints a histogram of what still calls out, which is
how the next lowering target is picked.

## Block chaining

A block whose exit target is a constant (JMP rel, Jcc both ways, CALL rel,
fall-through) ends in a patchable `b` followed by the slow path: store RIP,
hand the address of that `b` to the dispatcher in x1. The first time the
dispatcher resolves the target it patches the `b` to jump straight into the
target's code and records the site on the target block. From then on the two
blocks run back to back with no dispatcher, hash lookup or byte check in
between. When the cache drops a block (self-modifying code, unmapping) the
recorded sites are patched back to the slow path first, so a stale chain can
never be followed. Because chained entries bypass the dispatcher, the step
budget is checked in each block's prologue instead. `XCORE_JIT_CHAIN=0` turns
chaining off for A/B runs. The trade-off is documented rather than hidden: a
chained target skips the per-execution SMC byte check; write tracking is the
runtime's job.

## SSE on NEON

XMM0–15 live in v16–v31 while a block runs, with the same load-on-use /
store-on-exit discipline as the GPRs; v0–v3 are temporaries. Integer SIMD,
moves, shuffles, packs and logic lower one-to-one (PMULHW is
`smull; smull2; uzp2`, PMOVMSKB is a shift-and-or cascade with no constant
loads). Floating point is where the ISAs disagree, and only in one place:
**NaN results**. x86 propagates the first NaN operand and its default NaN is
negative; ARM prefers an SNaN in either position and its default NaN is
positive. So every FP operation computes into a temporary, tests the result
for NaN (one `fcmp` for scalars, `fcmeq; uminv` for vectors) and, if it is,
leaves the block through the interpreter, which redoes the instruction from
the untouched inputs. Non-NaN IEEE results are bit-identical by definition, so
this is exact; NaN results are rare in real code, so the guard costs two to
four instructions and the slow path almost never runs. Conversions use the
same trick for x86's "integer indefinite" on overflow and NaN; MIN/MAX are
`fcmgt; bsl`, which reproduces x86's "second operand wins on NaN or equal
zeros" rule; COMISS/UCOMISS turn `fcmp`'s NZCV directly into CF/ZF/PF, and a
following Jcc maps onto the ARM condition (JB after COMISS is `b.lt`).

MXCSR: the rounding mode and flush-to-zero are mirrored into FPCR while JIT
code runs; the sticky exception flags accumulate in FPSR and are folded into
MXCSR at callouts and exits. The denormal-operand flag (DE) is not tracked
natively, and FTZ/DAZ are honoured together (ARM's FZ) but not separately.
RCPPS/RSQRTPS (implementation-specific approximations), the pack/unpack
oddities not listed in `jit_sse.h` and everything SSE3+ still call out.

## x87 on NEON

The 32-bit Windows games this project targets do their floating point on the
x87 stack, not SSE — and an x87 register is 80 bits, which no ARM register
holds. But MSVC-built code runs the FPU in **53-bit precision** (FCW `0x027F`,
which the CRT sets and `winrun` starts a process in), and under 53-bit
precision every FADD/FSUB/FMUL/FDIV/FSQRT result is rounded to exactly the
53-bit significand an IEEE double has. The only thing the 80-bit format then
still buys is its 15-bit exponent: a result below `2^-1022` or at/above
`2^1024` stays finite and normal on the FPU where a double would go denormal
or infinite. So `jit_x87.h` holds each stack register that *is* a double as
one, in `d8`–`d15` (callee-saved, so a C call cannot lose them), and does the
arithmetic with the host FP unit — then checks every result lies in the
normal double range (or is an exact zero) and, if not, leaves the block
through the interpreter, which redoes the instruction in 80-bit SoftFloat from
the untouched inputs. The range check is six instructions and the escape
almost never runs.

Two things make this exact. **The stack is compile-time state.** Compiler
output has a fixed FPU depth at every address, so a block is compiled for the
TOP, tag word, FCW and MXCSR rounding seen when it was first reached and
checks that word once, at its first x87 op; a mismatch runs that one
instruction in the interpreter and leaves. FXCH is then a rename of two
table entries (free); FLD/FSTP move TOP at compile time. **The 80-bit file
stays authoritative.** `cpu->fpr[]` is still the architectural state the
interpreter uses; beside it `cpu->fpr_d[]` shadows each register as a double,
and C code that reads `fpr[]` (a callout, the end of `xc_run_jit`) first
materialises any register the JIT holds only as a double — `f64→f80` is exact
and bit-identical to `FLD m64`. Everything outside the fast set — 80-bit
loads/stores, transcendentals, FPREM, the environment and save/restore
instructions, a full-stack push or empty-register read known at compile time
— calls out and ends the block, so the compile-time model never has to track
what the interpreter did. FCW `0x037F` (64-bit precision) disables the fast
path for the block and falls back to the interpreter, as does a FCW/MXCSR
rounding-mode disagreement or flush-to-zero. `XCORE_JIT_X87=0` disables it
entirely.

Exception flags share FPSR with the SSE path: they are folded into whichever
status word owns them (MXCSR by default, `fsw` while a block is between its
first x87 op and its exit) at the switch. As with SSE, the denormal-operand
flag (DE) is not tracked. The whole writeback path uses only `x2`/`x3`/`x28`
and never touches NZCV, because a Jcc's condition or a computed exit address
can be live in `x0`/`x1` across the register spill it triggers.

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
2b. **JIT vs interpreter, every difftest case** (`tests/test_jitdiff.c`).
   Takes all 336 x86-64 cases of the differential suite — memory operands,
   flags, MXCSR and the full x87 state included, which the on-device replay
   cannot cover — seeds registers, memory, XMM and a partly-filled x87 stack
   (with extra seeds full of NaNs of both kinds, infinities, signed zeros,
   denormals, conversion-boundary values, doubles at the edges the x87 fast
   path must hand back, 64-bit-significand registers, and seeds that change
   the precision or rounding mode), runs each through the interpreter and the
   JIT, and requires identical final states (x87 status masked only for DE).
   67 000 runs, 0 differences. Runs under qemu in CI and natively on the
   Apple-silicon job.
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

### On the device (the only numbers that mean anything in absolute terms)

`xc_bench` through MemProbe, iPad Air 11" (M3), build bc628c0 — 200 000
iterations per loop, guest instructions per second:

| loop | interpreter | dynarec | speedup |
|---|---|---|---|
| integer (add/xor/imul + branch) | 9.8 MIPS | **1678 MIPS** | 172× |
| sse2 (add/mul/sub/div sd) | 12.8 MIPS | **853 MIPS** | 66× |
| x87 (fld/fmul/fadd/fstp m64, 53-bit) | 12.6 MIPS | **670 MIPS** | 53× |

The macOS CI runner measures 1770 MIPS on the integer loop, so the iPad is
within 6% of a desktop Apple-silicon part. For scale: the 32-bit games this
targets were built for single cores in the low gigahertz, and a dynarec
retiring 0.7–1.7 billion guest instructions per second on the *phone-class*
part is the number that makes them plausible at all.

x87 lowering on that device: the golden replay compiled 544 x87 instructions
natively against 138 left to the interpreter, and `nbody32.exe` — a real
mingw-w64 Windows guest whose float work is entirely x87 — came out at **455
lowered against 17 called out**, so 96% of its FPU work runs as NEON doubles.

### Under qemu

Absolute numbers are meaningless here, only the ratios matter:

| workload | interpreter | JIT + chaining + SSE/x87 |
|---|---|---|
| 30 M-iteration integer loop | 1× | 8.7× |
| busybox `sha256sum` of a 3.5 MB file (252 M instructions) | 3.82 s, 968 k callouts | 1.35 s, 1.4 k callouts |
| `tests/guest/nbody 300000` (SSE2 double-precision n-body) | 70.2 s | 1.3 s (54×) |
| `nbody32.exe 200000` (the same, built i686 = **x87** math) | 89.3 s | 2.1 s (43×) |

The 32-bit x87 build was 43× slower than the SSE2 build before x87 lowering
(every FLD/FADD/FMUL a callout); it is now within ~2.3× of it, the gap being
the range and stack guards. Output is byte-identical to the native x86 run in
every case. Real numbers come from the devices.

## What comes next, in order

1. **Registers live across links.** Chaining removed the dispatcher from the
   hot path; the remaining overhead in a hot loop is the store-on-exit /
   load-on-entry of guest registers at every block boundary. Keeping them in
   their host homes across a chained link (a per-block entry convention) is
   the next speed step.
2. **Indirect branch prediction.** RET and `jmp reg` still go through the
   dispatcher's hash lookup; an inline cache keyed by target address would
   cover most of them.
3. **Faults.** Map host SIGSEGV/SIGBUS inside guest code to guest faults with
   the interpreter's `XC_STOP_FAULT` semantics.
4. **A code cache on disk**, keyed by the block's bytes, so later launches
   start compiled (see `CPU-CORE.md`, "Could we just compile it instead").
