import UIKit

/// The app's front door: the Windows programs on the device.
///
/// A list of programs and a Run button. The probes that used to be the whole
/// app are not offered any more — they were development tools, and the one
/// part of them a person actually needs (turning the JIT on) lives in
/// Settings now, with a status line at the top of this screen so nobody has
/// to go looking for it.
///
/// Every row says what we know before you tap it: bitness, and how many of its
/// imports nothing here can satisfy. For anything real that second number will
/// be large, and saying so up front is more use than letting someone tap Run
/// and watch it stop.
final class LibraryViewController: UITableViewController {

    private var programs: [Program] = []

    /// The JIT state, at the top of the app's front door.
    ///
    /// It is here rather than tucked away somewhere because it is the
    /// single biggest thing that decides whether a game is playable, and
    /// because on iOS it is not a property of the build: the dynarec needs an
    /// executable region a debugger has authorised, once per launch. Without
    /// it everything silently runs interpreted, and "silently" is the problem
    /// -- a game just feels broken.
    private let jitBanner = UILabel()
    private let jitBannerHost = UIView()

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Winios"
        // C:\ for everything the library runs.
        if let c = ExeBrowser.driveC { c.path.withCString { w32_set_drive_c($0) } }

        navigationItem.rightBarButtonItems = [
            UIBarButtonItem(barButtonSystemItem: .add, target: self, action: #selector(addProgram)),
            UIBarButtonItem(title: "Settings", style: .plain, target: self, action: #selector(showSettings)),
        ]
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "p")
        tableView.rowHeight = 62

        // The display setting has to be in the emulator before anything runs.
        Settings.apply()
        // And the JIT, if this launch can have one. People launch through
        // StikDebug with LiveContainer, where JIT is already enabled before
        // the app's own code runs -- the arena only has to be created and
        // handed over, which nothing was doing.
        JIT.ensure()

        jitBanner.font = .preferredFont(forTextStyle: .subheadline)
        jitBanner.numberOfLines = 0
        jitBanner.translatesAutoresizingMaskIntoConstraints = false
        jitBannerHost.addSubview(jitBanner)
        NSLayoutConstraint.activate([
            jitBanner.leadingAnchor.constraint(equalTo: jitBannerHost.leadingAnchor, constant: 16),
            jitBanner.trailingAnchor.constraint(equalTo: jitBannerHost.trailingAnchor, constant: -16),
            jitBanner.centerYAnchor.constraint(equalTo: jitBannerHost.centerYAnchor),
        ])
        jitBannerHost.addGestureRecognizer(
            UITapGestureRecognizer(target: self, action: #selector(jitTapped)))
        // A frame, not autolayout: a table header view is positioned by the
        // table from its frame, and a header sized only by constraints ends
        // up zero-height.
        jitBannerHost.frame = CGRect(x: 0, y: 0, width: tableView.bounds.width, height: 56)
        tableView.tableHeaderView = jitBannerHost
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reload()
        refreshJITBanner()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        // Keep the header the table's width through rotation.
        if jitBannerHost.frame.width != tableView.bounds.width {
            jitBannerHost.frame = CGRect(x: 0, y: 0, width: tableView.bounds.width, height: 56)
            tableView.tableHeaderView = jitBannerHost
        }
    }

    private func refreshJITBanner() {
        // Try again on every appearance: a launch that came up without the
        // JIT can gain one if StikDebug attaches while the app is running.
        JIT.ensure()
        let on = Settings.jitReady
        jitBanner.text = on
            ? "JIT enabled — guest code is compiled to ARM64"
            : "JIT disabled — running interpreted. Tap for why."
        jitBanner.textColor = on ? .systemGreen : .systemRed
        jitBannerHost.backgroundColor = (on ? UIColor.systemGreen : UIColor.systemRed)
            .withAlphaComponent(0.12)
    }

    /// Tapping it goes where it is turned on. Pointless when it is already
    /// on, so then it goes to Settings, which is what someone tapping a
    /// status line is probably looking for.
    @objc private func jitTapped() {
        if Settings.jitReady { showSettings() } else { showProbes() }
    }

    @objc private func showSettings() {
        navigationController?.pushViewController(SettingsViewController(), animated: true)
    }

    private func reload() {
        programs = ProgramStore.load()
        tableView.reloadData()
    }

    /// Where the JIT is turned on. The probe screen it reaches is in
    /// JIT-only mode: the rest of it was a development tool and is no longer
    /// offered anywhere in the app.
    @objc private func showProbes() {
        navigationController?.pushViewController(ProbeViewController(onlyJIT: true), animated: true)
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
