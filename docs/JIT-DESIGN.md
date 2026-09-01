# JIT Acquisition and Capability Detection — Design

**Status:** design only. No implementation in this document by intent.
**Audience:** whoever writes `core/jit/` and the helper extension.

---

## 0. The one rule

**Never trust a flag. Probe the actual capability.**

Every naive iOS JIT implementation checks `CS_DEBUGGED` and proceeds. That was correct until iOS 18.4, and it has been wrong ever since. A process can carry `CS_DEBUGGED` and still get `EPERM` from `mprotect(..., PROT_EXEC)` — this is the Flutter/Dart `mprotect failed: 13` failure mode, and it is why several emulators broke silently on iOS 26 rather than failing loudly.

The probe must execute a real instruction through a real executable mapping. Anything less reports success on devices where the JIT will crash the moment the recompiler emits its first block.

---

## 1. Layered design

```
┌─ Layer 5  Loss recovery — JIT can vanish mid-session
├─ Layer 4  Backend selection — dynarec vs interpreter, at runtime
├─ Layer 3  Diagnostics — six distinct failure causes, six distinct remedies
├─ Layer 2  Acquisition — ask an external enabler to attach
└─ Layer 1  Capability probe — does W^X actually bend on this device, right now
```

Layers 1 and 3 are where the value is. Layer 2 is thin. Most projects build only Layer 2 and wonder why users file unreproducible bugs.

---

## 2. Layer 1 — Capability probe

### 2.1 Signals to gather (cheap, in order)

| Signal | How | Meaning |
|---|---|---|
| `CS_DEBUGGED` | `csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags)`, test `0x10000000` | Necessary, **not** sufficient |
| Hardware class | Detect TXM: A15+ / M2+ | Selects which enabler path can work at all |
| OS version | `NSProcessInfo.operatingSystemVersion` | Gates known-broken configurations |
| Enablers present | `canOpenURL:` on each scheme | Which acquisition routes exist |

### 2.2 The executability smoke test — the actual test

`MAP_JIT` is unavailable (it needs `dynamic-codesigning`, which is Apple's alone), and `mprotect` to add `PROT_EXEC` is unreliable. The working primitive is **dual mapping**:

1. `mmap` an anonymous page, no `PROT_EXEC`.
2. `mach_vm_remap()` the *same physical pages* to a second virtual address with `copy = false` — one physical page, two virtual aliases.
3. `vm_protect()` alias A as `RW`, alias B as `RX`.
4. Write a single AArch64 `ret` (`0xD65F03C0`) through alias A.
5. **`sys_icache_invalidate()`** on alias B. Mandatory on arm64 — skipping this produces intermittent, unreproducible failures that look like a JIT bug for weeks.
6. Call through alias B as a function pointer.

Returns cleanly → JIT is genuinely available. This is the same split-W^X design QEMU adopted for Darwin (`tcg_splitwx_diff`), so it is well-trodden.

### 2.3 Run the probe where a crash is survivable

If step 6 faults, the default outcome is a dead app on launch — the worst possible failure for a capability *check*.

**Run the probe inside the helper extension, not the host app.** The extension is a separate process; if it dies, the host learns "probe failed" from a broken XPC connection instead of terminating. This costs one XPC round trip at startup and removes an entire class of launch crashes.

If the probe must run in-process (early bring-up, before the extension exists), wrap it in a Mach exception handler installed for the calling thread only, and tear it down immediately after.

### 2.4 Caching

Cache **per launch**, never across launches. The Developer Disk Image unmounts on reboot, the VPN drops, the debugger detaches. A cached "JIT available" from yesterday is a crash today. Re-probe on every cold start and on every foreground transition (§6).

---

## 3. Layer 2 — Acquisition

### 3.1 Why the app cannot do this itself

A process attaching a debugger to itself deadlocks. The attach must come from another process. Two shapes:

- **External enabler** (StikDebug etc.) — the app opens a URL scheme; the enabler attaches by PID.
- **Own helper extension** — bundle `StikJIT.xcframework` in an app extension, hand it the host PID over XPC, let it attach `debugserver` over the device's RSD tunnel and detach.

Ship the external path first (less to maintain, users already have StikDebug). Add the in-bundle helper when you want a one-tap experience.

### 3.2 Enabler priority

| Order | Scheme | Notes |
|---|---|---|
| 1 | `stikjit://attach?pid=<pid>` | **PID attach** — the only form that works under LiveContainer multitask |
| 2 | `stosdebug://` | StosDebug, newer; LiveContainer 3.8.0+ |
| 3 | `sidestore://enable-jit?bundle-id=<id>` | Bundle-ID form; fails in multitask |
| 4 | SideJITServer / JitStreamer | Network attach; the documented iOS 17.0–17.3 fallback |

Probe with `canOpenURL:`, pick the highest available, open it, then **poll the Layer-1 probe with backoff** — attach takes 2–15 s and there is no completion callback. Suggested schedule: 500 ms × 4, then 1 s × 6, then 2 s × 5. Hard stop at ~30 s and hand off to Layer 3.

### 3.3 The iOS 26 opt-in obligation

Since iOS 26, a debugger can no longer enable JIT for *any* `get-task-allow` app. The app must implement the new method, and StikDebug must ship a **script for this bundle** (its `Scripts/` folder — `universal.js`, `legacy.js`, and per-app variants).

Consequences, both non-negotiable:

- Implementing the memory-setup side is a **day-one** requirement, not a later polish task.
- **Getting a script accepted upstream for this bundle ID is a shipping requirement.** Without one, users on iOS 26+ have no way to arm JIT no matter what the app does. Budget for that as a real work item with an external dependency.

---

## 4. Layer 3 — Diagnostics

This is what separates a usable app from a support nightmare. "JIT unavailable" is not a message; it is an abdication. Six causes, each with its own remedy:

| # | Cause | Detection | Message to user |
|---|---|---|---|
| 1 | No enabler installed | No scheme responds to `canOpenURL:` | "Install StikDebug (free, App Store)" + link |
| 2 | No pairing file | Enabler opens then returns immediately | "StikDebug needs a pairing file — set it up once from a computer" |
| 3 | DDI not mounted | Attach times out; first boot since restart | "Restarted since last use — open StikDebug once on Wi-Fi to remount" |
| 4 | LocalDevVPN down | Attach times out with VPN interface absent | "Turn on StosVPN / LocalDevVPN, then retry" |
| 5 | Attached but pages not executable | `CS_DEBUGGED` set **and** smoke test fails | "This iOS version needs a StikDebug script for this app" + issue link |
| 6 | Known-broken OS | Version in deny-list (e.g. offline JIT on 26.4+) | "Offline JIT was removed in iOS 26.4 — connect to Wi-Fi" |

Cause 5 is the one that matters most and the one nobody detects, because it requires having *both* Layer-1 signals rather than just the flag. Detecting it correctly is the single highest-value piece of this whole design.

Log every probe result with OS version, device class, TXM status, enabler used and elapsed attach time. When iOS breaks JIT again — it has twice in twelve months — this log is how you find out within a day instead of a release cycle.

---

## 5. Layer 4 — Backend selection

```
ICpuBackend
├── DynarecBackend      requires JIT
└── InterpreterBackend  always available
```

Rules:

1. Select at core init from the Layer-1 result. Never from the flag alone.
2. Allow a **per-title override** in the profile database — some games will be more stable interpreted.
3. **Surface the active backend in the UI and in logs, prominently.** A silent fallback to the interpreter invalidates every benchmark taken afterwards, and it is the most common source of "why is it suddenly slow" reports that turn out to be misconfiguration.
4. Never make the interpreter dead code. It is the thing that keeps the app working the next time Apple moves.

---

## 6. Layer 5 — Losing JIT mid-session

JIT is not durable. The debugger detaches, the VPN drops, the OS reclaims. The app must survive it.

- Re-probe on `UIApplicationWillEnterForeground`.
- On loss, do **not** crash and do not continue emitting. Options, in preference order:
  1. Pause at the next translation-block boundary, tell the user, offer re-attach.
  2. Abandon the code cache and continue on the interpreter.
- This requires the code cache to be **abandonable at a block boundary** — a constraint on the recompiler's design, so decide it now rather than retrofitting.

---

## 7. The extension is a shared primitive — design it once

The helper extension that attaches the debugger and **the app extension that would host `wineserver`** are the same thing: a pre-declared, system-launched sibling process reached over XPC.

Build one `ProcessPool` abstraction over pre-declared extension slots, with typed roles (`jit-helper`, `wineserver`, `game`), rather than a bespoke JIT helper now and a second unrelated mechanism later. Two consequences:

- **The extension memory-limit question is shared.** Apple documents that extension points define their own memory limits which *override* `increased-memory-limit`. This gates the wineserver idea entirely — and the JIT helper needs almost no memory, so the JIT helper is a **free, low-risk vehicle for measuring it**. Instrument `os_proc_available_memory()` in the helper from day one.
- Packaging must preserve `Payload/*.app/PlugIns`. Naive zip steps drop it silently and the failure looks like "enabler not found."

---

## 8. Open questions

1. Do app extensions really get app-level memory limits under LiveContainer, or Apple's documented lower ones? **Gates the wineserver architecture.** Measure with the JIT helper.
2. Does the in-process Mach exception handler in §2.3 behave on current iOS, or does the probe need to be extension-only from the start?
3. What exactly does the iOS 26 opt-in method require of the app, beyond cooperating with the debugger-side script? Read StikDebug's scripts against a known-working app (UTM, DolphiniOS) rather than guessing.
4. Can a StikDebug script for this bundle be upstreamed, and on what timeline? External dependency, so start early.
