import UIKit

/// Runs the ladder in the app process, launches it in the extension process,
/// and reports both. The comparison is the point: the app number on its own
/// says nothing, because the question is whether the extension is treated
/// differently.
final class ProbeViewController: UIViewController {

    private let results = UITextView()

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "MemProbe"
        view.backgroundColor = .systemBackground

        results.isEditable = false
        results.font = .monospacedSystemFont(ofSize: 13, weight: .regular)

        let stack = UIStackView(arrangedSubviews: [
            button("Run ladder in app process", #selector(runInApp)),
            button("Run ladder in extension", #selector(runInExtension)),
            button("Reset log", #selector(resetLog)),
            results,
        ])
        stack.axis = .vertical
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)

        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: g.topAnchor, constant: 16),
            stack.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -16),
            stack.bottomAnchor.constraint(equalTo: g.bottomAnchor, constant: -16),
        ])
        refresh()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        refresh()   // the extension may have died and been recorded since
    }

    private func button(_ title: String, _ action: Selector) -> UIButton {
        var c = UIButton.Configuration.bordered()
        c.title = title
        let b = UIButton(configuration: c)
        b.addTarget(self, action: action, for: .touchUpInside)
        return b
    }

    @objc private func runInApp() {
        // Off the main thread so the watchdog does not kill us for the wrong
        // reason -- a hang termination would look exactly like a memory kill.
        DispatchQueue.global(qos: .userInitiated).async {
            Ladder.climb(host: "app")
            DispatchQueue.main.async { self.refresh() }
        }
    }

    /// The share sheet is the only way to hand control to a share extension.
    /// Pick "MemProbe" from the list; it will climb and probably be killed.
    @objc private func runInExtension() {
        let vc = UIActivityViewController(activityItems: ["winios memprobe"], applicationActivities: nil)
        vc.popoverPresentationController?.sourceView = view
        present(vc, animated: true)
    }

    @objc private func resetLog() {
        ResultStore.reset()
        refresh()
    }

    private func refresh() {
        let high = ResultStore.highWater()
        let app = high["app"].map { "\($0) MB" } ?? "not run"
        let ext = high["extension"].map { "\($0) MB" } ?? "not run"

        var text = """
        High-water resident, by process
        ───────────────────────────────
          app process        \(app)
          extension process  \(ext)

        Device reports os_proc_available_memory() = \(Int(os_proc_available_memory()) >> 20) MB right now.

        A ladder that "stopped voluntarily" in the log hit the ceiling, not a
        limit -- raise ceilingMB and rerun. A ladder whose last rung is the last
        line was killed, and that rung is the answer.

        Recent rungs
        ────────────

        """
        for r in ResultStore.readAll().suffix(24) {
            text += String(format: "  %-9@ step %3d  resident %5d MB  available %5d MB\n",
                           r.host as NSString, r.step, r.residentMB, r.availableMB)
        }
        results.text = text
    }
}
