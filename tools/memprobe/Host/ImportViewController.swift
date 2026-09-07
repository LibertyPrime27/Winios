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
    private var cancelButton: UIButton!

    /// The finished import, held until the user accepts it. Nothing goes into
    /// the library until they do — an import they did not want should not
    /// leave an entry behind.
    private var pending: Program?
    private var busy = false
    /// Polls the C side while an import runs. See the note in winprobe.c for
    /// why progress is pulled rather than pushed.
    private var ticker: Timer?
    /// A long import must survive the screen locking and a brief trip to the
    /// home screen. Without these, iOS suspends the app partway through a
    /// 580 MB extraction and the user comes back to a half-copy.
    private var bgTask: UIBackgroundTaskIdentifier = .invalid

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
        cancelButton = button("Stop", #selector(cancelImport))
        cancelButton.isEnabled = false

        let bar = UIStackView(arrangedSubviews: [spinner, UIView()])
        bar.axis = .horizontal
        bar.spacing = 8

        let stack = UIStackView(arrangedSubviews: [status, chooseButton, addButton,
                                                   cancelButton, bar, output])
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

    /// Append to the report. `UITextView.text` is an implicitly unwrapped
    /// optional, and `+=` on one of those relies on the compiler unwrapping
    /// for both the read and the write. It does -- but there is one of these
    /// and there were four of those, so the unwrap happens here and is
    /// explicit.
    private func say(_ more: String) {
        output.text = (output.text ?? "") + more
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
            var family = [CChar](repeating: 0, count: 128)
            var note = [CChar](repeating: 0, count: 512)
            var is32: Int32 = -1
            var isInstaller: Int32 = 0
            let kind = src.path.withCString {
                win_probe_look($0, &is32, &isInstaller, &family, family.count, &note, note.count)
            }
            let look = Look(kind: kind, is32: is32, isInstaller: isInstaller != 0,
                            family: String(cString: family), note: String(cString: note))
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

    /// What the C side said about the file. A Swift struct rather than the C
    /// one: the importer's structs carry fixed-size char buffers, and Swift
    /// imports those as tuples of that many elements.
    private struct Look {
        let kind: Int32
        let is32: Int32
        let isInstaller: Bool
        let family: String
        let note: String
    }

    /// What it looks like, and the choice. The recommended mode is first and
    /// says why; the other is offered anyway, because a detector that is
    /// certain is a detector that cannot be corrected.
    private func offerModes(for src: URL, look: Look, scoped: Bool) {
        let kindWord: String
        switch look.kind {
        case Int32(WIN_LOOK_FOLDER):  kindWord = "a folder"
        case Int32(WIN_LOOK_ARCHIVE): kindWord = "an archive"
        case Int32(WIN_LOOK_EXE):     kindWord = "a Windows program"
        default:                      kindWord = "something we cannot read"
        }
        let bits = look.is32 == 1 ? "32-bit" : look.is32 == 0 ? "64-bit" : "unknown bitness"

        var text = "\(src.lastPathComponent)\n\n"
        text += "This is \(kindWord) (\(bits)).\n"
        text += "Identified as: \(look.family)\n\(look.note)\n"
        output.text = text

        if look.kind == Int32(WIN_LOOK_UNKNOWN) {
            say("\nThere is nothing here that can be imported.\n")
            if scoped { src.stopAccessingSecurityScopedResource() }
            return
        }

        let installerFirst = look.isInstaller
        let family = look.family
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

    // MARK: - a file that is not really on the device yet

    /// A download picked out of Files may not be local. iCloud Drive evicts
    /// files, and a fresh Safari download can still be a placeholder — either
    /// way the URL names something that does not exist yet, and the importer
    /// would report "nothing here that can be imported" for a file the user
    /// can plainly see. So it gets fetched first, with the waiting visible.
    ///
    /// Returns false if it could not be made local, having already said why.
    private func materialise(_ url: URL, _ then: @escaping (Bool) -> Void) {
        let fm = FileManager.default
        if fm.fileExists(atPath: url.path) { then(true); return }

        let vals = try? url.resourceValues(forKeys: [.isUbiquitousItemKey])
        guard vals?.isUbiquitousItem == true else {
            say("\nThat file is not on this device and is not in iCloud either, "
                + "so there is nothing to read.\n")
            then(false)
            return
        }
        say("\nThat file is in iCloud and has not been downloaded yet. Fetching it.\n")
        do { try fm.startDownloadingUbiquitousItem(at: url) }
        catch {
            say("iCloud would not start the download: \(error.localizedDescription)\n")
            then(false)
            return
        }
        // Poll rather than watch with NSMetadataQuery: one file, a visible
        // wait, and a query object whose lifetime has to be managed is more
        // machinery than this needs.
        var waited = 0.0
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { t in
            waited += 0.5
            if fm.fileExists(atPath: url.path) { t.invalidate(); then(true); return }
            if waited > 600 {
                t.invalidate()
                self.say("Still not downloaded after ten minutes. Open it in the "
                         + "Files app once to bring it down, then try again.\n")
                then(false)
                return
            }
            self.status.text = "Downloading from iCloud… \(Int(waited))s"
        }
    }

    // MARK: - importing

    private func run(src: URL, installer: Bool, scoped: Bool) {
        guard let drive = ExeBrowser.driveC else {
            say("\nThere is no C: drive to import into.\n")
            if scoped { src.stopAccessingSecurityScopedResource() }
            return
        }
        setBusy(true)
        materialise(src) { [weak self] ok in
            guard let self else { if scoped { src.stopAccessingSecurityScopedResource() }; return }
            guard ok else {
                self.setBusy(false)
                if scoped { src.stopAccessingSecurityScopedResource() }
                return
            }
            self.reallyRun(src: src, installer: installer, scoped: scoped, drive: drive)
        }
    }

    private func reallyRun(src: URL, installer: Bool, scoped: Bool, drive: URL) {
        say(installer
            ? "\nInstalling. This runs the setup program, so it may take a while.\n"
            : "\nCopying.\n")
        holdOn()
        startTicking()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            // The C side owns the whole import, including running a guest for
            // an installer, so there is one place where the order of
            // operations lives rather than two that can disagree.
            var detailBuf = [CChar](repeating: 0, count: 64 * 1024)
            var nameBuf = [CChar](repeating: 0, count: 256)
            var exeBuf = [CChar](repeating: 0, count: 512)
            var dllBuf = [CChar](repeating: 0, count: 1200)
            var is32: Int32 = -1, files: Int32 = 0, exes: Int32 = 0
            func attempt(_ keepGoing: Int32) -> Int32 {
                src.path.withCString { s in
                    drive.path.withCString { d in
                        win_probe_import(s, d, installer ? 1 : 0, keepGoing, 300,
                                         &detailBuf, detailBuf.count,
                                         &nameBuf, nameBuf.count,
                                         &exeBuf, exeBuf.count,
                                         &dllBuf, dllBuf.count,
                                         &is32, &files, &exes)
                    }
                }
            }
            // Strict first: an install that stopped is better than one that
            // half-happened.
            let rc = attempt(0)
            var detail = String(cString: detailBuf)

            // If it stopped without installing anything, run it once more
            // carrying on past what is missing -- purely to collect the list.
            // The first run stops at the first unimplemented function, so it
            // names one; this one names everything it wanted, which is the
            // difference between a clue and a work queue. Worth doing
            // unprompted, because getting that list otherwise costs another
            // build and another install.
            //
            // The strict result stays the outcome. A keep-going run returns
            // zero from functions it does not have, and zero means failure
            // for most of the Win32 API and success for some -- so anything
            // it managed to write was written by an installer being lied to,
            // and registering that as a working program would be worse than
            // reporting the failure. Diagnostic only.
            if installer, rc != 0, detail.contains("Nothing new appeared") {
                _ = attempt(1)
                detail += "\n\n================ tried again, carrying on past "
                    + "anything missing ================\n"
                    + "This second run is for the list below and nothing else: it "
                    + "lets unimplemented functions return zero, so whatever it "
                    + "wrote was written by an installer that was being lied to. "
                    + "Nothing has been added to the library from it.\n\n"
                    + String(cString: detailBuf)
                // attempt() overwrote the name and exe buffers with the loose
                // run's findings; the strict run installed nothing, so they
                // must not be used.
                nameBuf[0] = 0; exeBuf[0] = 0; dllBuf[0] = 0
            }
            let name = String(cString: nameBuf)
            let exeRel = String(cString: exeBuf)
            let dllDir = String(cString: dllBuf)
            let ok = rc == 0

            DispatchQueue.main.async {
                if scoped { src.stopAccessingSecurityScopedResource() }
                guard let self else { return }
                self.stopTicking()
                self.letGo()
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
                    self.say("\n---- what it needs ----\n" + report)
                }
                let p = Program(name: name, exeRelative: exeRel,
                                is32: is32 == 1,
                                importsResolved: resolved, importsMissing: missing,
                                lastExit: nil, lastRunMs: nil,
                                dllDir: dllDir.isEmpty ? nil : dllDir,
                                installed: installer)
                // Saved now, not on a tap. The files are already on the drive
                // and the library lists the drive, so the program appears
                // either way -- withholding the entry would only mean it
                // appeared without its bitness, its DLL directory or its
                // import count, which is strictly worse. The button is a
                // shortcut into it rather than permission to keep it.
                ProgramStore.update(p)
                self.pending = p
                self.addButton.setTitle("Open \(name)", for: .normal)
                self.addButton.isEnabled = true
                self.status.text = missing == 0
                    ? "Nothing is missing: this should run."
                    : missing > 0 ? "\(missing) function\(missing == 1 ? "" : "s") it needs are not implemented here yet."
                                  : "Imported. Tap Add to library to keep it."
            }
        }
    }

    /// Straight into the program. It is already in the library by now, so
    /// this is a shortcut past going back and finding it.
    @objc private func addToLibrary() {
        guard let p = pending else { return }
        navigationController?.pushViewController(ProgramViewController(p), animated: true)
    }

    private func setBusy(_ b: Bool) {
        busy = b
        chooseButton.isEnabled = !b
        cancelButton.isEnabled = b
        if b { spinner.startAnimating() } else { spinner.stopAnimating() }
    }

    @objc private func cancelImport() {
        win_probe_cancel_import()
        status.text = "Stopping after the current file…"
    }

    // MARK: - a long import has to survive the screen locking

    /// Keeps the app awake and asks for a little grace if it is backgrounded.
    /// A 580 MB extraction takes minutes; the default behaviour is for iOS to
    /// suspend the app when the screen locks, which leaves a half-written
    /// folder on the drive and no explanation.
    private func holdOn() {
        UIApplication.shared.isIdleTimerDisabled = true
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "import") { [weak self] in
            // Time is nearly up: stop cleanly rather than being killed
            // partway through writing a file.
            win_probe_cancel_import()
            self?.letGo()
        }
    }
    private func letGo() {
        UIApplication.shared.isIdleTimerDisabled = false
        if bgTask != .invalid {
            UIApplication.shared.endBackgroundTask(bgTask)
            bgTask = .invalid
        }
    }

    /// Ask the C side where it has got to, a few times a second.
    private func startTicking() {
        ticker?.invalidate()
        ticker = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            var stage = [CChar](repeating: 0, count: 64)
            var done: UInt64 = 0, total: UInt64 = 0
            win_probe_progress(&stage, stage.count, &done, &total)
            let name = String(cString: stage)
            guard !name.isEmpty else { return }
            if total > 0 {
                let pct = Int(Double(done) / Double(total) * 100)
                self.status.text = "\(name) — \(done) of \(total) (\(pct)%)"
            } else if done > 0 {
                self.status.text = "\(name) — \(done) so far"
            } else {
                self.status.text = "\(name)…"
            }
        }
    }
    private func stopTicking() {
        ticker?.invalidate()
        ticker = nil
    }

    deinit { ticker?.invalidate() }

    // MARK: - reading the two counts back out of the import report

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
