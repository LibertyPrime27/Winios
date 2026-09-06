# Winios

Run 32-bit and 64-bit Windows games on iOS. Sideloaded, JIT-enabled, maximum performance.

**Targets:** 3d games, EX: Fallout New Vegas and Fallout 4

## Status

Running real Windows executables on an iPad, through a from-scratch x86 core
and dynarec, with Direct3D shaders compiled to Metal. Not a game yet — no
threads, no DLL loading, no D3D API surface — but every layer underneath one
is now working on hardware.

| Piece | State |
|---|---|
| Architecture (why two engines, what blocks 64-bit) | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| JIT acquisition and probing design | [`docs/JIT-DESIGN.md`](docs/JIT-DESIGN.md) |
| CI — unsigned IPA on every push, core tests on Linux | [`.github/workflows/ios-build.yml`](.github/workflows/ios-build.yml) |
| **Win32 layer** — PE32/PE32+ loader, TEB/PEB, kernel32 + msvcrt on the host, imports as `int3` stubs | [`docs/WIN32.md`](docs/WIN32.md), [`win32/`](win32), [`tools/winrun/`](tools/winrun) — **runs real Windows executables**: a three-import hello, a full mingw-w64 CRT program (TLS callbacks, malloc, printf, exit code) and the n-body benchmark, each as 32- and 64-bit, output byte-identical to the Linux build; interpreter, qemu JIT, Apple-silicon CI — and **on the iPad itself, 6/6 through MemProbe** |
| **dynarec** — x86 basic blocks → ARM64 code, block chaining that carries guest registers across the link, SSE/SSE2 on NEON, x87 (53-bit precision) on NEON doubles, lazy flags, callouts to the interpreter for the rest | [`docs/DYNAREC.md`](docs/DYNAREC.md), [`core/src/jit/`](core/src/jit) — **passes all 2388 silicon vectors on the M3 iPad and the A19 Pro iPhone** (and under qemu-aarch64 / Apple-silicon CI); JIT-vs-interpreter differential over every difftest case, 67 000 runs identical; **on the iPad: ~1600 MIPS integer, ~820 SSE2, ~650 x87 — roughly 165× / 65× / 53× the interpreter** |
| **MemProbe** — the device app: CPU vectors, on-device benchmark, GPU probe, JIT bless, **real Windows .exe**, memory ladder | [`docs/MEMPROBE.md`](docs/MEMPROBE.md), [`tools/memprobe/`](tools/memprobe) — one button per probe; runs the six mingw-w64 guests through the PE loader on the device itself, and is where the only non-qemu performance numbers come from |
| **xcore** — one CPU core for 32- and 64-bit x86, interpreter + differential tests | [`docs/CPU-CORE.md`](docs/CPU-CORE.md), [`core/`](core) — full baseline x86 + SSE2 + x87 in both 64- and 32-bit mode, 542 cases verified against silicon; `xrun` runs static Linux binaries (musl, glibc, busybox; i386 glibc through the 4 GB arena) |
| **JIT on iOS 26 TXM hardware** — bless protocol, `jit_arena` | **working on device** (M3 iPad): [`docs/JIT-DESIGN.md` §1a](docs/JIT-DESIGN.md) |
| Process model | **decided**: single process, emulated Linux process model (no extension) — `ARCHITECTURE.md` top note |
| **GPU binding probe** — d12mt's heap-is-an-argument-buffer model for D3D9, D3D11 and D3D12, tested on device | **27/27 PASS on the M3 iPad and the A19 Pro iPhone Air** — [`docs/MEMPROBE.md`](docs/MEMPROBE.md) |
| **d12mt** — Direct3D → Metal compiler: D3D12 root signatures → argument buffers; DXIL (D3D12), SM5 (D3D11) and SM3 (D3D9) → MSL | **working**, own public repo [`LibertyPrime27/d12mt`](https://github.com/LibertyPrime27/d12mt), vendored at [`gpu/d12mt`](gpu/d12mt); CI compiles its MSL for iOS with Apple's Metal compiler |

## The two things to know before reading anything else

1. **32-bit and 64-bit are different engines on iOS, and 64-bit is the architecturally cleaner one.** Darwin reserves the low 4 GB of address space and won't give it back; a 32-bit guest needs it, a 64-bit guest doesn't. Details in `ARCHITECTURE.md` §0.
2. **Everything 64-bit depends on one unmeasured number:** whether an iOS app extension can hold ~3 GB resident. Apple's docs say no; LiveContainer's author says yes. `tools/memprobe` is the app that settles it — build it, sign it, run it, read `docs/MEMPROBE.md` for how to interpret the number.

## Build

Pushes to `main` build an unsigned `.ipa` on GitHub's `macos-26` runners and attach it as a workflow artifact. Sign it locally with Sideloadly, AltServer or SideStore. The repo is public because macOS runner minutes are free only for public repos.

The emulator core is kept platform-independent so most of it builds and tests on a Linux runner in minutes rather than a macOS runner in tens of minutes.

## Measured on hardware (Sept 2026)

Both columns are build `bb13301`.

| | iPad Air 11" (M3), iPadOS 26.3.1 | iPhone Air (A19 Pro), iOS 27.0 |
|---|---|---|
| CPU core vs x86 silicon (integer, SSE2, x87; 64- and 32-bit mode replayed on ARM64) | **2388/2388 match** | **2388/2388 match** |
| Dynarec: the same vectors through JIT-emitted ARM64 in the blessed arena | **2388/2388 match** — 401 blocks, 61 KB | **identical**: 401 blocks, 61 KB, same callout count |
| JIT (TXM bless protocol) | **working** | **working** |
| GPU: D3D9 / D3D11 / D3D12 binding model on Metal | **27/27 PASS** | **27/27 PASS** |
| Usable memory, app process | ≈8169 MB (ladder climbed to 7872 MB held) | ≈6126 MB (early read; full ladder pending) |
| Physical RAM | 7.5 GB | 11.5 GB |
| **Speed, dynarec vs interpreter** (`xc_bench`) | integer **~1600 MIPS** (~165×), sse2 **~820 MIPS** (~65×), x87 **~650 MIPS** (~53×) | pending |
| **Windows executables** (PE loader + kernel32/msvcrt + dynarec, on device) | **6/6 PASS** — hello, crt and nbody as PE32 and PE32+ | pending |
| x87 lowered onto NEON, `nbody32.exe` | **455 of 472** (96%) | pending |

The headline: **real Windows executables run on the iPad**, and the dynarec
retires between 0.7 and 1.7 billion guest instructions per second doing it.
Two more things that table says. The dynarec compiles **bit-for-bit identically on
A-series and M-series** — same block count, code size and callout count — so
compilation depends on the guest code and not the host chip. And the memory
ceiling is set by OS policy rather than RAM: the iPhone has 4 GB more physical
memory than the iPad and a ~2 GB lower per-app limit, which makes the phone,
not the tablet, the target to size the guest heap against.

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
