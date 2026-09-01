# Architecture — running 32-bit and 64-bit Windows games on iOS

**Target:** Fallout 3, Fallout: New Vegas (32-bit), Fallout 4 (64-bit), maximum performance, sideloaded with JIT.

---

## 0. The finding that shapes everything

**32-bit and 64-bit are not one engine on iOS. They are two, and the 64-bit one is architecturally cleaner.**

That inverts the usual expectation, and the cause is `__PAGEZERO`. Darwin permanently reserves the low 4 GB of address space so stray 32-bit pointers fault instead of silently corrupting memory, and it cannot be shrunk on arm64 — a custom `pagezero_size` is incompatible with the mandatory ASLR. `extended-virtual-addressing` extends the space *above* the reservation, never into it.

- A **32-bit x86 guest** needs its pointers to fit in 32 bits, so its arena has to live in the region iOS won't give you. Box64's `box32` mode marshals guest pointers by identity and calls `abort()` the moment one exceeds 32 bits.
- A **64-bit x86-64 guest** has no such requirement. Its arena sits anywhere above the reservation, which is exactly what the entitlement provides.

So Fallout 4 clears the wall that Fallout 3 and New Vegas hit, despite being the heavier game by every other measure.

---

## 1. Two engines

| | Engine A — 32-bit | Engine B — 64-bit |
|---|---|---|
| **Targets** | Fallout 3, New Vegas, the pre-2010 catalogue | Fallout 4 and anything modern |
| **Base** | Boxedwine (GPL-2.0) | Wine + a Box64-class dynarec |
| **Guest memory** | Software MMU with its own 4 KB pages | Native mapping above `__PAGEZERO` |
| **Address-space wall** | Sidestepped — soft MMU means guest pointers are never host pointers | Doesn't apply |
| **16 KB host pages** | Irrelevant — soft MMU | Needs handling; Box64 has had 16 K support since 0.2.8 |
| **Process model** | Single process; `fork`/`clone` emulated internally | **Needs a real second process for `wineserver`** |
| **ISA ceiling** | x87/MMX/SSE/SSE2 only — no SSE3/SSSE3/SSE4 | Whatever the dynarec implements |
| **Status** | Buildable today; nobody has built it for iOS | **Blocked on §2** |

### Engine A caveats, stated plainly

Boxedwine's own TODO says *"games after the year 2010 have limited success at running."* Its flagship 3D benchmark is Quake 2 (1997) — 88.9 fps on an M4 Mac mini with the ARM64 JIT. There is no published report of anyone running a post-2010 3D game on it.

Fallout 3 (2008) and New Vegas (2010) are SSE2-era D3D9, so they are *architecturally* in scope. They are also roughly a decade past anything demonstrated. Treat Engine A as "plausible, unproven," not "solved."

Use the **native** build with the ARM64 JIT, not the WebAssembly build. The WASM route (Boxedwine in a `WKWebView`, which is what the shipping App Store app appears to do) is attractive because WebKit's JIT is the only one Apple sanctions — but it adds a translation layer on top of an emulator and is nowhere near Fallout-class performance. Keep it as a possible App-Store-legal tier for the 1990s catalogue; it is not the Fallout answer.

---

## 2. The blocker: one process

Engine B needs Wine. Wine needs `wineserver` — a separate process providing what the Windows kernel provides, reached over a Unix socket plus shared memory. iOS forbids `fork`/`exec` and spawning child executables; Apple DTS is explicit, and adds that fork-without-exec is unsafe on every Apple platform.

There is no upstream in-process wineserver and nobody is building one. Collapsing it into threads means forking Wine's object manager and synchronization model.

### The untried alternative: wineserver as an app extension

iOS *does* have multiple processes. It just won't let you `fork` into them.

**App extensions are separate processes**, launched by the system from your own bundle. LiveContainer already exploits this — its multitasking mode runs guest apps in an extension called LiveProcess. So rather than making wineserver in-process, make it a **sibling process** and replace its Unix-socket transport with XPC plus Mach shared memory.

That rewrites Wine's *transport*, not its *object model* — far smaller than the alternative.

**Constraint:** extensions are pre-declared, not spawned on demand. Instead of arbitrary `fork` you get a fixed pool — declare four or five slots and hand them out by role. Most games need two or three (launcher, game, occasional helper), so this bites on installers more than on gameplay.

**Risk, and it is the whole project:** Apple documents that extension points define their own memory limits, which *override* `increased-memory-limit` — the documentation's example is 100 MB. LiveContainer's author claims parity with regular apps. These cannot both be true. Fallout 4 needs ~2.4 GB resident in the game process.

> **This single unverified question gates Engine B, and therefore Fallout 4.** Measure it first. See `JIT-DESIGN.md` §7 — the JIT helper extension is a free vehicle for the measurement.

---

## 3. Shared infrastructure

Both engines share more than they differ:

- **`ProcessPool`** — pre-declared extension slots with typed roles (`jit-helper`, `wineserver`, `game`). Engine A uses one slot; Engine B uses several. Build it once.
- **JIT acquisition and probing** — see `JIT-DESIGN.md`. Identical for both.
- **Metal display backend** — a software-surface blitter for 2D, a D3D→Metal path for 3D.
- **Audio, input, game import, per-title profile database.**
- **`ICpuBackend`** — dynarec/interpreter selection, per §5 of `JIT-DESIGN.md`.

The profile database is not a nice-to-have. Winlator's history and Winulator's before it both show the per-game configuration database *is* half the product.

---

## 4. Graphics

The GPU is not the constraint. An A19 Pro scores 6,557 in 3DMark Wild Life Extreme against the Snapdragon 8 Elite's 7,156 — and that class already runs Fallout 4 at 30–40 fps under Winlator on hardware two generations older. On Apple silicon, CrossOver on a base M2 runs Fallout 4 at 60 fps, 1440×900, Medium.

The constraint is the translation layer.

- **D3D9 (FO3/NV):** `d9mt` — the Direct3D 9 → Metal fork of DXMT. Early and low-activity, but the right shape.
- **D3D11 (FO4):** **DXMT** — LGPL-2.1, written for CodeWeavers, shipping as a first-class backend in CrossOver Mac 26 including an ARM64 build. Its `airconv` translates DXBC to Apple IR via LLVM 15; Apple's proprietary converter is not involved. macOS-only today; Metal is Metal, so the GPU side should port and the work is the Wine-unixlib boundary plus carrying LLVM.
- **DXVK on MoltenVK is not viable.** DXVK 3.0 requires `VK_EXT_transform_feedback`; Metal has no stream-output primitive and MoltenVK has never implemented it. Revisit only if KosmicKrisp ships on iOS with geometry-shader support.
- **Apple's D3DMetal is off the table** — proprietary, and the Game Porting Toolkit license restricts it to developing, testing or evaluating games.

Expect the *graphics path* to be where FO3/NV fail, not the frame rate. Gamebryo's DirectDraw-adjacent UI is exactly where translation layers break on Android today — "failed to initialize rendering" and invisible menu text are the documented failure modes there.

---

## 5. Order of work

1. **Measure the extension memory limit.** One day. Gates Engine B and therefore Fallout 4.
2. **Rehearse the JIT handshake on a toy app.** Fifty lines. Gates everything.
3. **Engine A on device, headless** — Boxedwine core building for `iphoneos` arm64, ARM64 JIT armed, Wine printing its version to `idevicesyslog`.
4. **Metal blitter, audio, input.** First playable target: something 2D and well-behaved, held for twenty unbroken minutes with thermals and `os_proc_available_memory()` logged.
5. **Fallout 3 / New Vegas on Engine A.** Expect graphics-path work, not CPU work.
6. **Engine B**, only if step 1 passed.

---

## 6. Licensing

Boxedwine is **GPL-2.0**; Wine is **LGPL**; DXMT is **LGPL-2.1**. A GPL-2.0 core means this project ships source and cannot go to the App Store — which is consistent with the sideload-plus-LiveContainer target, but it is a decision to make deliberately rather than discover later.

Note in passing: the App Store app that appears to ship Boxedwine as WebAssembly is closed-source. If it does bundle Boxedwine, that is a GPL-2.0 violation, and the corresponding source is requestable under the licence. That request — not reverse engineering — is the only legitimate route to "building off" it. Everything useful about it is upstream in Boxedwine and in `andrewnakas/exebrowser` (MIT frontend) anyway.
