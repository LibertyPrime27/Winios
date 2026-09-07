# MemProbe — the app, and the probes underneath it
> The app installs as **winios** (bundle id `winios.app`). This file keeps
> its old name because the probes it documents came first and the paths
> under `tools/memprobe/` have not moved; the product is winios.

## The app

The front door is a **library** of Windows programs on the device. `+` adds one
— a folder for anything real, since a program is rarely a single file — and
copies it into the app's own storage, which is the guest's `C:` drive. A
security-scoped URL from the Files app is not something a PE loader can hold
on to, so it has to be copied rather than referenced.

Every row says what is known before it is tapped: bitness, and how many of the
program's imports nothing here can satisfy. For anything real that second
number is large, and saying so up front is more use than letting someone tap
Run and watch it stop.

A program has two ways to run, and the difference is the point:

- **Run** carries on past functions we have not implemented — each logs itself
  and returns zero — so one run names *everything* the program wanted. That is
  how to find out what to build next. It is not a way to run something for
  real: zero is failure for most of the Win32 API but success for some, so a
  program in this mode can wander somewhere real Windows would never have let
  it go.
- **Run strictly** stops at the first missing function, which is the honest
  answer to "does this work yet".

Either way the output ends with the run report, and it is kept — whoever reads
it is usually not whoever tapped the button. **Full screen** is for a program
that draws: its frames as they arrive, with a Stop that ends it by making
`Present` fail rather than reaching into running guest code.

The probes below were the whole app for as long as there was nothing to run.
They are one tap away under **Diagnostics**, and they are still how you tell
"this program is broken" from "this device is broken".

## Measuring the app-extension memory limit

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

**▶ Run everything in order** is the one button: bless the arena, CPU vectors,
benchmark, GPU, the Windows guests, the Direct3D 9 frames, the Direct3D 11
frames, then the memory ladder — and finally arm the next arena size if the ceiling is still unknown.

The order is not cosmetic. The arena is blessed **first**, explicitly, so every
later step runs with executable memory rather than blessing it as a side effect
halfway through — and so the report says which size was blessed before anything
depends on it. The memory ladder goes **last** because it is the one step that
can end the process, and because holding several gigabytes would skew anything
measured after it. Arming the next arena rung comes after all of that, so a
launch that got through the whole sequence is what earns the bigger try:
walking the ladder is "tap, relaunch, tap".

Each probe also has its own button, because once something has failed you want
to re-run that one thing, and because you rarely want to risk the ladder just
to see the GPU result again.

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
- **7 · D3D9 frame** — both Direct3D 9 guests, with the frame shown in the app.
  `d3dframe` paints its own pixels through a locked back buffer, the way a
  software intro or a video player does. `d3ddraw` fills a vertex buffer and
  calls `DrawPrimitive`, so its pixels come out of the reference rasterizer —
  and because that rasterizer is integer by construction, its checksum has to
  match the one recorded on an x86 runner and under qemu. Checking it here is
  what proves that on real hardware. **Clear frame** hides the image again.
- **10 · D3D11 frame** — `d11test`, in both bitnesses, at a fixed 320x200. It
  builds DXBC containers by hand, creates a device, a swap chain, a render
  target, a texture and its view, vertex, index and constant buffers, an input
  layout and the state objects, then draws an indexed textured quad and a
  Gouraud strip — every call of it through a COM vtable in guest memory, which
  is also what checks that the twelve vtables have the slot counts D3D11 says
  they have. The report gives three answers separately: it ran, its own checks
  passed, and the frame checksum is `311139ad`. Fixed at 320x200 whatever the
  display setting says, because a recorded checksum belongs to a resolution;
  the screen size the app had set is put back afterwards.

  The number is the point. The rasterizer behind it has no floating point in
  it, so `311139ad` on the phone means Apple silicon computed the same pixels
  as an x86 runner and a qemu aarch64 run — and a different number means
  something is wrong, not that something rounded. What the picture does *not*
  prove is that a game will look right: the shaders are not executed (the
  pipeline is interpreted from their signatures), there is no depth buffer,
  and interpolation is affine. See `docs/WIN32.md`.

  The device, the back buffer and `Present` are real; the GPU is not in this
  path yet. Metal only uploads and scales what the guest and the rasterizer
  computed.
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
- **9 · JIT arena** — how much executable memory the debugger will bless, which
  is the ceiling on how much guest code can ever be resident: the bless happens
  once per launch and the region cannot grow afterwards. 1 MB was an arbitrary
  first choice and is already too small — one pass through the bundled guests
  emitted 1044 KB, and only fit because each run flushes the block cache, which
  a game never does.

  So it climbs one size per launch (1, 4, 16, 64, 256 MB), the same shape as
  the memory ladder: the size to try next is remembered, a size that blessed is
  remembered as good, and a size whose attempt never came back is remembered as
  bad, after which the next launch drops to the last good one. Backing off to a
  size that has already worked is what stops it being a crash loop. The size in
  flight is forced to disk before the breakpoint, because the next thing that
  happens may be a fatal SIGTRAP and otherwise the finding is lost. **Reset
  results** clears the in-flight attempt but keeps what the ladder has learnt —
  those are facts about the device, not results.
- **6 · Memory ladder** — the ladder on its own.

  A note on what the numbers are measured on: every probe runs at
  `.userInteractive` QoS, not `.userInitiated`, because a global concurrent
  queue at a lower band is eligible for the efficiency cores. That is the right
  thing for an app whose job is measuring this chip — but it did **not** explain
  the thing it was meant to.

  **The open question.** CI's macOS runner interprets the integer loop at 73
  MIPS; an x86 cloud container does 48; both iOS devices do 9. Meanwhile the
  same devices run *dynarec* output at ~4100 against the runner's 2393. An M3
  that is faster than a desktop runner at executing JIT code and five times
  slower than a shared cloud VM at executing C is not a story about core speed.
  Raising the QoS changed nothing (9.4 → 9.6 on the iPad), so scheduling is not
  it either.

  Until that is explained, **the "×" ratios in section 2 are not comparable
  across machines** — the absolute dynarec MIPS are. The `native C reference`
  line now printed above them is the instrument for settling it: it times the
  same arithmetic in plain C, not emulated, on whatever machine is running. If
  the device's native figure is also ~5× below the runner's, everything the app
  executes is slow there and the interpreter is fine; if it is not, the problem
  is specific to the interpreter's code and worth hunting.
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
