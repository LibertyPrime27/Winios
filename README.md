<img src="docs/logo/winios-logo.png" alt="Winios" width="128" align="right">

# Winios

Run 32-bit and 64-bit Windows games on iOS. Sideloaded, JIT-enabled, maximum performance.

**Targets:** 3d games, EX: Fallout New Vegas and Fallout 4

## Status

Running real Windows executables on an iPad, through a from-scratch x86 core
and dynarec, with Direct3D shaders compiled to Metal. Not a game yet — the GPU
is not drawing through Metal yet, and there is no DirectInput and no sound
coming out — but an installer's dialogs now read the way they do on Windows,
and a Windows program creates a Direct3D 9 or Direct3D 11 device,
fills a vertex buffer, draws triangles from it, and the frame appears on the
iPad. D3D11 draws far enough to put a textured, coloured picture on screen;
what it does not do is run the game's own shaders, which is the difference
between a game that draws and a game that looks right.

`gamelike32.exe` — a fixture that does what a game does in its first few
seconds (window, message pump, thread, lock, registry, directory walk,
memory-mapped file, frame timer, display query) — now reports **76 imports
resolved, 0 missing** and runs to its last line with nothing stubbed out. That
is a floor, not a ceiling: what one program asks for is not what every program
asks for.

You can also bring your own: point the app at a folder or a `.zip` and it
unpacks it onto its C:, works out which of the executables is the game, and
tells you what that program needs before you run it. Or hand it a setup `.exe`
and it will identify the installer, run it in that family's silent mode, and
keep whatever appeared on the drive.

| Piece | State |
|---|---|
| Architecture (why two engines, what blocks 64-bit) | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| JIT acquisition and probing design | [`docs/JIT-DESIGN.md`](docs/JIT-DESIGN.md) |
| CI — unsigned IPA on every push, core tests on Linux | [`.github/workflows/ios-build.yml`](.github/workflows/ios-build.yml) |
| **Win32 layer** — PE32/PE32+ loader with real DLL loading (export tables, forwarders, DllMain ordering, LoadLibrary/GetProcAddress), COM vtables in guest memory, **real text** (a TrueType rasterizer with a compiled-in font, so a dialog reads like Windows and still checksums identically on every machine), **DIB/icon/cursor decoding** out of the resource directory, **the common controls** (list view, tree view, tabs, status bar, trackbar, SysLink) and a **RichEdit that shows the licence** via `EM_STREAMIN`, **visual styles** (`uxtheme.dll` and the themed look), **`d3d9.dll`** (textures, vertex declarations, the transform pipeline, alpha blending, render targets, indexed draws — the sprite path a 2D game needs), **`d3d11.dll` + `dxgi.dll`** (device, immediate context, swap chain, input layouts, DXBC signature parsing, indexed and instanced draws; twelve vtables whose slot counts are `_Static_assert`ed against the D3D11 headers), a virtual `C:\` with wildcard directory walks and memory-mapped files, a **persistent registry**, **structured exception handling** (the 32-bit `fs:[0]` chain, `RaiseException`, faults becoming guest exceptions), **a window, a message pump and keyboard/mouse input**, **XInput** and a DirectSound that initialises, **threads** (`CreateThread` and the CRT's own, critical sections, events, mutexes, semaphores, interlocked, TLS — real pthreads, one executing guest code at a time), **`shell32.dll`** and the file/directory/disk calls an installer makes, `.ini` files, TEB/PEB, kernel32 + msvcrt on the host, imports as `int3` stubs | [`docs/WIN32.md`](docs/WIN32.md), [`win32/`](win32), [`tools/winrun/`](tools/winrun) — **runs real Windows executables**: a three-import hello, a full mingw-w64 CRT program (TLS callbacks, malloc, printf, exit code), the n-body benchmark, a DLL-chain loader test, four Direct3D 9 programs, a Direct3D 11 program that builds its own DXBC and draws an indexed textured quad, and file, registry, exception, input, audio and threading tests, each as 32- and 64-bit, output byte-identical to the Linux build; interpreter, qemu JIT, Apple-silicon CI — and **on both devices, through the app** |
| **The importer** — bringing a program in from outside, in the two shapes it arrives in: **a game** (folder, `.zip` or self-extracting `.exe`, unpacked onto the virtual C: with its own DLL directory and the right executable picked out of the half-dozen a game folder holds) or **an installer** (identified from its bytes as Inno / NSIS / InstallShield / MSI / 7-Zip / WinRAR / zip SFX / Wise / Setup Factory, run in that family's silent mode because its window is comctl32 and comctl32 is not implemented, then the drive is diffed to find what it actually installed) | [`docs/WIN32.md`](docs/WIN32.md), [`tools/import/`](tools/import), [`tools/wimport/`](tools/wimport) — 53 checks with no guest, plus 26 end to end against a program that behaves like a silent install and **fails unless it was handed the right flags** |
| **dynarec** — x86 basic blocks → ARM64 code, block chaining that carries guest registers across the link, SSE/SSE2 on NEON, x87 (53-bit precision) on NEON doubles, lazy flags, **precise faults** (every guest access carries a recovery stub, so a fault mid-block reports the state the faulting instruction had), callouts to the interpreter for the rest | [`docs/DYNAREC.md`](docs/DYNAREC.md), [`core/src/jit/`](core/src/jit) — **passes all 2388 silicon vectors on the M3 iPad and the A19 Pro iPhone** (and under qemu-aarch64 / Apple-silicon CI); JIT-vs-interpreter differential over every difftest case, 67 000 runs identical; **on the iPad: ~1600 MIPS integer, ~820 SSE2, ~650 x87 — roughly 165× / 65× / 53× the interpreter** |
| **The app** (**winios** on the home screen) — a library of Windows programs on the device: **import one from Files as a game (folder, .zip, self-extracting .exe) or run it as an installer**, see what it needs, run it (loosely or strictly), watch it draw full screen; a **JIT status line** at the top (on iOS the dynarec needs a debugger-blessed arena once per launch, and without it everything silently runs interpreted), **Settings** for what the game is told the screen is, mouse sensitivity and the touch keys, and an **opt-in log** that records each run beside what the device is. The probe menu is gone — it was a development tool; the one part of it a person needs, turning the JIT on, is reached from Settings | [`docs/MEMPROBE.md`](docs/MEMPROBE.md), [`tools/memprobe/`](tools/memprobe) — one button per probe; runs the six mingw-w64 guests through the PE loader on the device itself, and is where the only non-qemu performance numbers come from |
| **xcore** — one CPU core for 32- and 64-bit x86, interpreter + differential tests | [`docs/CPU-CORE.md`](docs/CPU-CORE.md), [`core/`](core) — full baseline x86 + SSE2 + x87 in both 64- and 32-bit mode, 542 cases verified against silicon; `xrun` runs static Linux binaries (musl, glibc, busybox; i386 glibc through the 4 GB arena) |
| **JIT on iOS 26 TXM hardware** — bless protocol, `jit_arena` | **working on device** (M3 iPad): [`docs/JIT-DESIGN.md` §1a](docs/JIT-DESIGN.md) |
| Process model | **decided**: single process, emulated Linux process model (no extension) — `ARCHITECTURE.md` top note |
| **GPU binding probe** — d12mt's heap-is-an-argument-buffer model for D3D9, D3D11 and D3D12, tested on device | **27/27 PASS on the M3 iPad and the A19 Pro iPhone Air** — [`docs/MEMPROBE.md`](docs/MEMPROBE.md) |
| **d12mt** — Direct3D → Metal compiler: D3D12 root signatures → argument buffers; DXIL (D3D12), SM5 (D3D11) and SM3 (D3D9) → MSL | **working**, own public repo [`LibertyPrime27/d12mt`](https://github.com/LibertyPrime27/d12mt), vendored at [`gpu/d12mt`](gpu/d12mt); CI compiles its MSL for iOS with Apple's Metal compiler |

## The two things to know before reading anything else

1. **32-bit and 64-bit are different engines on iOS, and 64-bit is the architecturally cleaner one.** Darwin reserves the low 4 GB of address space and won't give it back; a 32-bit guest needs it, a 64-bit guest doesn't. Details in `ARCHITECTURE.md` §0.
2. **Everything 64-bit depends on one unmeasured number:** whether an iOS app extension can hold ~3 GB resident. Apple's docs say no; LiveContainer's author says yes. the app (`tools/memprobe`, installed as **winios**) is what settles it — build it, sign it, run it, read `docs/MEMPROBE.md` for how to interpret the number.

## Build

Pushes to `main` build an unsigned `.ipa` on GitHub's `macos-26` runners and attach it as a workflow artifact. Sign it locally with Sideloadly, AltServer or SideStore. The repo is public because macOS runner minutes are free only for public repos.

The emulator core is kept platform-independent so most of it builds and tests on a Linux runner in minutes rather than a macOS runner in tens of minutes.

## Measured on hardware (Sept 2026)

Both devices on build `d8af0a5`.

| | iPad Air 11" (M3), iPadOS 26.3.1 | iPhone Air (A19 Pro), iOS 27.0 |
|---|---|---|
| CPU core vs x86 silicon (integer, SSE2, x87; 64- and 32-bit mode replayed on ARM64) | **2388/2388 match** | **2388/2388 match** |
| Dynarec: the same vectors through JIT-emitted ARM64 in the blessed arena | **2388/2388** — 436 blocks, 87 KB | **identical to the digit**: 436 blocks, 87 KB, same callout count |
| JIT (TXM bless protocol) | **working**, 4 MB arena blessed | **working**, 1 MB so far |
| GPU: D3D9 / D3D11 / D3D12 binding model on Metal | **27/27 PASS** | **27/27 PASS** |
| **Windows executables** (PE loader, DLL loading, kernel32/msvcrt/d3d9, dynarec) | **12/12 PASS** | **12/12 PASS** |
| **Direct3D 9**: device, vertex buffer, DrawPrimitive, Present | **both frames match the recorded checksums** | **both match** |
| **Direct3D 11**: device, swap chain, input layout, indexed and strip draws | not yet run on hardware — the probe is in the app (button 10) and the frame checksum `311139ad` is identical on x86, under qemu-aarch64, and in both bitnesses | not yet run on hardware |
| x87 lowered onto NEON, `nbody32.exe` | **455 of 472** (96%) | **455 of 472** (96%) |
| **Dynarec speed** (`xc_bench`) | integer **4060 MIPS**, sse2 **868**, x87 **675** | integer **4162 MIPS**, sse2 **919**, x87 **682** |
| Usable memory, app process | **≈8161 MB** | **≈6117 MB** |
| Physical RAM | 7.5 GB | 11.5 GB |

The headline: **real Windows executables run on both devices**, one of them
now draws a Direct3D 9 frame that appears on the iPad's screen, and the
dynarec retires around four billion guest instructions per second doing it.

Three more things that table says.

**The dynarec compiles bit-for-bit identically on A-series and M-series** —
same block count, same code size, same callout count, on an M3 under iPadOS 26
and an A19 Pro under iOS 27. What it emits depends on the guest code and
nothing else, which is what makes one set of golden vectors meaningful across
the whole device matrix.

**A drawn frame is identical everywhere.** `d3ddraw.exe` fills a vertex buffer,
calls `DrawPrimitive`, and its frame checksums `acde04d16c79039c` on x86 with
the JIT, on x86 without it, under qemu-aarch64, on the M3 and on the A19 Pro —
and as both PE32 and PE32+. The rasterizer is integer by construction for
exactly this reason.

**The memory ceiling is OS policy, not RAM.** The phone has 4 GB more physical
memory than the iPad and a ~2 GB lower per-app limit, so the phone is what the
guest heap has to be sized against.

One number in that table is *not* comparable between the two columns or against
CI: the interpreter's MIPS, and therefore the dynarec-to-interpreter ratios.
Both devices interpret at ~9 MIPS where a macOS CI runner does 73 and a cloud
x86 container does 48, while the same devices out-run the CI runner on dynarec
output. That is unexplained — raising the thread QoS did not change it — and
`xc_bench` now prints a plain-C reference figure alongside the guest ones so the
next report can separate "this machine is slow" from "this emulator is slow".

## Inspiration and prior art

- **[StikJIT / StikDebug](https://github.com/StikDebug/StikJIT)** — the iOS 26 TXM JIT
  protocol (`brk #0xf00d`, debugger-blessed pages) is theirs. Our `jitarena.c`
  implements the app side of it. This is also what DolphiniOS and MeloNX use.
- **[LiveExec32](https://github.com/LiveContainer/LiveExec32)** (khanhduytran0,
  Apache-2.0) — runs 32-bit ARM iOS binaries on 64-bit iOS via Dynarmic. Different
  guest (ARM, not x86) and needs jailbreak-only entitlements, so no code is shared
  — but its loader-plus-syscall-bridge shape (guest binary in, trapped system
  calls marshalled to the host through a page table) is the pattern our Win32
  personality will follow. Its Dynarmic dependency also points at
  **[oaknut](https://github.com/merryhime/oaknut)** (MIT), a standalone ARM64
  emitter that is a strong candidate for our dynarec's code-emission layer.
- **[Boxedwine](https://github.com/danoon2/Boxedwine)** — showed a soft-MMU can
  make the low-4 GB problem disappear; we took the idea, not the code.
- **[Zydis](https://github.com/zyantific/zydis)** (MIT) — our x86 decoder, as a
  submodule.
- **[dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv)** (MIT, Valve)
  and **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** (Apache-2.0,
  Khronos) — the two halves of d12mt's shader pipeline. vkd3d-proton and MoltenVK
  proved them on every D3D12 game on Steam Deck and every Vulkan app on a Mac.

## Licensing

The 32-bit engine is built on [Boxedwine](https://github.com/danoon2/Boxedwine) (GPL-2.0). This project is therefore GPL-2.0 and ships source. See `ARCHITECTURE.md` §6.

## Credits

The Winios logo and app icon are by **MegaNoob101**.

The fonts the Win32 layer draws with are subsets of Liberation Sans and
Liberation Mono, under the SIL Open Font License 1.1 — see
[`third_party/liberation/OFL.txt`](third_party/liberation/OFL.txt).
