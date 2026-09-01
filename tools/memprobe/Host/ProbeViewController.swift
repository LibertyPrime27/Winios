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
            button("2b · Memory ladder in extension", #selector(runInExtension)),
            button("3 · Check JIT", #selector(runJIT)),
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

    // MARK: - 3. JIT

    @objc private func runJIT() {
        var r = jit_result()
        // The marker lets a probe that faults last launch be detected instead of
        // crash-looping the app on every start.
        let marker = (ResultStore.containerDir?.appendingPathComponent("jit.marker").path) ?? ""
        marker.withCString { jit_probe(&r, $0) }
        let detail = withUnsafeBytes(of: r.detail) { raw -> String in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        jitLine = "\(String(cString: jit_state_name(r.state)))  (CS flags 0x\(String(r.cs_flags, radix: 16)))\n    \(detail)"
        refresh()
    }

    // MARK: - render

    private func refresh() {
        let high = ResultStore.highWater()
        let app = high["app"].map { "\($0) MB" } ?? "not run"
        let ext = high["extension"].map { "\($0) MB" } ?? "not run"
        let avail = Int(os_proc_available_memory()) >> 20

        var text = """
        1 · CPU CORE (x86 → ARM64)
        \(cpuLine)

        2 · MEMORY, high-water resident
            app process        \(app)
            extension process  \(ext)
            available now      \(avail) MB

            The extension figure is the one that matters: it decides whether
            wineserver can live in an app extension, and so whether 64-bit
            games are reachable at all. A ladder that "stopped voluntarily"
            in the log hit the ceiling rather than a limit.

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
