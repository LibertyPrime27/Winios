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

    /// Add a program. Two shapes arrive from outside -- a game that is already
    /// installed, and an installer whose output is the game -- and they need
    /// opposite treatment, so the choosing and the explaining happen on their
    /// own screen rather than behind this button.
    @objc private func addProgram() {
        navigationController?.pushViewController(ImportViewController(), animated: true)
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
            c.secondaryText = "Tap + to add one: a folder or .zip for a game you already "
                            + "have, or a setup .exe to install one. A folder is the usual "
                            + "case -- a program is rarely a single file."
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
