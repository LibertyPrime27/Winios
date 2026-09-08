import UIKit

/// Settings, and the JIT state that belongs with them.
///
/// Mostly things that change what the emulator does. The display mode is the
/// real one — it decides how many pixels a game draws — and the mouse and
/// keyboard settings are the ones people go looking for when a game feels
/// wrong rather than looks wrong.
///
/// The two at the bottom are different in kind: the log and the crash report
/// change nothing about a run, they are what is left to read afterwards.
/// They are here because this is the screen somebody is already on when a
/// game has just misbehaved, and a report they have to go looking for is a
/// report that does not get sent.
///
/// The JIT row is here as well as on the library screen, because this is
/// where someone comes when a game runs badly, and "the dynarec is not on"
/// is the first answer to that question by a wide margin.
final class SettingsViewController: UIViewController {

    private let jit = UILabel()
    private let jitNote = UILabel()
    private let sensLabel = UILabel()
    private let sens = UISlider()
    /// What is plugged in. The first line anyone should read when a game is
    /// not responding to a keyboard or a mouse, so it is a line of its own
    /// rather than a sentence inside a note.
    private let hardware = UILabel()
    private var hardwareObservers: [NSObjectProtocol] = []
    private let crashStatus = UILabel()

    deinit { hardwareObservers.forEach { NotificationCenter.default.removeObserver($0) } }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Settings"
        view.backgroundColor = .systemBackground

        // --- the JIT, first, because it matters most
        jit.font = .preferredFont(forTextStyle: .headline)
        jitNote.font = .preferredFont(forTextStyle: .footnote)
        jitNote.numberOfLines = 0
        refreshJIT()

        // --- what the game is told the screen is
        let display = UISegmentedControl(items: Settings.displayModes.map {
            "\($0.w)×\($0.h)"
        })
        display.selectedSegmentIndex = Settings.displayIndex
        display.addTarget(self, action: #selector(displayChanged(_:)), for: .valueChanged)

        // --- how big a Windows dialog comes out
        let scale = UISegmentedControl(items: Settings.dialogScales.map { "\($0)%" })
        scale.selectedSegmentIndex = Settings.dialogScales.firstIndex(of: Settings.dialogScale) ?? 2
        scale.addTarget(self, action: #selector(scaleChanged(_:)), for: .valueChanged)

        // --- mouse
        sens.minimumValue = 0.2
        sens.maximumValue = 4.0
        sens.value = Float(Settings.mouseSensitivity)
        sens.addTarget(self, action: #selector(sensChanged), for: .valueChanged)
        sensLabel.font = .preferredFont(forTextStyle: .footnote)
        updateSensLabel()

        let invert = UISwitch()
        invert.isOn = Settings.mouseInvertY
        invert.addTarget(self, action: #selector(invertChanged(_:)), for: .valueChanged)

        // --- keyboard
        let keys = UISwitch()
        keys.isOn = Settings.onScreenKeys
        keys.addTarget(self, action: #selector(keysChanged(_:)), for: .valueChanged)

        // --- what is plugged in, and a watch on it changing while this
        //     screen is open: somebody checking this line is quite likely to
        //     be plugging something in as they read it.
        hardware.font = .preferredFont(forTextStyle: .footnote)
        hardware.numberOfLines = 0
        hardwareObservers = HardwareInput.observe { [weak self] in self?.refreshHardware() }
        refreshHardware()

        // --- logs
        let logs = UISwitch()
        logs.isOn = Logs.isEnabled
        logs.addTarget(self, action: #selector(logsChanged(_:)), for: .valueChanged)
        let verbose = UISwitch()
        verbose.isOn = Settings.verboseLogs
        verbose.addTarget(self, action: #selector(verboseChanged(_:)), for: .valueChanged)

        // --- crash reports
        let crashes = UISwitch()
        crashes.isOn = Settings.recordCrashes
        crashes.addTarget(self, action: #selector(crashesChanged(_:)), for: .valueChanged)
        crashStatus.font = .preferredFont(forTextStyle: .footnote)
        crashStatus.numberOfLines = 0
        refreshCrash()

        let stack = UIStackView(arrangedSubviews: [
            jit, jitNote,
            button("Turn the JIT on", #selector(enableJIT)),
            heading("Display"),
            note("What a game is told the screen is. It reads this before it "
                 + "picks a resolution, so a smaller number means fewer pixels "
                 + "to draw — which is the biggest thing you can change for "
                 + "frame rate while drawing still goes through software. "
                 + "Takes effect the next time a game starts."),
            display,
            heading("Installer and dialog size"),
            note("An installer's window was designed for a desktop monitor, "
                 + "so on a tablet it comes out tiny. This tells the program "
                 + "the screen is denser than it is, and it lays its own "
                 + "window out bigger in response — the same thing Windows "
                 + "does on a high-resolution laptop. Takes effect the next "
                 + "time an installer or a dialog opens."),
            scale,
            heading("Hardware keyboard and mouse"),
            hardware,
            note("Both are used the moment they are connected; there is "
                 + "nothing to switch on. A keyboard's keys reach the game "
                 + "with the character its own layout produced rather than "
                 + "one guessed at here, and a mouse reports movement rather "
                 + "than a position — which is what a game doing mouselook "
                 + "needs, and what a pointer stopped at the edge of the "
                 + "screen cannot give it. While a game has its cursor hidden "
                 + "the pointer is captured, so nothing of the system's is "
                 + "drawn over it. If a game is ignoring a keyboard or a "
                 + "mouse, the line above is the thing to check first."),
            heading("Mouse"),
            sensLabel, sens,
            switchRow("Invert vertical look", invert),
            heading("Keyboard"),
            switchRow("On-screen WASD keys", keys),
            note("These hide themselves while a hardware keyboard is "
                 + "connected, so this switch is for keeping them out of the "
                 + "way when there is not one."),
            heading("Logs"),
            note("Off by default. When on, every run is written to a file on "
                 + "this device along with what the device is — model, iOS "
                 + "version, page size, memory, whether the JIT was on and what "
                 + "resolution the game was told about. That context is usually "
                 + "the difference between a report you can act on and one you "
                 + "cannot. Nothing leaves the device on its own."),
            switchRow("Record logs", logs),
            note("Detailed adds winrun's own narration to every run report: "
                 + "each module loaded and from where, each DLL search, each "
                 + "GetProcAddress, the sound and input objects a game creates. "
                 + "Long, and slightly slower — turn it on for a program that "
                 + "is being chased, off for playing."),
            switchRow("Detailed run log", verbose),
            row([("View log", #selector(viewLog)), ("Copy", #selector(copyLog)),
                 ("Clear", #selector(clearLog))]),
            heading("Crash reports"),
            note("Off by default because it costs performance. When on, the "
                 + "app installs its own handlers for the signals a crash "
                 + "arrives as, and writes the signal, the fault address and "
                 + "a backtrace into a file that was already open — so the "
                 + "report survives the process dying. There is no start "
                 + "button, because a crash never announces itself: either "
                 + "the handlers were armed before it or there is nothing to "
                 + "read afterwards."),
            note("Worth having because the system's own report usually names "
                 + "the wrong program. Launched through LiveContainer and "
                 + "StikDebug, the process that dies is often one of those, "
                 + "and the report is filed against it. Whatever a game was "
                 + "doing when it went — which one, 32- or 64-bit, with or "
                 + "without the JIT — is recorded here either way."),
            switchRow("Record crashes", crashes),
            crashStatus,
            row([("View crash", #selector(viewCrash)), ("Copy", #selector(copyCrash)),
                 ("Clear", #selector(clearCrash))]),
            UIView(),
        ])
        stack.axis = .vertical
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false

        // In a scroll view since the crash section was added: the screen was
        // already full on a phone in landscape, and a stack pinned to the
        // safe area does not overflow, it squashes -- the last few rows come
        // out unreadable rather than out of sight, which is a worse way to
        // fail because nothing about it looks wrong.
        let scroll = UIScrollView()
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.alwaysBounceVertical = true
        view.addSubview(scroll)
        scroll.addSubview(stack)

        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: g.topAnchor),
            scroll.leadingAnchor.constraint(equalTo: g.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: g.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: g.bottomAnchor),
            stack.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor, constant: 16),
            stack.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor, constant: -16),
            stack.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor, constant: -16),
            // The content guide alone gives the stack no width, and every
            // wrapping label then lays itself out at zero. This is what makes
            // it scroll in one direction only.
            stack.widthAnchor.constraint(equalTo: scroll.frameLayoutGuide.widthAnchor, constant: -32),
        ])
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        // The arena may have been blessed from Diagnostics since this screen
        // was last looked at.
        refreshJIT()
        refreshHardware()
        refreshCrash()
    }

    // MARK: - rows

    private func heading(_ s: String) -> UILabel {
        let l = UILabel()
        l.text = s
        l.font = .preferredFont(forTextStyle: .headline)
        return l
    }
    private func note(_ s: String) -> UILabel {
        let l = UILabel()
        l.text = s
        l.font = .preferredFont(forTextStyle: .footnote)
        l.textColor = .secondaryLabel
        l.numberOfLines = 0
        return l
    }
    private func switchRow(_ title: String, _ sw: UISwitch) -> UIStackView {
        let l = UILabel()
        l.text = title
        l.numberOfLines = 0
        let row = UIStackView(arrangedSubviews: [l, sw])
        row.axis = .horizontal
        row.spacing = 12
        row.alignment = .center
        return row
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
        s.axis = .horizontal
        s.spacing = 8
        s.distribution = .fillEqually
        return s
    }

    // MARK: - changes

    /// The JIT needs a debugger to authorise executable memory, once per
    /// launch. That screen is the only part of the old diagnostics anyone
    /// should ever need, so this is where it is reached from.
    @objc private func enableJIT() {
        navigationController?.pushViewController(ProbeViewController(onlyJIT: true), animated: true)
    }

    @objc private func logsChanged(_ sw: UISwitch) { Logs.isEnabled = sw.isOn }
    @objc private func verboseChanged(_ sw: UISwitch) { Settings.verboseLogs = sw.isOn }

    @objc private func viewLog() {
        let text = Logs.text()
        pushText(title: "Log", text: text.isEmpty
            ? (Logs.isEnabled ? "Nothing recorded yet — run a program."
                              : "Log recording is off. Turn it on above, then run a program.")
            : text)
    }

    /// One screen for both the log and the crash report, because they are the
    /// same thing to read: a wall of monospaced text somebody is going to
    /// copy out of.
    private func pushText(title: String, text: String) {
        let vc = UIViewController()
        vc.title = title
        vc.view.backgroundColor = .systemBackground
        let tv = UITextView()
        tv.isEditable = false
        tv.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        tv.text = text
        tv.translatesAutoresizingMaskIntoConstraints = false
        vc.view.addSubview(tv)
        let g = vc.view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            tv.topAnchor.constraint(equalTo: g.topAnchor),
            tv.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 8),
            tv.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -8),
            tv.bottomAnchor.constraint(equalTo: g.bottomAnchor),
        ])
        navigationController?.pushViewController(vc, animated: true)
    }

    /// Turning it on arms the handlers in this process rather than waiting
    /// for the next launch: somebody switching this on has just had a crash
    /// and asking them to relaunch first is asking them to reproduce it
    /// twice. Turning it off takes effect at the next launch — the handlers
    /// already installed are left where they are, since taking them down
    /// while a guest is running is more risk than the switch is worth.
    @objc private func crashesChanged(_ sw: UISwitch) {
        Settings.recordCrashes = sw.isOn
        CrashReports.arm()
        refreshCrash()
    }

    @objc private func viewCrash() {
        pushText(title: "Crash", text: CrashReports.report())
    }

    @objc private func copyCrash() {
        UIPasteboard.general.string = CrashReports.report()
        status("Copied.")
    }

    @objc private func clearCrash() {
        CrashReports.clear()
        refreshCrash()
        status("Crash report cleared.")
    }

    @objc private func copyLog() {
        let t = Logs.text()
        UIPasteboard.general.string = t.isEmpty ? "(nothing recorded)" : t
        status("Copied.")
    }

    @objc private func clearLog() {
        Logs.clear()
        status("Log cleared.")
    }

    /// A one-line acknowledgement. A button that does something invisible is
    /// a button people press twice.
    private func status(_ s: String) {
        jitNote.text = s
        // Put the real explanation back, so the acknowledgement does not
        // become the permanent caption under the JIT state.
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.refreshJIT()
        }
    }

    @objc private func scaleChanged(_ c: UISegmentedControl) {
        guard Settings.dialogScales.indices.contains(c.selectedSegmentIndex) else { return }
        Settings.dialogScale = Settings.dialogScales[c.selectedSegmentIndex]
    }

    @objc private func displayChanged(_ c: UISegmentedControl) {
        Settings.displayIndex = c.selectedSegmentIndex
    }
    @objc private func sensChanged() {
        Settings.mouseSensitivity = Double(sens.value)
        updateSensLabel()
    }
    private func updateSensLabel() {
        sensLabel.text = String(format: "Sensitivity: %.2f×", Settings.mouseSensitivity)
    }
    @objc private func invertChanged(_ sw: UISwitch) { Settings.mouseInvertY = sw.isOn }
    @objc private func keysChanged(_ sw: UISwitch) { Settings.onScreenKeys = sw.isOn }

    private func refreshHardware() {
        hardware.text = HardwareInput.summary
        // Full-strength text when something is attached and dimmed when
        // nothing is. Not a warning either way: running with neither is the
        // ordinary case, and colouring it as a fault would be telling people
        // something is wrong when nothing is.
        let any = HardwareInput.keyboardConnected || HardwareInput.mouseConnected
        hardware.textColor = any ? .label : .secondaryLabel
    }

    private func refreshCrash() {
        crashStatus.text = CrashReports.summary
        crashStatus.textColor = CrashReports.hasPrevious ? .systemRed : .secondaryLabel
    }

    private func refreshJIT() {
        let on = Settings.jitReady
        jit.text = on ? "JIT enabled" : "JIT disabled"
        jit.textColor = on ? .systemGreen : .systemRed
        // The real reason, from the probe, rather than one guess for every
        // way this can be off.
        jitNote.text = JIT.explanation
        jitNote.textColor = on ? .secondaryLabel : .systemRed
    }
}
