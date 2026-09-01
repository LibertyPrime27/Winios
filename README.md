# winios

Run 32-bit and 64-bit Windows games on iOS. Sideloaded, JIT-enabled, maximum performance.

**Targets:** Fallout 3, Fallout: New Vegas, Fallout 4.

## Status

Planning and design. No shippable code yet.

| Piece | State |
|---|---|
| Architecture (why two engines, what blocks 64-bit) | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| JIT acquisition and probing design | [`docs/JIT-DESIGN.md`](docs/JIT-DESIGN.md) |
| CI — unsigned IPA on every push, core tests on Linux | [`.github/workflows/ios-build.yml`](.github/workflows/ios-build.yml) |
| **MemProbe** — the measurement that gates 64-bit | [`docs/MEMPROBE.md`](docs/MEMPROBE.md), [`tools/memprobe/`](tools/memprobe) |
| Engine A — 32-bit (Boxedwine core, ARM64 JIT) | not started |
| Engine B — 64-bit (Wine + dynarec, wineserver as extension) | **blocked** on MemProbe's result |

## The two things to know before reading anything else

1. **32-bit and 64-bit are different engines on iOS, and 64-bit is the architecturally cleaner one.** Darwin reserves the low 4 GB of address space and won't give it back; a 32-bit guest needs it, a 64-bit guest doesn't. Details in `ARCHITECTURE.md` §0.
2. **Everything 64-bit depends on one unmeasured number:** whether an iOS app extension can hold ~3 GB resident. Apple's docs say no; LiveContainer's author says yes. `tools/memprobe` is the app that settles it — build it, sign it, run it, read `docs/MEMPROBE.md` for how to interpret the number.

## Build

Pushes to `main` build an unsigned `.ipa` on GitHub's `macos-26` runners and attach it as a workflow artifact. Sign it locally with Sideloadly, AltServer or SideStore. The repo is public because macOS runner minutes are free only for public repos.

The emulator core is kept platform-independent so most of it builds and tests on a Linux runner in minutes rather than a macOS runner in tens of minutes.

## Licensing

The 32-bit engine is built on [Boxedwine](https://github.com/danoon2/Boxedwine) (GPL-2.0). This project is therefore GPL-2.0 and ships source. See `ARCHITECTURE.md` §6.
