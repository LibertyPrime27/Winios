import UIKit
import UniformTypeIdentifiers

/// Bringing a Windows program in from outside, in the two shapes it arrives in.
///
/// A download is either **a game** — already installed, a folder of files,
/// usually inside a zip — or **an installer**, whose output is the game. They
/// need opposite treatment, and getting it wrong is not harmless: copying an
/// installer in as a game leaves a library entry that opens a dialog and
/// stops, and there is no dialog here to click.
///
/// So the file is examined first and the likely mode offered as the default,
/// with the other one still one tap away. The examination is cheap — it reads
/// the first few megabytes and the last few kilobytes and looks for the marker
/// the installer's builder left — and it costs nothing to be wrong about,
/// because the user can override it.
///
/// Everything the importer decides is shown rather than summarised. Which
/// executable it picked and *why*, what else it found, how many files
/// appeared: an import that went slightly wrong is far easier to fix when the
/// reasoning is visible than when the answer is "done".
final class ImportViewController: UIViewController, UIDocumentPickerDelegate {

    private let output = UITextView()
    private let status = UILabel()
    private let spinner = UIActivityIndicatorView(style: .medium)
    private var chooseButton: UIButton!
    private var addButton: UIButton!

    /// The finished import, held until the user accepts it. Nothing goes into
    /// the library until they do — an import they did not want should not
    /// leave an entry behind.
    private var pending: Program?
    private var busy = false

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Add a program"
        view.backgroundColor = .systemBackground

        status.font = .preferredFont(forTextStyle: .footnote)
        status.numberOfLines = 0
        status.text = "A folder or a .zip for a game you have already got; "
                    + "a setup .exe to install one. Either way it is copied onto "
                    + "this app's own C: drive."

        output.isEditable = false
        output.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        output.text = ""

        chooseButton = button("Choose a file or folder", #selector(choose))
        addButton = button("Add to library", #selector(addToLibrary))
        addButton.isEnabled = false

        let bar = UIStackView(arrangedSubviews: [spinner, UIView()])
        bar.axis = .horizontal
        bar.spacing = 8

        let stack = UIStackView(arrangedSubviews: [status, chooseButton, addButton, bar, output])
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

    // MARK: - picking

    @objc private func choose() {
        guard !busy else { return }
        // .item and .folder, not a .exe type: iOS has no UTI for a PE binary,
        // and a folder is the usual pick for a game.
        let p = UIDocumentPickerViewController(forOpeningContentTypes: [.item, .folder],
                                               asCopy: false)
        p.delegate = self
        p.allowsMultipleSelection = false
        present(p, animated: true)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {}

    func documentPicker(_ controller: UIDocumentPickerViewController,
                        didPickDocumentsAt urls: [URL]) {
        guard let src = urls.first else { return }

        // The picked URL is only readable while the scope is held, and the
        // import reads it on another thread — so the scope is opened here and
        // closed when the work finishes, not when this function returns.
        let scoped = src.startAccessingSecurityScopedResource()

        output.text = "Looking at \(src.lastPathComponent)…\n"
        setBusy(true)

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let look = src.path.withCString { win_probe_look($0) }
            DispatchQueue.main.async {
                guard let self else {
                    if scoped { src.stopAccessingSecurityScopedResource() }
                    return
                }
                self.setBusy(false)
                self.offerModes(for: src, look: look, scoped: scoped)
            }
        }
    }

    /// What it looks like, and the choice. The recommended mode is first and
    /// says why; the other is offered anyway, because a detector that is
    /// certain is a detector that cannot be corrected.
    private func offerModes(for src: URL, look: wi_probe_result, scoped: Bool) {
        let family = String(cString: look.setup.name)
        let note = String(cString: look.setup.note)
        let kindWord: String
        switch look.src {
        case WI_SRC_FOLDER: kindWord = "a folder"
        case WI_SRC_ZIP:    kindWord = "an archive"
        case WI_SRC_EXE:    kindWord = "a Windows program"
        default:            kindWord = "something we cannot read"
        }
        let bits = look.is32 == 1 ? "32-bit" : look.is32 == 0 ? "64-bit" : "unknown bitness"

        var text = "\(src.lastPathComponent)\n\n"
        text += "This is \(kindWord) (\(bits)).\n"
        text += "Identified as: \(family)\n\(note)\n"
        output.text = text

        if look.src == WI_SRC_UNKNOWN {
            output.text += "\nThere is nothing here that can be imported.\n"
            if scoped { src.stopAccessingSecurityScopedResource() }
            return
        }

        let installerFirst = look.looks_like_installer
        let a = UIAlertController(
            title: installerFirst ? "This looks like an installer" : "This looks like a game",
            message: installerFirst
                ? "Running it will install the program onto this app's C: drive, "
                + "using \(family)'s silent mode — its window cannot be drawn here, "
                + "so the flags that skip it are what make this possible at all."
                : "It will be copied onto this app's C: drive as it is.",
            preferredStyle: .alert)

        let game = UIAlertAction(title: "Import as a game", style: .default) { _ in
            self.run(src: src, installer: false, scoped: scoped)
        }
        let install = UIAlertAction(title: "Run it as an installer", style: .default) { _ in
            self.run(src: src, installer: true, scoped: scoped)
        }
        // Order matters: the first action is the one a hurried tap lands on.
        if installerFirst { a.addAction(install); a.addAction(game) }
        else { a.addAction(game); a.addAction(install) }
        a.addAction(UIAlertAction(title: "Cancel", style: .cancel) { _ in
            if scoped { src.stopAccessingSecurityScopedResource() }
        })
        present(a, animated: true)
    }

    // MARK: - importing

    private func run(src: URL, installer: Bool, scoped: Bool) {
        guard let drive = ExeBrowser.driveC else {
            output.text += "\nThere is no C: drive to import into.\n"
            if scoped { src.stopAccessingSecurityScopedResource() }
            return
        }
        setBusy(true)
        output.text += installer
            ? "\nInstalling. This runs the setup program, so it may take a while.\n"
            : "\nCopying.\n"

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            // The C side owns the whole import, including running a guest for
            // an installer, so there is one place where the order of
            // operations lives rather than two that can disagree.
            var r = wi_result()
            let rc = src.path.withCString { s in
                drive.path.withCString { d in
                    // strict rather than keep-going: an install that stopped
                    // is better than one that half-happened, and the run
                    // report still names what it wanted.
                    win_probe_import(s, d, installer ? 1 : 0, 0, 300, &r)
                }
            }
            let detail = Self.text(r.detail)
            let name = Self.text(r.name)
            let exeRel = Self.text(r.exe_rel)
            let dllDir = Self.text(r.dll_dir)
            let ok = rc == 0 && r.ok != 0

            DispatchQueue.main.async {
                if scoped { src.stopAccessingSecurityScopedResource() }
                guard let self else { return }
                self.setBusy(false)
                self.output.text = detail
                guard ok, !name.isEmpty else {
                    self.status.text = "That did not work — the report says how far it got."
                    return
                }
                // What it needs, before anything is added: the number is the
                // honest first fact about an imported program and it costs
                // milliseconds.
                var buf = [CChar](repeating: 0, count: 262_144)
                var resolved = -1, missing = -1
                if let exe = ProgramStore.exeURL(name: name, exeRelative: exeRel) {
                    _ = exe.path.withCString { win_probe_imports($0, &buf, buf.count) }
                    let report = String(cString: buf)
                    resolved = Self.number(before: " imports resolved", in: report)
                    missing = Self.number(after: "imports resolved, ", in: report)
                    self.output.text += "\n---- what it needs ----\n" + report
                }
                self.pending = Program(name: name, exeRelative: exeRel,
                                       is32: r.is32 == 1,
                                       importsResolved: resolved, importsMissing: missing,
                                       lastExit: nil, lastRunMs: nil,
                                       dllDir: dllDir.isEmpty ? nil : dllDir,
                                       installed: installer)
                self.addButton.isEnabled = true
                self.status.text = missing == 0
                    ? "Nothing is missing: this should run."
                    : missing > 0 ? "\(missing) function\(missing == 1 ? "" : "s") it needs are not implemented here yet."
                                  : "Imported. Tap Add to library to keep it."
            }
        }
    }

    @objc private func addToLibrary() {
        guard let p = pending else { return }
        ProgramStore.update(p)
        navigationController?.popViewController(animated: true)
    }

    private func setBusy(_ b: Bool) {
        busy = b
        chooseButton.isEnabled = !b
        if b { spinner.startAnimating() } else { spinner.stopAnimating() }
    }

    // MARK: - reading C strings out of the result

    /// A fixed-size C char array arrives in Swift as a tuple, so it has to be
    /// read through its bytes rather than indexed.
    private static func text<T>(_ field: T) -> String {
        withUnsafeBytes(of: field) { raw in
            guard let base = raw.baseAddress else { return "" }
            return String(cString: base.assumingMemoryBound(to: CChar.self))
        }
    }

    private static func number(before marker: String, in text: String) -> Int {
        guard let r = text.range(of: marker) else { return -1 }
        let digits = text[..<r.lowerBound].reversed().prefix { $0.isNumber }.reversed()
        return Int(String(digits)) ?? -1
    }
    private static func number(after marker: String, in text: String) -> Int {
        guard let r = text.range(of: marker) else { return -1 }
        let digits = text[r.upperBound...].prefix { $0.isNumber }
        return Int(String(digits)) ?? -1
    }
}
