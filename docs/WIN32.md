# The Win32 layer: Windows executables on xcore

**Where:** `win32/` (`pe.c` loader, `kernel32.c`, `msvcrt.c`, `com.c`, `d3d9.c`, `raster.c`, `w32.h`),
`tools/winrun/winrun.c` (runtime and command-line driver),
`tests/win32/` (mingw-built .exe guests with recorded output).

    winrun [-v] [-L dlldir] program.exe [args...]

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

## DLLs the guest brings with it

An import names a DLL, and two different things can satisfy it.

**A DLL we implement on the host** — kernel32, msvcrt, ntdll, user32 — resolves
to stubs, as above. The host list always wins: a program shipping its own
`kernel32.dll` gets ours, which is the whole point of the exercise.

**A real PE DLL sitting next to the executable** is loaded the same way the
executable is — mapped, relocated, its own imports resolved, its TLS block
allocated — and the import resolves through its **export directory**. Exports
resolve by name (the name table is sorted, so it is a binary search) or by
ordinal, and an export whose address lands inside the export directory is a
**forwarder**: the bytes there are `"otherdll.SomeFunc"`, which is resolved
again from the start. `LoadLibrary`, `GetProcAddress` (by name and by
ordinal), `GetModuleHandle`, `GetModuleFileName` and `FreeLibrary` all work
against the same module table. `-L dir` adds a directory to search; otherwise
it is the executable's own directory and the working directory.

Two ordering rules matter, and both are observable in the test:

- **Loading is recursive and cycle-safe.** A module is registered in the table
  *before* its imports are resolved, so a dependency loop finds the half-built
  module instead of looping forever.
- **DllMain runs in dependency order** — which is *not* table order. Because a
  module is registered before its dependencies are, the table has dependents
  first; each module also records the order in which it *finished* loading,
  and that is what the attach walk sorts by. So `sub.dll` is attached before
  `mid.dll`, and `mid.dll`'s DllMain can call into `sub.dll`. Getting this
  backwards is the first bug this code had, and the test now names the order
  explicitly rather than checking a return value.

A DLL loaded later by `LoadLibrary` is attached before that call returns, so a
plugin host, a mod loader, or a late-bound `d3d9` sees an initialised module.
Nothing is ever unmapped: `FreeLibrary` decrements a count and returns
success, because an emulator that keeps a dead image mapped is strictly safer
than one that unmaps it under a live return address.

## COM, and d3d9.dll

Direct3D is not a set of exported functions. `dev->lpVtbl->Clear(dev, ...)`
compiles to an indirect call through a slot the guest computes itself, so an
interface is only usable if the **vtable lives in guest memory** and every
slot in it is something the guest can call — which is what the import stubs
already are. `com.c` builds a vtable per interface: one `int3` stub per slot,
landing in the same dispatcher an imported function does, with `this` as
argument 0 (pushed first for x86 stdcall, `rcx` on x64 — the same place in
both). An object's state lives in guest memory right after its vtable pointer
and reference count, so the host side holds nothing and there is nothing to
free or reset.

`d3d9.c` is that mechanism plus one export. `Direct3DCreate9` returns an
`IDirect3D9`; `CreateDevice` returns an `IDirect3DDevice9` with a back buffer
allocated in **guest memory**, which is what makes both ends work — the guest
can lock and read it directly, and the host can hand the same bytes to Metal
without a copy through a second address space. `Clear`, `BeginScene`,
`EndScene`, `Present`, `GetBackBuffer` and the surface's `LockRect` are real;
the state setters accept and ignore, because there is nothing to draw with
yet. Where a presented frame goes is a host callback (`w32_set_present`):
nothing by default, a `.ppm` per frame under `WINRUN_PRESENT_PPM=<prefix>`,
and on the device a Metal texture MemProbe draws full screen.

Stopping a guest that is drawing frames goes the other way, and deliberately
does not reach into it: `w32_d3d9_device_lost(1)` makes `Present` and
`TestCooperativeLevel` return `D3DERR_DEVICELOST`, which is what a real driver
returns when the display mode changes or the machine sleeps. A program that
checks its `Present` result — every game does — leaves its own loop and exits
normally. `test_winrun_lib` runs `d3dloop32.exe` with no frame limit, trips
the flag after five frames, and requires that it stopped there and exited 0.

**Vtable order is load-bearing.** A slot in the wrong place is an indirect
call to the wrong function, so the tables carry explicit slot numbers taken
from `d3d9.h`, and every slot that is not implemented gets a stub that names
its interface and index. That is not decoration: the first version of
`IDirect3DDevice9` was missing `CreateDepthStencilSurface` at slot 29, so
everything after it was one out, and what came back was
`call to unimplemented IDirect3DDevice9::slot 57` — the guest asking for
`SetRenderState` at 57 while the table had it at 56.

### Drawing

`CreateVertexBuffer` gives the guest a buffer in **guest memory** that it
locks and fills itself, `SetStreamSource` and `SetFVF` bind it, and
`DrawPrimitive` / `DrawPrimitiveUP` assemble triangles from it — lists, strips
and fans. One vertex format is accepted: `D3DFVF_XYZRHW | D3DFVF_DIFFUSE`,
a position already in screen space plus a colour. That is deliberate: it
isolates the parts that had never run before (vertex fetch, primitive
assembly, rasterization, writing the render target) from the parts that do not
exist yet (the world/view/projection matrices, lighting, texture stages).
Anything else is refused with a message rather than drawn wrong.

The pixels come from `raster.c`, a **reference rasterizer** — not the renderer.
Its job is to make a draw call checkable without a GPU: the same guest that
draws a triangle on the iPad draws it on a Linux runner and under qemu, and
the frame checksums have to match. Every other layer here is verified that way
and a GPU-only draw path would have been the first with no coverage at all.
Determinism is therefore designed in rather than hoped for: vertex positions
become 28.4 fixed point by multiplying by 16 (exact for a float, being a power
of two) and everything after that is integer edge functions and integer
barycentric colour, so there is no floating-point rounding to differ between
an x86 runner, qemu on aarch64 and an M3, and nothing a compiler can contract
into an fma. `d3ddraw.exe` produces checksum `acde04d16c79039c` in all of
them, and identically as PE32 and PE32+.

What is still missing is the GPU. d12mt already compiles D3D9 SM3 shaders to
MSL and passes 27/27 on both devices; pointing the draw path at it, so the
back buffer becomes a Metal render target instead of memory the CPU writes, is
the next step. The rasterizer stays as the reference the GPU path is checked
against, and as the fallback where no GPU path is available.

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

`tests/win32/run.sh` runs eighteen executables and compares stdout and exit code
with recordings: the three-import `hello`, the full mingw-w64 CRT program
(`crt.c`: TLS callbacks, `__getmainargs`, `_initterm`, malloc/free, `sqrt`,
`printf`, `snprintf`, exit code), the n-body benchmark, the loader test
`dlltest`, and the four Direct3D 9 programs `d3dtest`, `d3dframe`,
`d3dloop` and `d3ddraw`, each as PE32 and PE32+.

`d3dtest` calls Direct3D 9 the way a game starts up — `Direct3DCreate9`,
`GetAdapterIdentifier`, `CreateDevice`, `Clear`, `Present`, then
`GetBackBuffer` and `LockRect` to read the pixels back — and every one of
those goes through a COM vtable, so it is also what keeps the slot numbers
honest. `d3dframe` produces an actual picture: it locks the back buffer and
draws a gradient, a disc and a checkerboard with integer arithmetic (so PE32
and PE32+ produce byte-identical output), presents it, and prints an FNV-1a
checksum of the frame. `d3dloop` is the same thing as a loop — the shape a
game's frame loop has — animating a disc across the screen and either stopping
at a frame count (which is how it is tested) or running until the device is
lost (which is how the app runs it). All are recorded like the others, and the
same checksums come out of the interpreter, the qemu JIT and the device.

`dlltest` is built as a chain — `dlltest.exe` statically imports `mid.dll`,
which statically imports `sub.dll`, and `late.dll` is in nobody's import table
and reachable only through `LoadLibrary`. Each DllMain writes its name into a
log inside `sub.dll`, so the program prints the order the loader actually used
(`sub mid exe`, then `late` when it is loaded) rather than trusting a return
code. It also checks `GetProcAddress` by name and by ordinal against each
other, that a DLL imported twice is one image and not two, that a forwarded
export lands on the real function in the other DLL, that a name that is not
exported gives NULL, and that the module stays callable after `FreeLibrary`. The 64-bit n-body output is byte-identical to the Linux build of the
same source (`tests/guest/nbody`), which is byte-identical to native x86. The
suite runs on the x86 runner (interpreter, 4 KB and simulated 16 KB pages),
under `qemu-aarch64` (JIT) and natively on the Apple-silicon CI job (JIT,
16 KB pages). `winrun` builds on Linux and macOS and will build for iOS
unchanged: it is plain C over `mmap`. A host crash prints the guest RIP/RSP,
the runtime's map (image, stubs, TEB, stack, heap) and a host backtrace, so a
CI log is enough to start from.

## Measured

`nbody64.exe 300000` through the JIT under qemu: 1.26 s — the same as the
Linux binary, because it is the same SSE2 code. `nbody32.exe`, built i686 so
the compiler emits **x87** for `double`, used to take 102 s (every
FLD/FADD/FMUL a callout to the SoftFloat interpreter). With x87 lowered onto
NEON doubles (see the x87 section of `docs/DYNAREC.md`) it is 2.1 s for
200000 iterations — 43× faster, and within ~2.3× of the SSE2 build. Windows
processes start in 53-bit precision (the mode the lowering handles natively),
which is what `winrun` and a real MSVC CRT both set; Fallout 3 and New Vegas
are 32-bit MSVC programs that do their scalar float on the x87 stack, so this
is the path they run on. `_controlfp`/`_control87` are implemented on the
guest's real FCW/MXCSR so the CRT actually reaches that mode.

## Where a guest's files are

A Windows program opens `C:\...` for what it installed and a bare relative name
for what sits beside its executable, and both have to land somewhere real or a
game cannot find its own data.

- `C:\...` resolves under a **drive root** the host chooses — the app points it
  at its own storage, `winrun` takes `-C dir` or `WINRUN_DRIVE_C`. With no root
  set (the command-line default) an absolute path falls back to being relative,
  which is what the older behaviour was.
- A relative path resolves against **the executable's own directory**, because
  that is the working directory a Windows program is started in. Not the host
  process's cwd, which on iOS points nowhere useful.
- `C:\xcore\...` is still the executable's directory, because that is the
  location `GetModuleFileName` has always reported and the recorded guest
  output depends on it.

`pathtest.exe` locks all three down, including a path that is deliberately not
there — "not found" is a result too, and a file layer that cheerfully opens
anything would pass a weaker test.

## What a program needs that we do not have

    winrun -imports program.exe

Loads the image, resolves every import, and prints the ones nothing could
satisfy — without running an instruction. Each unresolved import already
becomes a stub carrying its own name, so the list falls out of the loader for
free, and it turns "what should we build next" from a guess into a list taken
from the binary. Pointed at a real game, it *is* the roadmap.

`tests/win32/gamelike32.exe` is a fixture for it: a program that does what a
game does in its first few seconds — makes a window and pumps messages, starts
a thread, takes a lock, reads the registry, walks a directory, memory-maps a
file, times a frame, queries the display. It is deliberately not in the test
suite, because it cannot run yet. What it reports today:

    59 imports resolved, 17 missing
      advapi32.dll (1)   RegOpenKeyExA
      kernel32.dll (6)   CreateThread, CreateFileMappingA, MapViewOfFile,
                         FindFirstFileA, FindNextFileA, FindClose
      user32.dll   (9)   RegisterClassA, CreateWindowExA, ShowWindow,
                         PeekMessageA, TranslateMessage, DispatchMessageA,
                         DefWindowProcA, GetSystemMetrics, EnumDisplaySettingsA
      winmm.dll    (1)   timeGetTime

Worth reading closely, because it corrects the obvious assumption. "No threads"
is not quite right: `EnterCriticalSection`, `CreateEventA`, `SetEvent`,
`InterlockedIncrement`, `WaitForSingleObject`, `QueryPerformanceCounter` and
`CreateFileA` all resolve already — only `CreateThread` itself is absent. The
window and its message pump are the larger hole, and file enumeration and
memory-mapped files are what a game reaches for to load its archives.

## Carrying on past what we do not have

    winrun -k program.exe          # and -t seconds, for one that will not stop

Normally, calling an unimplemented import ends the run: it names the function
and exits 127. That is right for the test suite — a guest that silently
half-works is worse than one that stops — but it is the wrong tool for finding
out what a real program needs, because you learn one name per run.

`-k` logs the call, returns zero, and carries on. One run then names
*everything* the program needed. `gamelike32.exe` reaches its last line and
reports sixteen missing functions in the order it called them.

The obstacle was the x86 calling convention: a stdcall callee pops its own
arguments, and an import table gives a name and nothing else. Guess the count
and the caller's stack is corrupt somewhere far away. `win32/stdcall_args.c` is
generated from mingw-w64's import libraries, which decorate stdcall imports as
`_Name@bytes` — 9817 functions across the DLLs a Windows program is likely to
name. A name that is not in the table still ends the run, because "I cannot
return from this safely" is worth saying rather than guessing. On x64 there is
no callee-pop, so anything can be returned from.

**Returning zero is a lie, and sometimes a consequential one.** Zero means
failure for most of the API but success for some (`RegOpenKeyEx` returns
`ERROR_SUCCESS`), and `FindFirstFile` fails with `INVALID_HANDLE_VALUE`, not
zero — which is why `gamelike` goes on to call `FindNextFile` and `FindClose`
on a handle it should have known was bad. For discovery that is fine and even
useful; it is not a way to run anything for real, and `-k` is off by default
for that reason.

## When something goes wrong

Any abnormal end — a fault, an int3 that is not one of ours, the time limit,
the Stop button — and any clean end that leaned on functions we do not have,
prints a run report: the reason, the guest's registers, the instructions at
RIP, which module RIP is inside and at what offset, the module map with the
stack and TEB, and everything called that is not implemented.

It is written for a clean exit too, because "exited 0 having called nine
functions that returned nothing" is also a diagnosis — and because the person
reading it is usually not the person who ran it. A report that only appears on
a crash is a report you cannot ask for.

`w32_request_stop()` ends a run from another thread; it is checked between
execution slices, so nothing is interrupted mid-instruction. `-t seconds` is
the same thing on a timer, which is what stops a runaway program from wedging
the app.

## What is deliberately not here yet

Threads (`CreateThread`/`_beginthreadex` report failure), structured
exception handling (a guest fault ends the run), the registry, and everything
user32/gdi32 beyond `MessageBoxA`.

Within the loader specifically: `DLL_PROCESS_DETACH` is never sent (nothing is
ever unloaded and the process exits without unwinding), `DLL_THREAD_ATTACH`
cannot exist until threads do, delay-loaded imports are left to the guest's own
helper, and `GetProcAddress` by ordinal works on guest DLLs but not on the
host-implemented ones, which have no ordinals to speak of.

Each of those is a defined next step, not a design gap: the stub mechanism, the
two memory models, the calling-convention helpers and now the module table are
the parts that had to be right first, and they are the same parts the
D3D-to-Metal layer will plug into as `d3d9.dll` / `d3d11.dll` / `d3d12.dll` —
which, now that a guest DLL can be loaded and its exports resolved, is the
next thing to build.
