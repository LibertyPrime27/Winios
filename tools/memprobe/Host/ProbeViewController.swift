import UIKit
import os

/// The questions that gate everything else, on one screen, one tap.
///
///  1. Does the CPU core behave on ARM64 the way it does on x86? (golden vectors)
///  2. Does the D3D binding design (heap == argument buffer) hold on this GPU,
///     for D3D9, D3D11 and D3D12 shaders alike?
///  3. Is JIT actually available, not just flagged?
///  4. How much memory will the app process hold? (the ladder)
///
/// Answering these on real hardware is the entire purpose of this build. It is
/// not a game runner and does not pretend to be one. Every result is persisted
/// as soon as it exists, so a memory-ladder kill costs nothing.
final class ProbeViewController: UIViewController {

    private let results = UITextView()
    private let store = UserDefaults.standard
    private var cpuLine: String { get { store.string(forKey: "cpu") ?? "not run" } set { store.set(newValue, forKey: "cpu") } }
    private var gpuLine: String { get { store.string(forKey: "gpu") ?? "not run" } set { store.set(newValue, forKey: "gpu") } }
    private var jitLine: String { get { store.string(forKey: "jit") ?? "not run" } set { store.set(newValue, forKey: "jit") } }
    private var running = false
    private var jitAttachPending = false

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "winios probes · \(DeviceInfo.modelIdentifier)"
        view.backgroundColor = .systemBackground

        results.isEditable = false
        results.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        results.alwaysBounceVertical = true

        let stack = UIStackView(arrangedSubviews: [
            button("▶  Run all probes  (CPU · GPU ×3 APIs · JIT check · memory)", #selector(runAll)),
            button("JIT: attach StikDebug, then execute in a blessed arena", #selector(attachJIT)),
            button("Copy report", #selector(copyReport)),
            button("Reset results", #selector(resetAll)),
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

    // MARK: - run everything

    /// CPU, GPU and the safe JIT check are quick and harmless, so they go
    /// first and get saved. The memory ladder goes last because it may end
    /// with the system killing the process; by then everything else is on disk.
    @objc private func runAll() {
        guard !running else { return }
        running = true
        cpuLine = "running…"; gpuLine = "queued"; jitLine = "queued"; refresh()
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 8192)
            let bad = xc_selftest(&buf, buf.count, 12)
            let cpu = (bad == 0 ? "PASS — matches x86 silicon\n" : "FAIL — \(bad) mismatched\n") + String(cString: buf)
            DispatchQueue.main.async { self.cpuLine = cpu; self.gpuLine = "running…"; self.refresh() }

            let gpu = GpuProbe.run()
            DispatchQueue.main.async { self.gpuLine = gpu; self.jitLine = "checking…"; self.refresh() }

            // If a debugger is already attached -- LiveContainer + StikDebug do
            // that before the app's first instruction -- go straight to the real
            // test. Only skip it if a previous attempt crashed here (marker).
            var r = jit_result()
            self.markerPath.withCString { jit_probe_safe(&r, $0) }
            let jit: String
            if r.cs_debugged != 0 && r.state != JIT_CRASHED {
                jit = self.executeArena() + "\n    (debugger was already attached at launch — executed directly)"
            } else if r.cs_debugged != 0 {
                jit = self.describe(r) + "\n    (a previous execute crashed; Reset results clears the marker to retry)"
            } else {
                jit = self.describe(r) + "\n    (no debugger attached — use the JIT button to attach StikDebug)"
            }
            DispatchQueue.main.async { self.jitLine = jit; self.refresh() }

            Ladder.climb(host: "app")
            DispatchQueue.main.async { self.running = false; self.refresh() }
        }
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
        ["cpu", "gpu", "jit"].forEach { store.removeObject(forKey: $0) }
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
            jitLine = probe.state == JIT_CRASHED
                ? describe(probe) + "\n    (a previous execute crashed; Reset results clears the marker to retry)"
                : executeArena() + "\n    (debugger was already attached — executed directly)"
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

    @objc private func becameActive() {
        guard jitAttachPending else { return }
        var probe = jit_result()
        markerPath.withCString { jit_probe_safe(&probe, $0) }
        guard probe.cs_debugged != 0 else { return }      // not attached yet; keep waiting
        jitAttachPending = false
        runArena()
    }

    /// The real protocol: allocate RX, have the debugger bless every 16 KB page,
    /// build the RW alias, detach, then write and execute. Thread-agnostic;
    /// returns the report line.
    private func executeArena() -> String {
        var r = jit_result()
        var arena = jit_arena()
        let ok = markerPath.withCString { jit_arena_create(&arena, 1 << 20, &r, $0) }
        if ok == 1 {
            var code: UInt32 = 0xD65F03C0      // AArch64 `ret`
            _ = withUnsafeBytes(of: &code) { raw in
                markerPath.withCString { jit_arena_run(&arena, raw.baseAddress, 4, &r, $0) }
            }
            jit_arena_free(&arena)
        }
        return describe(r)
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

        2 · GPU — Direct3D 9 / 11 / 12 binding model on Metal (d12mt)
        \(gpuLine)

        3 · JIT
        \(jitLine)

        4 · MEMORY, app process (stops 256 MB short of the kill on purpose)
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
