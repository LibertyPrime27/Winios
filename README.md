# Winios

Run 32-bit and 64-bit Windows games on iOS. Sideloaded, JIT-enabled, maximum performance.

**Targets:** 3d games, EX: Fallout New Vegas and Fallout 4

## Status

Planning and design. No shippable code yet.

| Piece | State |
|---|---|
| Architecture (why two engines, what blocks 64-bit) | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| JIT acquisition and probing design | [`docs/JIT-DESIGN.md`](docs/JIT-DESIGN.md) |
| CI — unsigned IPA on every push, core tests on Linux | [`.github/workflows/ios-build.yml`](.github/workflows/ios-build.yml) |
| **MemProbe** — the measurement that gates 64-bit | [`docs/MEMPROBE.md`](docs/MEMPROBE.md), [`tools/memprobe/`](tools/memprobe) |
| **xcore** — one CPU core for 32- and 64-bit x86, interpreter + differential tests | [`docs/CPU-CORE.md`](docs/CPU-CORE.md), [`core/`](core) — 72 instructions verified against silicon |
| **JIT on iOS 26 TXM hardware** — bless protocol, `jit_arena` | **working on device** (M3 iPad): [`docs/JIT-DESIGN.md` §1a](docs/JIT-DESIGN.md) |
| Process model | **decided**: single process, emulated Linux process model (no extension) — `ARCHITECTURE.md` top note |
| **GPU binding probe** — d12mt's heap-is-an-argument-buffer model for D3D9, D3D11 and D3D12, tested on device | **27/27 PASS on the M3 iPad** — [`docs/MEMPROBE.md`](docs/MEMPROBE.md) |
| **d12mt** — Direct3D → Metal compiler: D3D12 root signatures → argument buffers; DXIL (D3D12), SM5 (D3D11) and SM3 (D3D9) → MSL | **working**, own public repo [`LibertyPrime27/d12mt`](https://github.com/LibertyPrime27/d12mt), vendored at [`gpu/d12mt`](gpu/d12mt); CI compiles its MSL for iOS with Apple's Metal compiler |

## The two things to know before reading anything else

1. **32-bit and 64-bit are different engines on iOS, and 64-bit is the architecturally cleaner one.** Darwin reserves the low 4 GB of address space and won't give it back; a 32-bit guest needs it, a 64-bit guest doesn't. Details in `ARCHITECTURE.md` §0.
2. **Everything 64-bit depends on one unmeasured number:** whether an iOS app extension can hold ~3 GB resident. Apple's docs say no; LiveContainer's author says yes. `tools/memprobe` is the app that settles it — build it, sign it, run it, read `docs/MEMPROBE.md` for how to interpret the number.

## Build

Pushes to `main` build an unsigned `.ipa` on GitHub's `macos-26` runners and attach it as a workflow artifact. Sign it locally with Sideloadly, AltServer or SideStore. The repo is public because macOS runner minutes are free only for public repos.

The emulator core is kept platform-independent so most of it builds and tests on a Linux runner in minutes rather than a macOS runner in tens of minutes.

## Measured on hardware (Sept 2026)

| | M3 iPad | iPhone Air |
|---|---|---|
| CPU core vs x86 silicon | 336/336 match | 336/336 match |
| Usable memory, app process | 8128 MB | 6080 MB |
| JIT (TXM bless protocol) | **working** | — |
| GPU: D3D9 / D3D11 / D3D12 binding model on Metal | **27/27 PASS** (iPadOS 26.3.1) | not yet run |
| App extension launch | failed (x2) — no longer required | — |

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
