import UIKit
import os

/// The three questions that gate everything else, on one screen.
///
///  1. Does the CPU core behave on ARM64 the way it does on x86? (golden vectors)
///  2. Can an app extension hold enough memory to be a wineserver? (the ladder)
///  3. Is JIT actually available, not just flagged? (the probe)
///
/// Answering these on real hardware is the entire purpose of this build. It is
/// not a game runner and does not pretend to be one.
final class ProbeViewController: UIViewController {

    private let results = UITextView()
    private var cpuLine = "not run"
    private var jitLine = "not run"

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "winios probes"
        view.backgroundColor = .systemBackground

        results.isEditable = false
        results.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        results.alwaysBounceVertical = true

        let stack = UIStackView(arrangedSubviews: [
            button("1 · Run CPU self-test", #selector(runCPU)),
            button("2 · Memory ladder in app", #selector(runInApp)),
            button("2b · Memory ladder in extension (direct)", #selector(runInExtensionDirect)),
            button("2c · … via share sheet (fallback)", #selector(runInExtension)),
            button("3a · Check JIT (safe)", #selector(runJITSafe)),
            button("3b · Attach StikDebug (universal script)", #selector(attachJIT)),
            button("3c · Create blessed arena + execute", #selector(runArena)),
            button("Reset JIT marker", #selector(resetJIT)),
            button("Reset memory log", #selector(resetLog)),
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
        refresh()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        refresh()   // the extension may have been killed and recorded since
    }

    private func button(_ title: String, _ action: Selector) -> UIButton {
        var c = UIButton.Configuration.bordered()
        c.title = title
        c.contentInsets = .init(top: 8, leading: 12, bottom: 8, trailing: 12)
        let b = UIButton(configuration: c)
        b.addTarget(self, action: action, for: .touchUpInside)
        return b
    }

    // MARK: - 1. CPU core

    /// Replays post-states recorded from a real x86-64 CPU on the CI runner.
    /// A mismatch means the interpreter is architecture-dependent somewhere —
    /// the class of bug that otherwise only ever shows up on device.
    @objc private func runCPU() {
        cpuLine = "running…"; refresh()
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 8192)
            let bad = xc_selftest(&buf, buf.count, 12)
            let report = String(cString: buf)
            DispatchQueue.main.async {
                self.cpuLine = (bad == 0 ? "PASS — matches x86 silicon\n" : "FAIL — \(bad) mismatched\n") + report
                self.refresh()
            }
        }
    }

    // MARK: - 2. Memory

    @objc private func runInApp() {
        // Off the main thread: a watchdog hang-kill would look exactly like a
        // memory kill and would corrupt the measurement.
        DispatchQueue.global(qos: .userInitiated).async {
            Ladder.climb(host: "app")
            DispatchQueue.main.async { self.refresh() }
        }
    }

    /// A share extension can only be entered through the share sheet.
    /// Pick "MemProbe"; it will climb and most likely be killed — that is the
    /// measurement, not a crash.
    @objc private func runInExtension() {
        let vc = UIActivityViewController(activityItems: ["winios memprobe"], applicationActivities: nil)
        vc.popoverPresentationController?.sourceView = view
        present(vc, animated: true)
    }

    @objc private func resetLog() { ResultStore.reset(); refresh() }

    private let extensionID = "winios.memprobe.app.probe"
    private var extLine = ""

    /// Launch the extension the way LiveContainer launches LiveProcess: through
    /// NSExtension, no share sheet. The interruption callback is the result --
    /// the system killing the extension means the ladder found the limit.
    @objc private func runInExtensionDirect() {
        var path: NSString? = nil
        guard ExtensionLauncher.extensionEmbedded(extensionID, path: &path) else {
            extLine = "EXTENSION NOT IN BUNDLE. The signing tool stripped PlugIns. In Sideloadly, turn off 'Remove app extensions' and reinstall."
            refresh(); return
        }
        extLine = "launching…"; refresh()
        ExtensionLauncher.launch(extensionID) { [weak self] event, detail in
            DispatchQueue.main.async {
                guard let self else { return }
                switch event {
                case "interrupted":
                    self.extLine = "extension KILLED by the system — the last rung below is the limit"
                default:
                    self.extLine = "\(event): \(detail)"
                }
                self.refresh()
            }
        }
    }

    // MARK: - 3. JIT

    private var markerPath: String {
        (ResultStore.containerDir?.appendingPathComponent("jit.marker").path) ?? ""
    }

    private func cstr<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
    }

    private func show(_ r: jit_result) {
        var s = "\(String(cString: jit_state_name(r.state)))"
        s += "   CS flags 0x\(String(r.cs_flags, radix: 16))"
        s += "  debugged=\(r.cs_debugged != 0)"
        s += "  remap_kr=\(r.remap_kr) protect_kr=\(r.protect_kr)"
        let last = cstr(r.last_step)
        if !last.isEmpty { s += "\n    last attempt reached: \(last)" }
        s += "\n    \(cstr(r.detail))"
        jitLine = s
        refresh()
    }

    /// Everything except the jump. Cannot fault, so it is safe to run first and
    /// tells us most of what we need.
    @objc private func runJITSafe() {
        var r = jit_result()
        markerPath.withCString { jit_probe_safe(&r, $0) }
        show(r)
    }

    /// Ask StikDebug to attach with a script that can service the bless
    /// breakpoint. On TXM hardware (A15+, M2+, so every M-series iPad) a plain
    /// attach is not enough — the script has to stay to answer `brk #0xf00d`.
    @objc private func attachJIT() {
        let bundle = Bundle.main.bundleIdentifier ?? ""
        let pid = getpid()
        let candidates = [
            "stikdebug://enable-jit?bundle-id=\(bundle)&pid=\(pid)&script-name=universal.js",
            "stikjit://enable-jit?bundle-id=\(bundle)&pid=\(pid)&script-name=universal.js",
            "stikjit://attach?pid=\(pid)",
        ]
        for s in candidates {
            if let u = URL(string: s), UIApplication.shared.canOpenURL(u) {
                UIApplication.shared.open(u)
                jitLine = "asked StikDebug to attach:\n    \(s)\n    Come back once it reports success, then run 3c."
                refresh()
                return
            }
        }
        jitLine = "No StikDebug URL scheme responded. Install StikDebug, or attach it manually and then run 3c."
        refresh()
    }

    /// The real protocol: allocate RX, have the debugger bless every 16 KB page,
    /// build the RW alias, detach, then write and execute. Everything before the
    /// bless is what my earlier probe was missing.
    @objc private func runArena() {
        var r = jit_result()
        var arena = jit_arena()
        let ok = markerPath.withCString { jit_arena_create(&arena, 1 << 20, &r, $0) }
        if ok == 1 {
            // AArch64 `ret`.
            var code: UInt32 = 0xD65F03C0
            _ = withUnsafeBytes(of: &code) { raw in
                markerPath.withCString { jit_arena_run(&arena, raw.baseAddress, 4, &r, $0) }
            }
            jit_arena_free(&arena)
        }
        show(r)
    }

    @objc private func resetJIT() {
        markerPath.withCString { jit_probe_reset($0) }
        jitLine = "marker cleared — the execute test can be retried"
        refresh()
    }

    // MARK: - render

    private func refresh() {
        let high = ResultStore.highWater()
        let app = high["app"].map { "\($0) MB" } ?? "not run"
        let ext = high["extension"].map { "\($0) MB" } ?? "not run"
        let avail = Int(os_proc_available_memory()) >> 20

        var path: NSString? = nil
        let embedded = ExtensionLauncher.extensionEmbedded(extensionID, path: &path)

        var text = """
        1 · CPU CORE (x86 → ARM64)
        \(cpuLine)

        2 · MEMORY, high-water resident
            app process        \(app)
            extension process  \(ext)
            available now      \(avail) MB
            extension in bundle: \(embedded ? "yes" : "NO — stripped at signing; see 2b")
            \(extLine)

            ►► The EXTENSION figure is the measurement that matters. It decides
            whether wineserver can live in an app extension, and so whether
            64-bit games are reachable at all. Run 2b: share sheet → MemProbe.
            The extension vanishing IS the result, not a crash.

            "stopped voluntarily" in the log means the ceiling was reached
            rather than a limit — the process was never killed.

        3 · JIT
        \(jitLine)

        RECENT LADDER RUNGS

        """
        for r in ResultStore.readAll().suffix(20) {
            let who = r.host.padding(toLength: 9, withPad: " ", startingAt: 0)
            text += "  \(who) step \(r.step)  resident \(r.residentMB) MB  available \(r.availableMB) MB\n"
        }
        results.text = text
    }
}
