import UIKit

/// Settings, and the JIT state that belongs with them.
///
/// Only things that change what the emulator does. The display mode is the
/// real one — it decides how many pixels a game draws — and the mouse and
/// keyboard settings are the ones people go looking for when a game feels
/// wrong rather than looks wrong.
///
/// The JIT row is here as well as on the library screen, because this is
/// where someone comes when a game runs badly, and "the dynarec is not on"
/// is the first answer to that question by a wide margin.
final class SettingsViewController: UIViewController {

    private let jit = UILabel()
    private let jitNote = UILabel()
    private let sensLabel = UILabel()
    private let sens = UISlider()

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

        // --- logs
        let logs = UISwitch()
        logs.isOn = Logs.isEnabled
        logs.addTarget(self, action: #selector(logsChanged(_:)), for: .valueChanged)

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
            heading("Mouse"),
            sensLabel, sens,
            switchRow("Invert vertical look", invert),
            heading("Keyboard"),
            switchRow("On-screen WASD keys", keys),
            note("Turn these off when a hardware keyboard is attached — they "
                 + "only cover the game up."),
            heading("Logs"),
            note("Off by default. When on, every run is written to a file on "
                 + "this device along with what the device is — model, iOS "
                 + "version, page size, memory, whether the JIT was on and what "
                 + "resolution the game was told about. That context is usually "
                 + "the difference between a report you can act on and one you "
                 + "cannot. Nothing leaves the device on its own."),
            switchRow("Record logs", logs),
            row([("View log", #selector(viewLog)), ("Copy", #selector(copyLog)),
                 ("Clear", #selector(clearLog))]),
            UIView(),
        ])
        stack.axis = .vertical
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: g.topAnchor, constant: 16),
            stack.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -16),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: g.bottomAnchor, constant: -16),
        ])
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        // The arena may have been blessed from Diagnostics since this screen
        // was last looked at.
        refreshJIT()
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

    @objc private func viewLog() {
        let text = Logs.text()
        let vc = UIViewController()
        vc.title = "Log"
        vc.view.backgroundColor = .systemBackground
        let tv = UITextView()
        tv.isEditable = false
        tv.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        tv.text = text.isEmpty
            ? (Logs.isEnabled ? "Nothing recorded yet — run a program."
                              : "Log recording is off. Turn it on above, then run a program.")
            : text
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

    private func refreshJIT() {
        let on = Settings.jitReady
        jit.text = on ? "JIT enabled" : "JIT disabled"
        jit.textColor = on ? .systemGreen : .systemRed
        jitNote.text = on
            ? "Guest code is compiled to ARM64. This is the fast path — roughly "
            + "a hundred times the interpreter."
            : "Guest code is being interpreted, which is far slower. The dynarec "
            + "needs an executable memory region that a debugger has authorised, "
            + "once per launch: open Diagnostics and run step 4, \"JIT: attach "
            + "StikDebug\"."
        jitNote.textColor = on ? .secondaryLabel : .systemRed
    }
}
