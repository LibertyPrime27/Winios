import UIKit

/// The app's front door: the Windows programs on the device.
///
/// The probes that used to be the whole app are still here, one tap away under
/// Diagnostics, because they are how you tell "this program is broken" from
/// "this device is broken" — but they are no longer what the app is *about*.
/// What it is about is a list of programs and a Run button.
///
/// Every row says what we know before you tap it: bitness, and how many of its
/// imports nothing here can satisfy. For anything real that second number will
/// be large, and saying so up front is more use than letting someone tap Run
/// and watch it stop.
final class LibraryViewController: UITableViewController {

    private var programs: [Program] = []

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "winios"
        // C:\ for everything the library runs.
        if let c = ExeBrowser.driveC { c.path.withCString { w32_set_drive_c($0) } }

        navigationItem.rightBarButtonItem = UIBarButtonItem(
            barButtonSystemItem: .add, target: self, action: #selector(addProgram))
        navigationItem.leftBarButtonItem = UIBarButtonItem(
            title: "Diagnostics", style: .plain, target: self, action: #selector(showProbes))
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "p")
        tableView.rowHeight = 62
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reload()
    }

    private func reload() {
        programs = ProgramStore.load()
        tableView.reloadData()
    }

    @objc private func showProbes() {
        navigationController?.pushViewController(ProbeViewController(), animated: true)
    }

    /// Add a program: a folder for anything real, a single .exe for a test.
    /// The import report runs immediately, because "can this run at all" is
    /// the only useful first question and it costs a few milliseconds.
    @objc private func addProgram() {
        ExeBrowser.shared.pick(from: self) { [weak self] _, exe in
            guard let self else { return }
            defer { self.reload() }
            guard let exe, let c = ExeBrowser.driveC else { return }
            // The library entry is the top-level item on the drive, which is
            // the folder when one was picked and the file when it was not.
            let rel = exe.path.replacingOccurrences(of: c.path + "/", with: "")
            let parts = rel.split(separator: "/").map(String.init)
            let name = parts.first ?? exe.lastPathComponent
            let inner = parts.count > 1 ? parts.dropFirst().joined(separator: "/") : ""

            var buf = [CChar](repeating: 0, count: 65536)
            _ = exe.path.withCString { win_probe_imports($0, &buf, buf.count) }
            let report = String(cString: buf)
            let missing = Self.number(after: "imports resolved, ", in: report, trailing: " missing")
            let resolved = Self.number(before: " imports resolved", in: report)

            ProgramStore.update(Program(name: name, exeRelative: inner,
                                        is32: report.contains(": 32-bit"),
                                        importsResolved: resolved, importsMissing: missing,
                                        lastExit: nil, lastRunMs: nil))
        }
    }

    /// Pull the two counts out of the report rather than plumbing a second
    /// return value through the C bridge for something printed one line up.
    private static func number(before marker: String, in text: String) -> Int {
        guard let r = text.range(of: marker) else { return -1 }
        let head = text[..<r.lowerBound]
        let digits = head.reversed().prefix { $0.isNumber }.reversed()
        return Int(String(digits)) ?? -1
    }
    private static func number(after marker: String, in text: String, trailing: String) -> Int {
        guard let r = text.range(of: marker) else { return -1 }
        let tail = text[r.upperBound...]
        let digits = tail.prefix { $0.isNumber }
        return Int(String(digits)) ?? -1
    }

    // MARK: - table

    override func numberOfSections(in tableView: UITableView) -> Int { 1 }

    override func tableView(_ t: UITableView, numberOfRowsInSection s: Int) -> Int {
        max(programs.count, 1)
    }

    override func tableView(_ t: UITableView, cellForRowAt ip: IndexPath) -> UITableViewCell {
        let cell = t.dequeueReusableCell(withIdentifier: "p", for: ip)
        cell.accessoryType = .disclosureIndicator
        cell.detailTextLabel?.numberOfLines = 0
        if programs.isEmpty {
            var c = UIListContentConfiguration.subtitleCell()
            c.text = "No programs yet"
            c.secondaryText = "Tap + to add a Windows .exe, or a folder containing one. "
                            + "A folder is the usual case: a program is rarely one file."
            c.secondaryTextProperties.numberOfLines = 0
            cell.contentConfiguration = c
            cell.accessoryType = .none
            return cell
        }
        let p = programs[ip.row]
        var c = UIListContentConfiguration.subtitleCell()
        c.text = p.name
        c.secondaryText = p.importsMissing < 0 ? "not examined yet" : p.subtitle
        c.secondaryTextProperties.numberOfLines = 0
        cell.contentConfiguration = c
        return cell
    }

    override func tableView(_ t: UITableView, didSelectRowAt ip: IndexPath) {
        t.deselectRow(at: ip, animated: true)
        guard !programs.isEmpty else { addProgram(); return }
        navigationController?.pushViewController(ProgramViewController(programs[ip.row]), animated: true)
    }

    override func tableView(_ t: UITableView, canEditRowAt ip: IndexPath) -> Bool { !programs.isEmpty }

    override func tableView(_ t: UITableView, commit editingStyle: UITableViewCell.EditingStyle,
                            forRowAt ip: IndexPath) {
        guard editingStyle == .delete, !programs.isEmpty else { return }
        ProgramStore.remove(programs[ip.row])
        reload()
    }
}
