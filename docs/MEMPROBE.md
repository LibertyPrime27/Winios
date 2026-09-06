# MemProbe — measuring the app-extension memory limit

**Why this exists.** Whether 64-bit Windows games (Fallout 4) are reachable on
iOS comes down to one unmeasured number: how much resident memory an iOS app
extension can hold.

The chain is short. Wine needs `wineserver` in a second process. iOS forbids
`fork`/`exec`, but app extensions *are* separate processes launched by the
system from your own bundle — so wineserver could be an extension talking over
XPC instead of a Unix socket (`ARCHITECTURE.md` §2). That works only if an
extension can hold the ~2.4 GB Fallout 4 needs.

Two authorities disagree:

- **Apple**: extension points define their own memory limits, and those limits
  *override* `com.apple.developer.kernel.increased-memory-limit`. The
  documentation's worked example is 100 MB.
- **LiveContainer's author**: multitask processes (LiveProcess, an app
  extension) have the same limit as regular apps.

Both cannot be true. This measures which.

## What it does

A ladder: allocate 64 MB, touch every page, record the rung durably, repeat.

Three details carry the whole design:

1. **Pages are touched, not just allocated.** Untouched pages are not resident.
   An allocation-only loop measures address space — which the entitlements
   really do extend to ~64 GiB — and would report a happy, useless number.
2. **Each rung is written and `fsync`'d before the next is attempted.** Jetsam
   kills without a callback. There is no "and then it returned" — the answer is
   the last rung that survived, read from a file after the process is gone.
3. **The same ladder runs in the app process for comparison.** The extension
   number alone means nothing; the ratio is the finding.

## Running it

```
xcodegen generate --spec tools/memprobe/project.yml --project tools/memprobe
```

Or take `MemProbe-unsigned.ipa` from the CI artifacts. Sign and install with
Sideloadly, then:

1. **Run ladder in app process** — baseline.
2. **Run ladder in extension** — launched directly via `NSExtension`. The
   status line reports the pid, then "KILLED" when the system terminates it;
   that is the measurement, not a crash.
3. Reopen MemProbe. The high-water figures are on screen.

Watch `idevicesyslog` for the `winios.memprobe` subsystem to see rungs live.

## Reading the result

| Extension high-water | Meaning |
|---|---|
| ≥ 3 GB | Wineserver-as-extension is viable. Fallout 4 stays on the table. |
| ~1–3 GB | Marginal. Possible at low settings; needs a real memory budget first. |
| ≤ few hundred MB | Apple's documented behaviour holds. Engine B is dead by this route, and with it 64-bit games on iOS by any known architecture. |

"Stopped voluntarily" in the log means the ceiling was reached rather than a
limit — raise `ceilingMB` in `Ladder.swift` and rerun.

Check the jetsam report afterwards (Settings → Privacy & Security → Analytics &
Improvements → Analytics Data, `JetsamEvent-*`). The `reason` distinguishes
`per-process-limit` (a real cap) from `vm-compressor-space-shortage` (device
pressure, so retest on a quiet device).

## Why the extension is an AR Quick Look extension

The first version was a share extension launched from the share sheet. Two
things were wrong with that, and the second one is the interesting one.

A regular app is not allowed to *host* a share extension through `NSExtension`
— only the system share sheet is. So `extensionWithIdentifier:` returned nil
with no error for an extension that was demonstrably in the bundle. LiveContainer
avoids this by declaring LiveProcess as **`com.apple.ar.viewer`** with
`NSExtensionActivationRule: FALSEPREDICATE`: any app may host AR Quick Look
content, and the predicate keeps it out of every UI. This probe now does the
same and is launched directly, one button, with a callback when the system
terminates it — which for the ladder *is* the result.

More importantly, LiveProcess's `Info.plist` carries an `XPCService` block with
`_ProcessType: App`. That is the most plausible mechanism behind the
LiveContainer author's claim that LiveProcess gets app-level memory limits in
spite of Apple's documented extension caps — and it is the very claim this
tool exists to test. The probe copies that block exactly, so a good number here
is a number for *this* configuration, which is the one the real product would
ship with.

## Before drawing conclusions

**The extension point is a variable.** Apple says each point sets its own limit,
so one number does not generalise. This now probes exactly the configuration
LiveProcess uses (`com.apple.ar.viewer`, App-type XPC). If the number is good
here, it is good for the configuration we would ship.

**Then repeat inside LiveContainer**, in multitask mode. That is the
configuration the real product would ship in, and entitlements there apply to
LiveContainer's bundle rather than the guest's — so a good number here does not
guarantee a good number there.

## The GPU binding probe (section 2 of the report)

This is the on-device test for d12mt (`gpu/d12mt`), the Direct3D → Metal
compiler. Its whole binding model rests on one claim that can only be settled
on real silicon: a Metal 3 argument buffer is a flat array of 8-byte slots,
`[[id(k)]]` at byte `8k`, so a D3D12 descriptor heap — or a D3D9/11 stage's
register file — can be an ordinary `MTLBuffer` the CPU writes directly, and a
descriptor table is that buffer bound at an offset.

The same nine-column probe is written three times and compiled by d12mt from
the real bytecode each API uses (`gpu/d12mt/tests/shaders/`; the MSL is
bundled as `Host/Shaders/*.msl` and compiled on the device):

- **D3D12** — `probe.hlsl`, DXIL. Root constants, a root CBV (device pointer),
  an SRV+CBV descriptor table, a sampler table and a static sampler.
- **D3D11** — `probe11.hlsl`, Shader Model 5 DXBC (what Fallout 4 ships).
  Fixed register slots through d12mt's legacy plan: `b#` at `[[id(#)]]`,
  `t#` at `[[id(16+#)]]`, samplers in a second buffer.
- **D3D9** — `probe9.hlsl`, Shader Model 3 (what Fallout 3 and New Vegas
  ship). `c#` constant registers become one constant buffer, sampler stages
  become texture+sampler pairs, and the fixed-function state blocks
  (alpha test, clip planes, per-sampler state) are supplied as constant
  buffers the way dxbc-spirv expects.

For each API the probe draws one quad with every binding fed from descriptors
written by hand — textures and constant buffers in one heap at a non-zero
base, samplers in a sampler heap, vertex colour through the input layout — and
reads the nine pixels back. Each column is one binding path with an exact
expected value, identical across the three APIs.

Reading it: `27/27 PASS` means the binding design holds for all three
generations on this GPU and d12mt's runtime can be built on it. A `FAIL` line
names the API and the path that is wrong, with expected against actual; that
is a design finding, not a crash. `argument buffers: tier 1` or a GPU family
below `apple6` means the device is too old for this design (A12 and earlier).
Run it on both the iPad and the iPhone — the result must hold on A-series and
M-series alike.

## The ladder no longer climbs into the kill

The original design let jetsam kill the process and read the answer from the
last surviving rung. That was right while the limit was unknown; it is measured
now (8128 MB on the M3 iPad, 6080 MB on the iPhone Air), and inside
LiveContainer a kill takes LiveContainer down too. `os_proc_available_memory()`
reports the remaining jetsam budget exactly, so the ladder now stops when fewer
than 256 MB remain and reports `held + remaining` as the limit. Pass
`headroomMB: 0` to `Ladder.climb` to get the old behaviour.

## The other way the ladder used to die: the CPU watchdog

A `cpu_resource_fatal` report from the M3 iPad (build 432a8db) showed the real
cause of the intermittent crashes in the LiveContainer flow: not jetsam but
`48 seconds cpu time over 51 seconds, exceeding limit of 80% cpu over 60
seconds` with the app `Non-Frontmost`. iOS kills any background app that
behaves like that, and touching pages is CPU work (zero-fill is charged to the
process). Switching to StikDebug while the ladder ran was enough. The ladder
now pauses whenever the app is not frontmost, and sleeps at least as long as
each rung's work took, so its duty cycle stays under 50% even in the foreground.

## The buttons

**▶ Run all probes** runs everything in report order — CPU vectors, benchmark,
GPU, Windows guests — and finally the memory ladder, last because the ladder
may end with the system killing the process and by then everything else has
been saved. Each probe also has its own button, because once something has
failed you want to re-run that one thing, and because you rarely want to risk
the ladder just to see the GPU result again.

- **1 · CPU vectors** — the recorded silicon post-states, replayed through the
  interpreter and then through the dynarec. Since half the x87 vectors are now
  recorded in 53-bit precision, the dynarec pass reports how many x87
  instructions it lowered onto NEON versus left to the interpreter.
- **2 · Benchmark** — `xc_bench`: integer, SSE2 and x87 loops through both
  engines, in milliseconds and millions of guest instructions per second. Every
  performance figure in the other docs comes from qemu-user, where the absolute
  numbers mean nothing; this is the only place real ones come from.
- **3 · GPU** — the D3D9/11/12 binding probe (section above).
- **4 · JIT: attach StikDebug** opens StikDebug with the universal script; when
  you return to the app with the debugger attached, the blessed-arena execution
  test runs on its own. If a debugger is already attached when the app starts —
  LiveContainer paired with StikDebug does this before the first instruction —
  the round trip is skipped and it executes immediately. The only thing that
  stops the automatic execute is a crash marker from an earlier attempt, which
  **Reset results** clears.
- **5 · Windows .exe** — runs the twelve mingw-w64 guests bundled with the app
  (`hello`, `crt`, `nbody`, the DLL-chain loader test `dlltest`, and the two
  Direct3D 9 programs `d3dtest` and `d3dframe`, each as PE32 and PE32+) through
  `winrun_lib` and compares stdout and exit code against the recorded
  expectations. This is the PE loader, DLL loading, the host-implemented
  kernel32/msvcrt/d3d9, and the dynarec, end to end on the device.
- **x87 fast path** — the same machinery on `nbody32.exe` alone, the guest whose
  float work is entirely x87, with its timing and its lowered-versus-called-out
  counts. The quickest way to see the 53-bit lowering working on hardware.
- **7 · D3D9 frame** — runs `d3dframe32.exe`, which creates a Direct3D 9
  device, locks its back buffer, draws into it and presents — and shows the
  frame that arrived, right there in the app. The device, the back buffer and
  `Present` are real; the pixels come from the guest's own x86 code on the
  dynarec, because `DrawPrimitive` is not implemented yet. It is the first
  thing this project has put on a screen. **Clear frame** hides it again.
- **8 · Run a Windows program full screen** — the app rather than the probe.
  `d3dloop32.exe` runs its own frame loop (clear, draw, present, repeat) on a
  background queue while a `CAMetalLayer` shows each frame as it arrives, with
  the guest's frame rate on screen. **Close** asks it to stop by making
  `Present` return `D3DERR_DEVICELOST` — what a real driver returns when the
  display mode changes — so the guest leaves its own loop and exits; nothing
  reaches into running guest code.

  The guest never waits for the display and the display never waits for the
  guest: every presented frame is copied under a lock with a sequence number,
  and the display link takes whatever is newest once per vsync. Frames it
  misses are dropped rather than queued, which is what makes the number on
  screen the rate the *emulator* managed rather than the rate the screen
  refreshed at.
- **6 · Memory ladder** — the ladder on its own.
- **Copy report** puts the whole screen on the clipboard. **Reset results**
  clears everything, including the JIT crash marker.

Only one probe runs at a time, off the main thread, and each result is written
to `UserDefaults` the moment it exists.

### One bless, many buttons

Every button that needs executable memory goes through `ensureArena()`, which
blesses the shared arena at most once per launch. That is not tidiness: on iOS
26 / TXM hardware the debugger detaches at the end of the bless, and a second
`brk #0xf00d` is an unserviced breakpoint that kills the process (the 5c2e468
crash). The benchmark, the dynarec pass and the Windows guests all reuse the
one arena the JIT probe created.

### Running a guest twice

`winrun_main()` resets every global the runtime owns before it starts —
including unmapping the previous guest's memory and flushing the block cache.
Both matter: a second PE32+ image wants the same preferred base as the first,
and two PE32 images both map their code at `0x400000`, where a stale compiled
block would run the previous program's instructions. `tests/test_winrun_lib.c`
runs all six guests twice, in opposite orders, to keep that honest — it caught
the unmapping bug the first time it ran.
