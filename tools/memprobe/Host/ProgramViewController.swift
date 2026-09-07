import UIKit

/// One program: run it, watch it, read what happened.
///
/// Two ways to run, and the difference matters. **Run** carries on past
/// functions we have not implemented — they log themselves and return zero —
/// so one run names everything the program wanted. That is how to learn what
/// to build next, and it is *not* a way to run something for real: zero means
/// failure for most of the Win32 API but success for some, so a program in
/// this mode can wander somewhere a real Windows would never have let it go.
/// **Run strictly** stops at the first missing function, which is the honest
/// answer to "does this actually work yet".
///
/// Either way the output ends with the run report, and it is kept, because the
/// person who reads it is usually not the person who tapped the button.
final class ProgramViewController: UIViewController {

    private var program: Program
    private let output = UITextView()
    private let status = UILabel()
    private var running = false

    init(_ p: Program) {
        self.program = p
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = program.name
        view.backgroundColor = .systemBackground

        status.font = .preferredFont(forTextStyle: .footnote)
        status.numberOfLines = 0
        status.text = program.importsMissing < 0 ? "not examined yet" : program.subtitle

        output.isEditable = false
        output.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        output.text = "not run yet"

        let stack = UIStackView(arrangedSubviews: [
            status,
            row([("▶  Run", #selector(runKeepGoing)), ("Run strictly", #selector(runStrict))]),
            row([("What it needs", #selector(showImports)), ("Stop", #selector(stop))]),
            row([("Full screen (live frames)", #selector(runFullScreen)), ("Copy", #selector(copyOut))]),
            output,
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
    private func row(_ items: [(String, Selector)]) -> UIStackView {
        let s = UIStackView(arrangedSubviews: items.map { button($0.0, $0.1) })
        s.axis = .horizontal; s.spacing = 8; s.distribution = .fillEqually
        return s
    }

    @objc private func stop() { win_probe_request_stop(); title = "stopping…" }
    @objc private func copyOut() { UIPasteboard.general.string = output.text }

    @objc private func showImports() {
        guard let exe = ProgramStore.exeURL(program) else { return }
        output.text = "examining…"
        DispatchQueue.global(qos: .userInitiated).async {
            var buf = [CChar](repeating: 0, count: 262144)
            _ = exe.path.withCString { win_probe_imports($0, &buf, buf.count) }
            let text = String(cString: buf)
            DispatchQueue.main.async { self.output.text = text }
        }
    }

    @objc private func runKeepGoing() { run(keepGoing: true) }
    @objc private func runStrict()    { run(keepGoing: false) }

    private func run(keepGoing: Bool) {
        guard !running, let exe = ProgramStore.exeURL(program) else { return }
        running = true
        title = keepGoing ? "running…" : "running (strict)…"
        output.text = keepGoing
            ? "running, carrying on past anything unimplemented…\n"
            : "running, stopping at the first unimplemented function…\n"

        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            guard let self else { return }
            var out = [CChar](repeating: 0, count: 1 << 20)
            var ns: UInt64 = 0
            xc_jit_enable(1)
            // The program's own DLL directory goes with it: an imported game
            // loads its libraries from beside its executable, and for a deeply
            // nested one that is not the folder the library shows.
            let dll = ProgramStore.dllDirURL(self.program)?.path ?? ""
            let rc = exe.path.withCString { p in
                dll.withCString { d in
                    win_probe_run_dir(p, dll.isEmpty ? nil : d, keepGoing ? 1 : 0,
                                      120, &out, out.count, &ns)
                }
            }
            xc_jit_enable(0)
            var text = "\(exe.lastPathComponent) exited \(rc) after \(ns / 1_000_000) ms\n"
            switch rc {
            case 124: text += "(stopped by the time limit or the Stop button)\n"
            case 127: text += "(it called something we cannot even return from — see the report)\n"
            case 125: text += "(it faulted — the report says where)\n"
            default: break
            }
            text += "\n" + String(cString: out)

            var p = self.program
            p.lastExit = rc
            p.lastRunMs = ns / 1_000_000
            ProgramStore.update(p)

            DispatchQueue.main.async {
                self.program = p
                self.status.text = p.subtitle
                self.output.text = text
                self.title = self.program.name
                self.running = false
            }
        }
    }

    /// For a program that draws: its frames, as they arrive, full screen.
    @objc private func runFullScreen() {
        guard let exe = ProgramStore.exeURL(program), let c = ExeBrowser.driveC else { return }
        let rel = exe.path.replacingOccurrences(of: c.path + "/", with: "")
        let vc = GuestViewController(exe: rel, root: c)
        vc.modalPresentationStyle = .fullScreen
        present(vc, animated: true)
    }
}
