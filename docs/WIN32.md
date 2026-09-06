# The Win32 layer: Windows executables on xcore

**Where:** `win32/` (`pe.c` loader, `kernel32.c`, `msvcrt.c`, `w32.h`),
`tools/winrun/winrun.c` (runtime and command-line driver),
`tests/win32/` (mingw-built .exe guests with recorded output).

    winrun [-v] program.exe [args...]

This is the step from "a CPU that runs x86 code" to "a machine that runs
Windows programs": load a PE image, give it a process to live in, and answer
every DLL call it makes. It is the same idea Wine proved — the program's own
code runs unchanged; the operating system underneath it is reimplemented —
built here on our CPU instead of the host's.

## How an import becomes a host call

Every import the executable names resolves to a 16-byte **stub** in a guest
page whose first byte is `int3`. Calling the import stops the CPU with RIP one
past the `int3`; the runtime finds the stub, reads the arguments where the
calling convention left them (rcx/rdx/r8/r9 and the stack on x64; the stack on
x86), runs the host implementation, writes the result to rax (edx:eax for
64-bit results on x86, xmm0 / ST(0) for doubles), and returns to the caller —
popping the arguments too for stdcall. No guest code is generated for any of
this and the JIT is unaware of it: an `int3` is a breakpoint to the CPU, so
the same mechanism works through the interpreter and through compiled blocks.

Imports of *data* (msvcrt's `_iob`, `__initenv`, `_fmode`...) resolve to guest
memory the runtime owns; imports nobody implements resolve to a stub that
names itself when — if — it is called, so a missing function is a one-line
message rather than a crash.

Guest callbacks go the other way through `w32_call_guest()`: push a
return-to-host stub, set up the frame, re-enter the run loop until that stub
is hit. TLS callbacks, `_initterm`, `atexit` handlers and `qsort` comparators
all run this way, nested to any depth.

## Both bitnesses, one implementation

A PE32 image gets the 4 GB arena (guest address = base + zext32), a PE32+
image the identity mapping — the two memory models `xrun` established.
Every API implementation is written once against `w32_arg(i)`, `w32_ret()`,
`W32P()` and a few size helpers, so `WriteFile` or `printf` do not know which
ABI called them. `hello32.exe`/`hello64.exe`, `crt32.exe`/`crt64.exe` and
`nbody32.exe`/`nbody64.exe` are the same sources built for both.

Two facts about the host shape guest memory. Guest pages are never executed
by the host — the interpreter reads them, the dynarec translates them — so
they are mapped read/write whatever `PAGE_EXECUTE_*` the image or
`VirtualAlloc` asked for; Apple silicon refuses RWX mappings outside
`MAP_JIT` and iOS refuses them outright (the first Apple-silicon CI run of
this layer died on exactly that: the stub page came back unmapped and the
first `int3` was written to address 0). And the guest's 4 KB pages are a
fiction on Apple silicon, whose kernel maps in 16 KB units and rejects
`mmap(MAP_FIXED)`/`mprotect` on anything less aligned; `w32_alloc*`,
`VirtualFree` and `VirtualProtect` widen guest ranges to host pages
(`w32_host_page()`), and the Linux CI job runs the suite once more with
`WINRUN_HOST_PAGE=16384` so the 16 KB path is exercised on every push, not
only on the Mac.

## What the process looks like from inside

TEB (gs on x64, fs on x86) with stack limits, Self, ClientId, LastErrorValue,
TLS slots and ThreadLocalStoragePointer; PEB with ImageBaseAddress,
ProcessHeap, NumberOfProcessors and OS version 10.0.19045. Static TLS from the
image's TLS directory. A 1 MB stack whose bottom frame returns into an exit
stub. `C:\xcore\` is the directory the executable came from; the command line
is quoted the Windows way; an environment block with the usual variables.
Three standard handles (4, 8, 12) onto the host's stdio.

`kernel32`: process and module queries, `GetProcAddress`/`LoadLibrary` onto
the built-in DLLs, memory (`VirtualAlloc/Protect/Query/Free`, heaps), TLS,
critical sections and SRW locks as no-ops (one thread), files and console,
time, code pages and the MultiByte/WideChar conversions, `IsProcessorFeature-
Present` reporting SSE through SSE4. `msvcrt`: the CRT startup protocol
(`__getmainargs`, `_initterm`, `__set_app_type`, `_onexit`...), heap, the
string family on guest memory, stdio on the standard streams, and the printf
family with a formatter that walks the *guest's* va_list (8-byte slots on x64,
4-byte slots with 8 for doubles and 64-bit integers on x86) and formats each
conversion through the host. `VirtualProtect` tells the block cache to drop
compiled code for pages that become writable.

## Verified

`tests/win32/run.sh` runs six executables and compares stdout and exit code
with recordings: the three-import `hello`, the full mingw-w64 CRT program
(`crt.c`: TLS callbacks, `__getmainargs`, `_initterm`, malloc/free, `sqrt`,
`printf`, `snprintf`, exit code) and the n-body benchmark, each as PE32 and
PE32+. The 64-bit n-body output is byte-identical to the Linux build of the
same source (`tests/guest/nbody`), which is byte-identical to native x86. The
suite runs on the x86 runner (interpreter, 4 KB and simulated 16 KB pages),
under `qemu-aarch64` (JIT) and natively on the Apple-silicon CI job (JIT,
16 KB pages). `winrun` builds on Linux and macOS and will build for iOS
unchanged: it is plain C over `mmap`. A host crash prints the guest RIP/RSP,
the runtime's map (image, stubs, TEB, stack, heap) and a host backtrace, so a
CI log is enough to start from.

## Measured, and what it says about the road ahead

`nbody64.exe 300000` through the JIT under qemu: 1.26 s — the same as the
Linux binary, because it is the same SSE2 code. `nbody32.exe 300000`: 102 s.
The i686 mingw compiler emits **x87** for `double`, and x87 is a callout to
the SoftFloat interpreter today: twelve million `fsubr`, seven million
`faddp`, seven million `fxch`. Fallout 3 and New Vegas are 32-bit programs
compiled with the MSVC of their day, which also defaults to x87 for scalar
float — so **x87 in the dynarec is the next performance item**, ahead of
anything else: FLD/FST/FXCH/FADD/FSUB/FMUL/FDIV/FCOMI on the host's doubles
when the precision control is 53 bits (or floats at 24, which is what D3D9
sets), with SoftFloat kept for the 64-bit-mantissa mode and the
transcendentals.

## What is deliberately not here yet

Threads (`CreateThread`/`_beginthreadex` report failure), structured
exception handling (a guest fault ends the run), real DLL loading (only the
built-in DLL surface exists; a game's own DLLs need the loader to map them
and resolve their exports — the PE code is there, the export-table walk is
not), registry, and everything user32/gdi32 beyond `MessageBoxA`. Each of
those is a defined next step, not a design gap: the stub mechanism, the two
memory models and the calling-convention helpers are the parts that had to be
right first, and they are the same parts the D3D-to-Metal layer will plug
into as `d3d9.dll` / `d3d11.dll` / `d3d12.dll`.
