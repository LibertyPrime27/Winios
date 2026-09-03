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

## 4 · The GPU binding probe

Button 4 is not about memory. It is the on-device test for d12mt
(`gpu/d12mt`), the Direct3D 12 → Metal compiler. d12mt's whole binding model
rests on one claim that can only be settled on real silicon: a Metal 3
argument buffer is a flat array of 8-byte slots, `[[id(k)]]` at byte `8k`, so
a D3D12 descriptor heap can be an ordinary `MTLBuffer` the CPU writes directly
and a descriptor table is that buffer bound at an offset.

The probe draws one quad with the shaders d12mt produced from
`gpu/d12mt/tests/shaders/probe.hlsl` (bundled as `Host/Shaders/probe.*.msl`,
compiled on the device by the Metal compiler), feeding every binding path from
descriptors written by hand — four textures and a constant buffer in one heap
at a non-zero base, two samplers in a sampler heap, a root constant block and a
root descriptor in the root buffer, a vertex colour through the input layout —
and reads the nine pixels back. Each column is one binding path with an exact
expected value.

Reading it: `9/9 PASS` means the D3D12 binding design holds on this GPU and
d12mt's runtime can be built on it without a translation step for descriptors.
A `FAIL` line names the path that is wrong and shows expected against actual;
that is a design finding, not a crash. `argument buffers: tier 1` or a family
below `apple6` means the device is too old for this design (A12 and earlier).
Run it on both the iPad and the iPhone — the result must hold on A-series and
M-series alike.
