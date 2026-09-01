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
2. **Run ladder in extension** → share sheet → **MemProbe**. Expect it to
   disappear; that is the measurement, not a crash.
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

## Before drawing conclusions

**The extension point is a variable.** Apple says each point sets its own limit,
so one number does not generalise. This probes a **share extension** — a
general-purpose, user-invocable point in the same broad class as LiveProcess.
Sweep others before committing: action extension, and whatever LiveContainer
actually uses.

**Then repeat inside LiveContainer**, in multitask mode. That is the
configuration the real product would ship in, and entitlements there apply to
LiveContainer's bundle rather than the guest's — so a good number here does not
guarantee a good number there.
