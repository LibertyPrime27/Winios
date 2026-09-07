import UIKit

/// What happened, written by the app itself.
///
/// This exists because the system's reports named the wrong process. Four
/// .ips files came off a device and between them said almost nothing about
/// this app: two `cpu_resource_fatal` kills — 48 seconds of CPU in 51,
/// against iOS's 80%-over-60-seconds limit — whose heaviest stack was
/// main-thread SwiftUI and dispatch churn belonging to the launcher; a
/// SIGSEGV inside StikDebug's own log manager, which is a `Published` array
/// being mutated from a dispatch block and is that app's bug, not ours,
/// though our log volume is what makes it fire; and a SIGABRT in SwiftUI's
/// RenderBox failing to load its Metal library, which is what running under
/// LiveContainer looks like. Running as a guest inside somebody else's
/// process means the report a person eventually finds is usually about the
/// host. So the app records its own, in its own container, immediately.
///
/// Three things are kept, and they answer different questions:
///
/// 1. **The breadcrumb**, written whenever a guest starts and deleted when it
///    finishes. It names the executable, the bitness, whether the JIT was
///    blessed, the display size and the device. Finding one at the next
///    launch means the process did not survive that run. This is the only
///    part that is *not* optional, because it is the only part that helps
///    with the failure that actually happened: a CPU-watchdog kill delivers
///    no signal at all, runs no handler, and leaves nothing behind but this.
/// 2. **The signal report**, when crash recording is on: the signal, the
///    fault address and a backtrace, written by the C handler in
///    `Shared/crashcatch.c` into a descriptor opened ahead of time.
/// 3. **The system's own .ips files**, where they can be reached. On a
///    sideloaded app they usually cannot, and that has to be quiet rather
///    than an error.
///
/// Nothing leaves the device on its own; the report is read, copied or
/// cleared by hand, the same as the log.
enum CrashReports {

    /// The same directory the JIT crash marker uses, and for the same reason:
    /// under LiveContainer the app-group entitlement may not resolve, and
    /// `ResultStore.containerDir` already falls back to the app's own Library
    /// folder rather than to nowhere.
    private static var dir: URL? { ResultStore.containerDir }
    /// Held open by the C handler for this launch.
    private static var liveURL: URL? { dir?.appendingPathComponent("crash-live.txt") }
    /// What the previous launch left behind, composed at startup.
    private static var previousURL: URL? { dir?.appendingPathComponent("crash-previous.txt") }
    private static var breadcrumbURL: URL? { dir?.appendingPathComponent("guest-breadcrumb.txt") }

    // MARK: - launch

    /// Call once, as early in the launch as there is anything to call it
    /// from. Moves whatever the previous run left behind somewhere it will
    /// not be overwritten, then arms the handlers if they are wanted.
    ///
    /// The order matters: arming truncates the file the handler writes into,
    /// so reading the previous report has to happen first.
    static func prepare() {
        rotate()
        arm()
        setContext("no guest running")
    }

    /// Arm the handlers now. Called at launch and again when the switch in
    /// Settings is turned on, so somebody who turns it on does not have to
    /// relaunch before it means anything.
    static func arm() {
        guard Settings.recordCrashes, let u = liveURL else { return }
        guard u.path.withCString({ crashcatch_install($0) }) == 1 else { return }
        installExceptionHandler()
    }

    private static var exceptionHandlerInstalled = false

    /// An Objective-C exception that nobody caught reaches the process as a
    /// SIGABRT *after* the runtime has already unwound and lost the reason,
    /// so the signal handler alone would record an abort with no explanation.
    /// This runs first and writes the name and reason while they still exist.
    private static func installExceptionHandler() {
        guard !exceptionHandlerInstalled else { return }
        exceptionHandlerInstalled = true
        // The closure captures nothing on purpose: this is a C function
        // pointer, and a capturing closure cannot be one.
        NSSetUncaughtExceptionHandler { exception in
            let name = exception.name.rawValue
            let reason = exception.reason ?? "(no reason given)"
            let stack = exception.callStackSymbols.joined(separator: "\n")
            name.withCString { n in
                reason.withCString { r in
                    stack.withCString { s in
                        crashcatch_write_exception(n, r, s)
                    }
                }
            }
        }
    }

    /// Fold the live report and any surviving breadcrumb into one record of
    /// the previous launch, then clear both.
    private static func rotate() {
        let fm = FileManager.default
        let crash = liveURL.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? ""
        let crumb = breadcrumbURL.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? ""
        if let u = liveURL { try? fm.removeItem(at: u) }
        if let u = breadcrumbURL { try? fm.removeItem(at: u) }
        // A launch that ended cleanly leaves an empty report file and no
        // breadcrumb, and there is nothing to say about it.
        guard !crash.isEmpty || !crumb.isEmpty else { return }

        var s = "==== the previous run ended badly ====\n"
        s += "found at    \(ISO8601DateFormatter().string(from: Date()))\n"
        if crumb.isEmpty {
            s += "guest       none was running\n"
        } else {
            s += "\nwhat was running\n" + crumb
            if !crumb.hasSuffix("\n") { s += "\n" }
        }
        s += "\n"
        if crash.isEmpty {
            // The interesting case, and the one the .ips reports were about:
            // the process was killed rather than faulting. iOS kills an app
            // that averages more than 80% CPU over a minute, and a jetsam or
            // watchdog kill delivers no signal, so no handler runs and there
            // is nothing to print but this.
            s += Settings.recordCrashes
                ? "No signal was recorded, so nothing faulted: the process was "
                  + "killed from outside. That is what a jetsam kill or the CPU "
                  + "watchdog looks like — iOS terminates an app that averages "
                  + "more than 80% CPU over 60 seconds, and it does it without "
                  + "delivering a signal, so no handler can report it.\n"
                : "Crash recording was off during that run, so there is no "
                  + "backtrace. The breadcrumb above is what survived. Turn "
                  + "crash reports on in Settings to get more than this.\n"
        } else {
            s += crash
        }
        if let u = previousURL { writeDurably(s, to: u) }
    }

    // MARK: - breadcrumbs

    /// Record that a guest is starting. Written before the run, flushed, and
    /// deleted when the run comes back — so a breadcrumb that is still there
    /// at the next launch is a run that did not.
    ///
    /// Not conditional on the setting. It is one small write per game launch,
    /// and without it a report says only that a process died; with it, it
    /// says which game, in which bitness, with or without the dynarec, at
    /// which resolution. That is most of what a bug report needs.
    static func guestStarting(program: String, is32: Bool?) {
        let m = Settings.displayMode
        var bits = "not known on this path"
        if let is32 { bits = is32 ? "32-bit (PE32)" : "64-bit (PE32+)" }
        var s = "started     \(ISO8601DateFormatter().string(from: Date()))\n"
        s += "program     \(program)\n"
        s += "bitness     \(bits)\n"
        s += "jit         \(Settings.jitReady ? "blessed, guest code is compiled" : "DISABLED (interpreted)")\n"
        s += "told screen \(m.w)x\(m.h), dialogs at \(Settings.dialogScale)%\n"
        s += "device      \(DeviceInfo.modelIdentifier)"
        s += " — \(UIDevice.current.systemName) \(UIDevice.current.systemVersion)\n"
        s += "app build   \(build)\n"
        if let u = breadcrumbURL { writeDurably(s, to: u) }
        // The handler cannot read a file, so it gets its own copy.
        setContext(s)
    }

    /// The run came back, however it ended. Anything that happens after this
    /// is not that guest's fault.
    static func guestFinished() {
        if let u = breadcrumbURL { try? FileManager.default.removeItem(at: u) }
        setContext("no guest running")
    }

    private static func setContext(_ s: String) {
        s.withCString { crashcatch_set_context($0) }
    }

    private static var build: String {
        let v = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
        let short = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        return "\(v) (\(short))"
    }

    // MARK: - reading it back

    /// True when the previous launch left something worth showing.
    static var hasPrevious: Bool { !previousText().isEmpty }

    private static func previousText() -> String {
        guard let u = previousURL, let s = try? String(contentsOf: u, encoding: .utf8) else { return "" }
        return s
    }

    /// One line for the Settings screen, which is where somebody looks first.
    static var summary: String {
        var s = hasPrevious
            ? "The previous run ended in a crash or a kill."
            : "Nothing from the previous run — it ended normally."
        let n = systemReportFiles().count
        if n > 0 { s += " \(n) system report\(n == 1 ? "" : "s") found on this device." }
        // The one way the switch can be lying: it is on, and the file the
        // handler writes into could not be opened, so nothing is armed and a
        // crash would still leave only the breadcrumb. Better said here than
        // discovered after the crash it was turned on for.
        if Settings.recordCrashes && crashcatch_armed() == 0 {
            s += " Recording is on but nothing is armed — the report file could not be opened."
        }
        return s
    }

    /// Everything there is, in one piece of text: what this app recorded,
    /// then whatever the system left where we can reach it.
    static func report() -> String {
        var s = previousText()
        if s.isEmpty {
            s = "Nothing recorded. The previous run of this app ended normally, "
              + "or ended before anything had been written down.\n"
            if !Settings.recordCrashes {
                s += "\nCrash reports are off. Turn them on above and they will be "
                   + "armed from the next launch — and from now, in this one.\n"
            }
        }
        s += "\n==== the system's own reports ====\n"
        let files = systemReportFiles()
        if files.isEmpty {
            s += "None readable from here. That is the normal case for a "
               + "sideloaded app: the folders either do not exist in this "
               + "container or are not ours to read. They are on the device "
               + "under Settings > Privacy & Security > Analytics & "
               + "Improvements > Analytics Data, newest last.\n"
            s += "\nBe careful reading one of those: launched through "
               + "LiveContainer and StikDebug, the process that dies is often "
               + "not this app, and the report will be named after whichever "
               + "one it was.\n"
            return s
        }
        for f in files.prefix(4) {
            s += "\n---- \(f.lastPathComponent) ----\n"
            s += contents(of: f, limit: 16 * 1024)
            if !s.hasSuffix("\n") { s += "\n" }
        }
        if files.count > 4 { s += "\n(\(files.count - 4) older reports not shown)\n" }
        return s
    }

    /// The system's .ips files, newest first, from the two places they turn
    /// up. Both are usually absent or unreadable on a sideloaded app, so
    /// every step here is allowed to fail and the answer is then simply an
    /// empty list — an error at this point would be reporting on the
    /// reporting.
    static func systemReportFiles() -> [URL] {
        let fm = FileManager.default
        var dirs: [URL] = [
            URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
                .appendingPathComponent("Library/Logs/CrashReporter"),
        ]
        if let lib = fm.urls(for: .libraryDirectory, in: .userDomainMask).first {
            dirs.append(lib.appendingPathComponent("Logs"))
            dirs.append(lib.appendingPathComponent("Logs/CrashReporter"))
        }
        var seenDir = Set<String>()
        var seenFile = Set<String>()
        var out: [URL] = []
        for d in dirs {
            // The two lists overlap for an ordinary sandboxed app and do not
            // under LiveContainer, where the home directory and the container
            // are different places.
            guard seenDir.insert(d.standardizedFileURL.path).inserted else { continue }
            let items = (try? fm.contentsOfDirectory(at: d,
                                                     includingPropertiesForKeys: [.contentModificationDateKey],
                                                     options: [.skipsHiddenFiles])) ?? []
            for f in items where f.pathExtension.lowercased() == "ips" {
                if seenFile.insert(f.standardizedFileURL.path).inserted { out.append(f) }
            }
        }
        return out.sorted { modified($0) > modified($1) }
    }

    private static func modified(_ u: URL) -> Date {
        (try? u.resourceValues(forKeys: [.contentModificationDateKey]))?
            .contentModificationDate ?? .distantPast
    }

    /// Capped, because an .ips is JSON and a long one would push everything
    /// above it off the top of a text view.
    private static func contents(of u: URL, limit: Int) -> String {
        guard let d = try? Data(contentsOf: u) else { return "(could not be read)\n" }
        if d.count <= limit { return String(decoding: d, as: UTF8.self) }
        return String(decoding: d.prefix(limit), as: UTF8.self)
             + "\n(truncated at \(limit / 1024) KB of \(d.count / 1024) KB)\n"
    }

    /// Forget the previous run's report. The system's own files are not ours
    /// to delete and are left alone.
    static func clear() {
        if let u = previousURL { try? FileManager.default.removeItem(at: u) }
    }

    // MARK: - writing

    /// Write and fsync, rather than write and hope. A buffered write is
    /// precisely what is lost when the process is killed, which is the case
    /// every one of these files exists for — `ResultStore.append` does the
    /// same thing for the ladder for the same reason.
    private static func writeDurably(_ text: String, to url: URL) {
        guard let data = text.data(using: .utf8) else { return }
        _ = FileManager.default.createFile(atPath: url.path, contents: nil)
        guard let h = try? FileHandle(forWritingTo: url) else {
            try? data.write(to: url, options: .atomic)
            return
        }
        try? h.write(contentsOf: data)
        fsync(h.fileDescriptor)
        try? h.close()
    }
}
