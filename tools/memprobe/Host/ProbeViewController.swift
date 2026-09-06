import UIKit
import os

/// The questions that gate everything else, on one screen.
///
///  1. Does the CPU core behave on ARM64 the way it does on x86? (golden vectors)
///  2. How fast is it, really? (interpreter vs dynarec, on this silicon)
///  3. Does the D3D binding design (heap == argument buffer) hold on this GPU,
///     for D3D9, D3D11 and D3D12 shaders alike?
///  4. Is JIT actually available, not just flagged?
///  5. Can it run a real Windows .exe?
///  6. How much memory will the app process hold? (the ladder)
///
/// "Run all" answers them in that order. Each also has its own button, because
/// once something has failed you want to re-run that one thing, and because the
/// memory ladder may end with the process being killed -- which is fine when it
/// runs last, and a nuisance when you only wanted the GPU result again.
///
/// Answering these on real hardware is the entire purpose of this build. It is
/// not a game runner and does not pretend to be one. Every result is persisted
/// as soon as it exists, so a memory-ladder kill costs nothing.
final class ProbeViewController: UIViewController {

    private let results = UITextView()
    private let store = UserDefaults.standard
    private var cpuLine: String { get { store.string(forKey: "cpu") ?? "not run" } set { store.set(newValue, forKey: "cpu") } }
    private var benchLine: String { get { store.string(forKey: "bench") ?? "not run" } set { store.set(newValue, forKey: "bench") } }
    private var gpuLine: String { get { store.string(forKey: "gpu") ?? "not run" } set { store.set(newValue, forKey: "gpu") } }
    private var jitLine: String { get { store.string(forKey: "jit") ?? "not run" } set { store.set(newValue, forKey: "jit") } }
    private var winLine: String { get { store.string(forKey: "win") ?? "not run" } set { store.set(newValue, forKey: "win") } }
    private var running = false
    private var jitAttachPending = false
    /// False while the app is not frontmost. Probe work pauses on it: iOS kills
    /// a background app that stays above 80% CPU for a minute (see the
    /// cpu_resource_fatal report in docs/MEMPROBE.md).
    private var isActive = true

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "winios probes · \(DeviceInfo.modelIdentifier)"
        view.backgroundColor = .systemBackground

        results.isEditable = false
        results.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        results.alwaysBounceVertical = true

        let stack = UIStackView(arrangedSubviews: [
            button("▶  Run all probes", #selector(runAll)),
            row([("1 · CPU vectors", #selector(runCPU)), ("2 · Benchmark", #selector(runBench))]),
            row([("3 · GPU (D3D9/11/12)", #selector(runGPU)), ("5 · Windows .exe", #selector(runWindows))]),
            row([("x87 fast path", #selector(runX87)), ("6 · Memory ladder", #selector(runLadder))]),
            button("4 · JIT: attach StikDebug, then execute in a blessed arena", #selector(attachJIT)),
            row([("Copy report", #selector(copyReport)), ("Reset results", #selector(resetAll))]),
            results,
        ])
        stack.axis = .vertical
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)

        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: g.topAnchor, constant: 12),
            stack.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -12),
            stack.bottomAnchor.constraint(equalTo: g.bottomAnchor, constant: -12),
        ])

        // Coming back from StikDebug with the debugger attached finishes the
        // JIT test without another tap.
        NotificationCenter.default.addObserver(self, selector: #selector(becameActive),
                                               name: UIApplication.didBecomeActiveNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(resignedActive),
                                               name: UIApplication.willResignActiveNotification, object: nil)
        refresh()
    }

    private func button(_ title: String, _ action: Selector) -> UIButton {
        var c = UIButton.Configuration.bordered()
        c.title = title
        c.titleAlignment = .leading
        c.titleLineBreakMode = .byWordWrapping
        c.contentInsets = .init(top: 8, leading: 12, bottom: 8, trailing: 12)
        let b = UIButton(configuration: c)
        b.contentHorizontalAlignment = .leading
        b.addTarget(self, action: action, for: .touchUpInside)
        return b
    }

    /// Two buttons side by side, so ten of them still leave room for the report.
    private func row(_ items: [(String, Selector)]) -> UIStackView {
        let s = UIStackView(arrangedSubviews: items.map { button($0.0, $0.1) })
        s.axis = .horizontal
        s.spacing = 8
        s.distribution = .fillEqually
        return s
    }

    /// Every button funnels through here: one probe at a time, off the main
    /// thread, with the result stored the moment it exists.
    private func work(_ label: String, _ body: @escaping () -> Void) {
        guard !running else { return }
        running = true
        title = "running \(label)…"
        DispatchQueue.global(qos: .userInitiated).async {
            body()
            DispatchQueue.main.async {
                self.running = false
                self.title = "winios probes · \(DeviceInfo.modelIdentifier)"
                self.refresh()
            }
        }
    }

    // MARK: - the probes

    /// CPU, GPU, benchmark, Windows guests and the safe JIT check are quick and
    /// harmless, so they go first and get saved. The memory ladder goes last
    /// because it may end with the system killing the process; by then
    /// everything else is on disk.
    @objc private func runAll() {
        work("everything") {
            self.cpuProbe()
            self.benchProbe()
            self.gpuProbe()
            self.windowsProbe(single: nil)
            Ladder.climb(host: "app", paused: { !self.isActive })
        }
    }

    @objc private func runCPU()  { work("CPU vectors") { self.cpuProbe() } }
    @objc private func runBench(){ work("benchmark")   { self.benchProbe() } }
    @objc private func runGPU()  { work("GPU")         { self.gpuProbe() } }
    @objc private func runWindows() { work("Windows guests") { self.windowsProbe(single: nil) } }
    @objc private func runX87()  { work("x87")         { self.windowsProbe(single: "nbody32.exe") } }
    @objc private func runLadder() {
        work("memory ladder") { Ladder.climb(host: "app", paused: { !self.isActive }) }
    }

    /// The golden vectors, twice: once decoded and executed (the reference
    /// semantics), once through the ARM64 the dynarec emits into the blessed
    /// arena. Half the recorded x87 vectors are in 53-bit precision, so the
    /// second pass exercises the x87 lowering rather than its callout path.
    private func cpuProbe() {
        DispatchQueue.main.async { self.cpuLine = "running…"; self.refresh() }
        var buf = [CChar](repeating: 0, count: 8192)
        xc_jit_enable(0)
        let bad = xc_selftest(&buf, buf.count, 12)
        var cpu = "interpreter: " + (bad == 0 ? "PASS — matches x86 silicon\n" : "FAIL — \(bad) mismatched\n") + String(cString: buf)
        DispatchQueue.main.async { self.cpuLine = cpu; self.refresh() }

        guard let arena = ensureArena() else {
            cpu += "dynarec:     not run — no blessed arena (see JIT below)\n"
            DispatchQueue.main.async { self.cpuLine = cpu; self.refresh() }
            return
        }
        guard handArenaToXcore(arena) else {
            cpu += "dynarec:     not run — arena too small for xcore (\(arena.pointee.size >> 10) KB)\n"
            DispatchQueue.main.async { self.cpuLine = cpu; self.refresh() }
            return
        }
        xc_jit_enable(1)
        var buf2 = [CChar](repeating: 0, count: 8192)
        let bad2 = xc_selftest(&buf2, buf2.count, 12)
        var blocks: UInt64 = 0, callouts: UInt64 = 0, bytes: UInt64 = 0
        xc_jit_stats(&blocks, &callouts, &bytes)
        var x87n: UInt64 = 0, x87c: UInt64 = 0
        xc_jit_x87_stats(&x87n, &x87c)
        xc_jit_enable(0)
        cpu += "dynarec:     " + (bad2 == 0 ? "PASS — ARM64 code matches x86 silicon\n" : "FAIL — \(bad2) mismatched\n")
            + String(cString: buf2)
            + "    \(blocks) blocks compiled, \(bytes >> 10) KB of ARM64, \(callouts) interpreter callouts\n"
            + "    x87: \(x87n) instructions lowered onto NEON, \(x87c) left to the interpreter\n"
        DispatchQueue.main.async { self.cpuLine = cpu; self.refresh() }
    }

    /// The first real performance numbers this project has: everything in the
    /// docs so far comes from qemu, where absolute figures mean nothing.
    private func benchProbe() {
        DispatchQueue.main.async { self.benchLine = "running…"; self.refresh() }
        if let arena = ensureArena() { _ = handArenaToXcore(arena) }
        var buf = [CChar](repeating: 0, count: 4096)
        let bad = xc_bench(&buf, buf.count, 200_000)
        let text = String(cString: buf) + (bad == 0 ? "" : "  \(bad) case(s) failed to run\n")
        DispatchQueue.main.async { self.benchLine = text; self.refresh() }
    }

    private func gpuProbe() {
        DispatchQueue.main.async { self.gpuLine = "running…"; self.refresh() }
        let gpu = GpuProbe.run()
        DispatchQueue.main.async { self.gpuLine = gpu; self.refresh() }
    }

    /// Windows executables built with mingw-w64, bundled as resources: the PE
    /// loader, the host-implemented kernel32/msvcrt, and the dynarec, end to
    /// end on the device. `single` runs just one of them (the x87 button uses
    /// nbody32.exe, whose float work is all x87).
    private func windowsProbe(single: String?) {
        DispatchQueue.main.async { self.winLine = "running…"; self.refresh() }
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("win32") else {
            DispatchQueue.main.async { self.winLine = "guests not bundled"; self.refresh() }
            return
        }
        if let arena = ensureArena() { _ = handArenaToXcore(arena) }

        // name, arguments, expected exit code
        let all: [(String, [String], Int32)] = [
            ("hello64.exe", ["a", "b"], 7), ("hello32.exe", ["a", "b"], 7),
            ("crt64.exe", [], 3),           ("crt32.exe", [], 3),
            ("nbody64.exe", [], 0),         ("nbody32.exe", [], 0),
        ]
        let cases = single.map { s in all.filter { $0.0 == s } } ?? all

        var text = ""
        var failed = 0
        for (name, args, wantRC) in cases {
            let exe = dir.appendingPathComponent(name).path
            var out = [CChar](repeating: 0, count: 16384)
            var ns: UInt64 = 0, x87n: UInt64 = 0, x87c: UInt64 = 0
            // strdup rather than letting Swift bridge three String?s at once:
            // explicit lifetimes, and nil really is a null pointer.
            let a1 = args.count > 0 ? strdup(args[0]) : nil
            let a2 = args.count > 1 ? strdup(args[1]) : nil
            xc_jit_enable(1)
            let rc = exe.withCString { p in
                win_probe_run(p, a1, a2, &out, out.count, &ns, &x87n, &x87c)
            }
            xc_jit_enable(0)
            if a1 != nil { free(a1) }
            if a2 != nil { free(a2) }
            let got = String(cString: out)
            let want = (try? String(contentsOf: dir.appendingPathComponent(
                            name.replacingOccurrences(of: ".exe", with: ".expected")),
                            encoding: .utf8)) ?? ""
            let ok = rc == wantRC && got.trimmingCharacters(in: .whitespacesAndNewlines)
                                     == want.trimmingCharacters(in: .whitespacesAndNewlines)
            if !ok { failed += 1 }
            text += "  \(ok ? "PASS" : "FAIL")  \(name)  exit \(rc) (want \(wantRC))  \(ns / 1_000_000) ms"
            if x87n + x87c > 0 { text += "   x87 \(x87n) lowered / \(x87c) called out" }
            text += "\n"
            if !ok {
                text += "      got:  \(got.split(separator: "\n").first.map(String.init) ?? "(nothing)")\n"
                text += "      want: \(want.split(separator: "\n").first.map(String.init) ?? "(nothing)")\n"
            }
        }
        let head = failed == 0
            ? "\(cases.count)/\(cases.count) PASS — Windows executables run on this device\n"
            : "\(failed) of \(cases.count) FAILED\n"
        DispatchQueue.main.async { self.winLine = head + text; self.refresh() }
    }

    @objc private func copyReport() {
        UIPasteboard.general.string = results.text
        let old = title
        title = "copied"
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { self.title = old }
    }

    @objc private func resetAll() {
        ResultStore.reset()
        markerPath.withCString { jit_probe_reset($0) }
        ["cpu", "bench", "gpu", "jit", "win"].forEach { store.removeObject(forKey: $0) }
        refresh()
    }

    // MARK: - JIT

    private var markerPath: String {
        (ResultStore.containerDir?.appendingPathComponent("jit.marker").path) ?? ""
    }

    private func cstr<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self)) }
    }

    private func describe(_ r: jit_result) -> String {
        var s = "\(String(cString: jit_state_name(r.state)))"
        s += "   CS flags 0x\(String(r.cs_flags, radix: 16))  debugged=\(r.cs_debugged != 0)"
        s += "  remap_kr=\(r.remap_kr) protect_kr=\(r.protect_kr)"
        let last = cstr(r.last_step)
        if !last.isEmpty { s += "\n    last attempt reached: \(last)" }
        s += "\n    \(cstr(r.detail))"
        return s
    }

    /// Ask StikDebug to attach with a script that can service the bless
    /// breakpoint. On TXM hardware (A15+, M2+) a plain attach is not enough --
    /// the script has to stay to answer `brk #0xf00d`. When the app comes back
    /// to the foreground with CS_DEBUGGED set, the arena test runs by itself.
    @objc private func attachJIT() {
        // Already attached (LiveContainer + StikDebug enable JIT at launch)?
        // Then there is nothing to ask for; run the execution test now.
        var probe = jit_result()
        markerPath.withCString { jit_probe_safe(&probe, $0) }
        if probe.cs_debugged != 0 {
            jitLine = executeArena()        // reuses the arena if this launch already has one
            refresh()
            return
        }
        let bundle = Bundle.main.bundleIdentifier ?? ""
        let pid = getpid()
        let candidates = [
            "stikdebug://enable-jit?bundle-id=\(bundle)&pid=\(pid)&script-name=universal.js",
            "stikjit://enable-jit?bundle-id=\(bundle)&pid=\(pid)&script-name=universal.js",
            "stikjit://attach?pid=\(pid)",
        ]
        for s in candidates {
            if let u = URL(string: s), UIApplication.shared.canOpenURL(u) {
                jitAttachPending = true
                UIApplication.shared.open(u)
                jitLine = "asked StikDebug to attach:\n    \(s)\n    The arena test runs automatically when you come back."
                refresh()
                return
            }
        }
        jitLine = "No StikDebug URL scheme responded. Install StikDebug, or attach it manually and press this button again."
        refresh()
    }

    @objc private func resignedActive() { isActive = false }

    @objc private func becameActive() {
        isActive = true
        guard jitAttachPending else { return }
        var probe = jit_result()
        markerPath.withCString { jit_probe_safe(&probe, $0) }
        guard probe.cs_debugged != 0 else { return }      // not attached yet; keep waiting
        jitAttachPending = false
        runArena()
    }

    /// The process-wide blessed arena (nil until executeArena has run, or if
    /// the bless failed). 1 MB: the size that is known to work on the iPad, and
    /// twenty times what the dynarec self-test compiles (≈50 KB). Whether the
    /// debugger script copes with much larger regions is a separate experiment
    /// -- one that has to be run knowing it may cost the launch.
    private var sharedArena: UnsafeMutablePointer<jit_arena>?
    private var arenaInXcore = false
    private var arenaReport = ""
    private let arenaSize = 1 << 20

    /// The one bless of this launch, on demand. Every button that needs
    /// executable memory goes through here, so none of them can trigger a
    /// second bless: the debugger detaches at the end of the first, and a
    /// second `brk #0xf00d` would be an unserviced breakpoint (the 5c2e468
    /// crash). Returns nil when there is no usable arena.
    private func ensureArena() -> UnsafeMutablePointer<jit_arena>? {
        if sharedArena == nil || sharedArena?.pointee.blessed == 0 {
            let jit = executeArena()
            DispatchQueue.main.async { self.jitLine = jit; self.refresh() }
        }
        guard let a = sharedArena, a.pointee.blessed != 0 else { return nil }
        return a
    }

    /// Point xcore's code emitter at the arena. Idempotent: xcore keeps the
    /// dispatcher it built on first use, and the `ret` probe only ever ran
    /// before this (on the fresh arena), so nothing tramples anything.
    private func handArenaToXcore(_ a: UnsafeMutablePointer<jit_arena>) -> Bool {
        if arenaInXcore { return true }
        arenaInXcore = xc_jit_set_code(a.pointee.rw, a.pointee.rx, a.pointee.size) == 1
        return arenaInXcore
    }

    /// The real protocol, once per launch: allocate RX, have the debugger bless
    /// every 16 KB page, build the RW alias, detach, then write a `ret` and
    /// execute it. Later calls return the same arena and the saved report
    /// without touching the breakpoint again. Thread-agnostic.
    private func executeArena() -> String {
        var r = jit_result()

        // Refuse to walk into a breakpoint that killed a previous launch.
        var probe = jit_result()
        markerPath.withCString { jit_probe_safe(&probe, $0) }
        if probe.cs_debugged == 0 {
            return describe(probe) + "\n    (no debugger attached — use the JIT button to attach StikDebug)"
        }
        if probe.state == JIT_CRASHED && sharedArena == nil {
            return describe(probe) + "\n    (a previous bless/execute crashed; Reset results clears the marker to retry)"
        }

        var fresh: Int32 = 0
        let arena = markerPath.withCString { jit_arena_shared(arenaSize, &r, $0, &fresh) }
        guard let arena else {
            arenaReport = describe(r)
            return arenaReport
        }
        sharedArena = arena
        if fresh == 1 {
            var code: UInt32 = 0xD65F03C0      // AArch64 `ret`
            _ = withUnsafeBytes(of: &code) { raw in
                markerPath.withCString { jit_arena_run(arena, raw.baseAddress, 4, &r, $0) }
            }
            arenaReport = describe(r) + "\n    (debugger was attached — blessed \(arenaSize >> 10) KB and executed directly)"
        }
        return arenaReport
    }

    private func runArena() {
        jitLine = executeArena()
        refresh()
    }

    // MARK: - render

    private func refresh() {
        let high = ResultStore.highWater()
        let avail = Int(os_proc_available_memory()) >> 20
        // The ladder stops with a margin, so the limit is the top rung plus
        // what the OS said was still available at that point.
        let top = ResultStore.readAll().filter { $0.host == "app" }.max { $0.residentMB < $1.residentMB }
        let app: String
        if let t = top, let h = high["app"] {
            app = "\(h) MB held, \(t.availableMB) MB budget left  ->  limit ≈ \(h + t.availableMB) MB"
        } else { app = "not run" }

        var text = """
        \(DeviceInfo.summary())

        1 · CPU CORE (x86 → ARM64)
        \(cpuLine)

        2 · SPEED — interpreter vs dynarec on this silicon
        \(benchLine)

        3 · GPU — Direct3D 9 / 11 / 12 binding model on Metal (d12mt)
        \(gpuLine)

        4 · JIT
        \(jitLine)

        5 · WINDOWS EXECUTABLES (PE loader + kernel32/msvcrt + dynarec)
        \(winLine)

        6 · MEMORY, app process (stops 256 MB short of the kill on purpose;
            pauses while the app is in the background)
            \(app)
            available now \(avail) MB

        RECENT LADDER RUNGS

        """
        for r in ResultStore.readAll().suffix(12) {
            let who = r.host.padding(toLength: 9, withPad: " ", startingAt: 0)
            text += "  \(who) step \(r.step)  resident \(r.residentMB) MB  available \(r.availableMB) MB\n"
        }
        results.text = text
    }
}
