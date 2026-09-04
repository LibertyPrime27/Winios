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

Implemented, all checked against silicon (see below):

- **Integer.** `MOV`/`MOVZX`/`MOVSX`/`MOVSXD`, `LEA`, `XCHG`, `BSWAP`; the ALU
  family (`ADD ADC SUB SBB AND OR XOR CMP TEST INC DEC NEG NOT`); shifts,
  rotates, `RCL`/`RCR`, `SHLD`/`SHRD`; `MUL`/`IMUL` (all forms), `DIV`/`IDIV`
  with `#DE`; `CBW`…`CQO`; `PUSH`/`POP`/`LEAVE`; `JMP`/`Jcc`/`CALL`/`RET`/
  `JRCXZ`/`LOOP*`; `CMOVcc`/`SETcc`; the whole string family with `REP`/
  `REPE`/`REPNE`; `BT`/`BTS`/`BTR`/`BTC`, `BSF`/`BSR`/`TZCNT`/`LZCNT`/`POPCNT`;
  `CMPXCHG`/`XADD` (one core, so plain read-modify-write is atomic); `LAHF`/
  `SAHF`/`CLC`/`STC`/`CMC`/`CLD`/`STD`; `CPUID` (a fixed SSE2-class CPU with
  no AVX/BMI, so libraries pick the code paths that exist here), `RDTSC`
  (deterministic), fences and prefetches as no-ops, CET shadow-stack ops as
  the no-ops they are on hardware without CET.
- **SSE / SSE2.** All the moves (`MOVAPS`…`MOVQ`/`MOVD`, the merge semantics
  of `MOVSD`/`MOVSS`, `MOVLPS`/`MOVHPS`), packed integer arithmetic including
  the saturating and multiply forms, compares, shifts, shuffles, unpacks and
  packs, `PMOVMSKB`, scalar and packed single/double arithmetic, `MIN`/`MAX`
  with x86's "second operand wins" NaN rule, `COMIS*`/`UCOMIS*`, the `CMP`
  predicates, every conversion, `LDMXCSR`/`STMXCSR`. NaN propagation follows
  the SDM (first NaN operand, quieted; invalid operations yield x86's
  *negative* default NaN), and the MXCSR exception flags are maintained:
  IE/ZE/OE/UE/PE come back from the host FPU via `fenv`, DE from the inputs.
- **x87.** The full stack machine in `core/src/interp_x87.h`: loads and stores
  in every width, the arithmetic group in all operand forms, `FCOM*`/`FUCOM*`/
  `FCOMI*`/`FTST`/`FXAM`, `FCMOVcc`, `FPREM`/`FPREM1` with the quotient bits,
  `FSCALE`, `FXTRACT`, `FRNDINT`, `FSQRT`, control-word handling (precision
  control and rounding mode are honoured on every operation — D3D9 puts the
  FPU into 24-bit mode and Fallout 3 lives there), stack faults, and the
  environment/save-area formats (`FNSTENV`, `FNSAVE`, `FXSAVE`). Arithmetic is
  [Berkeley SoftFloat](https://github.com/ucb-bar/berkeley-softfloat-3)
  (BSD-3) with the 8086 specialisation, so the results are the x87's bits on
  any host. `FSIN`/`FCOS`/`FPTAN`/`FPATAN`/`F2XM1`/`FYL2X*` use the host's
  `long double`: real x87s disagree with each other in the last bits of
  these, and the test compares them with a tolerance.

Flags are computed eagerly and exactly, including the parts people get wrong:
`INC`/`DEC` preserve `CF`; a 32-bit `CMOVcc` zero-extends the destination even
when the move does not happen; shift-by-zero leaves flags untouched; `NEG`
sets `CF` from the operand rather than the result; `BT` with a register bit
offset and a memory operand adjusts the address.

Not yet: SSE3 and later (CPUID does not advertise them, so well-behaved code
does not use them), AVX, `CMPXCHG16B`, `FBLD`/`FBSTP`, segment-register loads,
anything privileged. Each is a case in the switch and a line in the difftest.

## 32-bit: tested against silicon too

Every 64-bit case has a 32-bit sibling table (`CASES32` in `cases_gen.py`,
155 cases): the same instructions with 32-bit registers and addressing, plus
the things only 32-bit code still uses -- `PUSHAD`/`POPAD`, `PUSHFD`/`POPFD`,
`ENTER`, `CMPXCHG8B`, `XLAT`, the BCD group, and x87 with `FLDCW` in D3D9's
24-bit mode. The native side runs them in **compatibility mode inside the
64-bit test process**: Linux keeps a 32-bit code segment (selector `0x23`) in
every process, a far return into it switches modes, and a far return back
lands in a 64-bit stub below 4 GB -- the same mechanism a 32-bit Windows
program runs under on a 64-bit kernel. The emulated side runs in 32-bit mode
over an arena with base 0, so guest and host addresses coincide and the
buffers compare directly. On the device the golden vectors replay over an
arena with a *nonzero* base, which is exactly how a 32-bit game runs on iOS.

## Running programs: `xrun`

`tools/xrun/xrun.c` loads a static x86-64 Linux ELF (`ET_EXEC` or static PIE),
builds the initial stack the way the kernel does (argv, envp, auxv with
`AT_PHDR`/`AT_RANDOM`/…), and services system calls at the `XC_STOP_SYSCALL`
boundary. Because guest addresses are host addresses, a pointer the guest
hands to `write()` goes straight to the host; the layer is an explicit
allow-list rather than a pass-through, and `brk`, `arch_prctl` (TLS) and the
signal/thread calls are handled in xrun rather than reaching the host kernel.

It runs `tests/guest/hello` (no libc), `tests/guest/libc_hello` (musl static:
TLS setup, `malloc` over `brk`, `printf`), and a stock glibc-static
`busybox`: `sh`, `awk` with floating point, `md5sum`, `sha256sum`, `sort`,
`gzip`, `printf %f` all produce output identical to native. That is the
milestone the roadmap below called "enough instructions to run real code".

The 32-bit half (`tools/xrun/linux32.c`) loads a static i386 ELF into a 4 GB
arena and services `int 0x80` with an explicit i386 syscall table -- pointers
rebased, `stat64`/`iovec`/`timespec`/`rlimit` converted to the 32-bit
layouts, `set_thread_area` setting the `%gs` base. A static i386 glibc
program (TLS through `%gs`, `brk`, x87 `long double` arithmetic, `printf`,
libm's `sin`/`exp`/`atan2`) produces output identical to native; the guest
test builds and runs one when a 32-bit toolchain is present. The Win32
loader will replace the ELF and syscall parts of this file and keep the
memory model.

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
4. Compare all sixteen GPRs, the flags under a per-case mask, XMM0-15 and
   MXCSR, the x87 stack (control word, status word under a mask, tags, and
   every valid register), and every byte of the data and stack regions.

The trampoline moves the whole FPU state with `FXRSTOR`/`FXSAVE` and keeps the
host's own state aside, so a case that leaves the x87 stack full or the
precision control at 24 bits does not poison the process running the test.

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

Vectors are regenerated by CI, not hand-edited. Transcendental x87 results are recorded from the emulator to 32 significant bits (libm versions differ in the last bit, and real x87s differ from each other), so their replay is a loose comparison. Memory-touching cases are
skipped on replay (the recorded addresses are host pointers that mean nothing
elsewhere); those stay covered by `difftest` on CI and `test_arena32` locally.

## Adding an instruction

1. Add the case in `xc_step` (or `do_sse` / `do_x87`).
2. Add a line to `CASES` in `tests/difftest/cases_gen.py` — assembly text,
   not bytes — and run it to regenerate `cases_gen.inc`. If the instruction
   leaves flags undefined, mask them and say why in a comment.
3. If it touches memory, make it touch `[rdi+…]` — `rdi` points at the shared
   data buffer, which is compared byte-for-byte.
4. If it is 32-bit-only or address-width sensitive, add a check to
   `test_arena32.c` too.

## "Could we just compile it instead of emulating?"

Yes — the dynarec *is* a compiler. It reads x86 machine code and emits ARM64
machine code, and once a block is translated the game runs as native ARM64
instructions. The interpreter is the reference semantics and the no-JIT
fallback, not the engine.

What is **not** possible is compiling a whole game ahead of time into a native
binary — a true port — without its source code. This is structural to x86
binaries, not a matter of effort:

- **Indirect control flow.** `jmp rax`, function pointers, C++ virtual calls,
  switch tables: the target exists only at runtime. Fallout's engine is made of
  these.
- **Code is indistinguishable from data** until it executes. Disassembling an
  arbitrary binary correctly is undecidable in general.
- **Code that does not exist yet.** DRM unpacks code at runtime; engines load
  DLLs on demand; mods load arbitrary ones.

Rosetta 2 is not pure ahead-of-time either; it carries a JIT fallback for
exactly these cases.

What we do instead is **cache translations**: on first run, blocks are compiled
as they are reached and the ARM64 written to disk; later launches load them and
start fast. That captures nearly all of AOT's benefit without its impossibility,
and it is a feature on top of the dynarec rather than a different design — so
the dynarec is built from the start so its output is relocatable and cacheable.

Wine itself is emulated in the current plan. Making it native ARM64 later
(built against a libc that calls our kernel emulation) is a real optimisation
path, but it comes after games run: a game's frame time is dominated by its own
code and by graphics, and both of those will be native from day one.

## What comes after this

In order, and each is gated by the previous:

1. ~~Enough instructions to run real code.~~ Done: `xrun` runs musl and glibc
   static binaries, busybox included.
2. **A Linux-syscall or Win32 personality on top of the `XC_STOP_SYSCALL`
   boundary.** `xrun` is the Linux one, host-only for now; the Win32 side is
   what Wine provides once Wine itself runs here.
3. **The ARM64 dynarec**, behind the same `xc_cpu`, checked against this
   interpreter the way this interpreter is checked against silicon.
