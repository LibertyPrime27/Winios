import UIKit

/// A record of what ran and what happened, kept only if you ask for it.
///
/// Off by default and switched on in Settings, because it writes what you ran
/// to a file on the device, and that should be a decision rather than a
/// surprise. Nothing leaves the device on its own — the log is read, copied
/// or shared by hand.
///
/// Every run is recorded, not only the ones that fail. "It started and the
/// screen stayed black" exits zero and is exactly the entry worth having,
/// and a log that keeps only failures cannot be compared against a run that
/// worked.
///
/// What makes it worth having is the header. A run report on its own says a
/// program faulted at some address; the same report beside the device, the
/// iOS version, the page size, whether the JIT was on and what resolution
/// the game was told about is usually enough to know *why*, without another
/// round trip to ask. The page size in particular has already been the whole
/// answer once: a test that had never actually run on Apple silicon because
/// 4 KB arithmetic is invalid on a 16 KB page.
enum Logs {

    /// Off until asked for.
    static var isEnabled: Bool {
        get { UserDefaults.standard.object(forKey: "recordLogs") as? Bool ?? false }
        set { UserDefaults.standard.set(newValue, forKey: "recordLogs") }
    }

    private static var url: URL? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)
            .first?.appendingPathComponent("Winios-log.txt")
    }

    /// Everything about the machine that has ever mattered when reading one
    /// of these. Recorded per entry rather than once at the top of the file:
    /// the JIT state and the display setting change between runs, and an
    /// entry that has to be cross-referenced with an earlier one is an entry
    /// that gets misread.
    private static func header(_ program: String, exit code: Int32, ms: UInt64) -> String {
        let f = ISO8601DateFormatter()
        var s = "\n==== \(f.string(from: Date())) ====\n"
        s += "program     \(program)\n"
        s += "exit        \(code)\(reason(code))\n"
        s += "ran for     \(ms) ms\n"
        s += "app build   \(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?")"
        s += " (\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"))\n"
        s += "device      \(DeviceInfo.modelIdentifier)\n"
        s += "system      \(UIDevice.current.systemName) \(UIDevice.current.systemVersion)\n"
        s += "page size   \(getpagesize()) bytes\n"
        s += "memory      \(ProcessInfo.processInfo.physicalMemory >> 20) MB physical\n"
        s += "cores       \(ProcessInfo.processInfo.processorCount)"
        s += " (\(ProcessInfo.processInfo.activeProcessorCount) active)\n"
        s += "thermal     \(thermal())\n"
        s += "low power   \(ProcessInfo.processInfo.isLowPowerModeEnabled ? "on" : "off")\n"
        s += "jit         \(Settings.jitReady ? "enabled" : "DISABLED (interpreted)")\n"
        let m = Settings.displayMode
        s += "told screen \(m.w)x\(m.h)\n"
        s += "free disk   \(freeMB()) MB\n"
        return s
    }

    /// The exit codes winrun uses, spelled out. A bare number in a log is a
    /// number somebody has to go and look up.
    private static func reason(_ code: Int32) -> String {
        switch code {
        case 0:   return " (clean)"
        case 2:   return " (could not start: see the report)"
        case 124: return " (stopped by the time limit or Stop)"
        case 125: return " (faulted)"
        case 127: return " (called something that cannot be returned from)"
        case 129: return " (unhandled guest exception)"
        default:  return ""
        }
    }

    private static func thermal() -> String {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal:  return "nominal"
        case .fair:     return "fair"
        case .serious:  return "serious (the CPU is being throttled)"
        case .critical: return "critical (heavily throttled)"
        @unknown default: return "unknown"
        }
    }

    private static func freeMB() -> Int {
        guard let u = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first,
              let v = try? u.resourceValues(forKeys: [.volumeAvailableCapacityKey]),
              let bytes = v.volumeAvailableCapacity
        else { return -1 }
        return bytes >> 20
    }

    /// Record a run, however it ended.
    static func record(program: String, exit code: Int32, ms: UInt64, report: String) {
        guard isEnabled else { return }
        guard let u = url else { return }
        let entry = header(program, exit: code, ms: ms) + "\n" + report + "\n"
        guard let data = entry.data(using: .utf8) else { return }
        if let h = try? FileHandle(forWritingTo: u) {
            // Appending rather than rewriting, and trimmed from the front when
            // it gets long: a log that grows without limit on a phone is a
            // disk-space bug waiting to happen.
            h.seekToEndOfFile()
            h.write(data)
            try? h.close()
            trimIfHuge()
        } else {
            try? data.write(to: u)
        }
    }

    private static let cap = 512 * 1024

    private static func trimIfHuge() {
        guard let u = url,
              let size = try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              size > cap,
              var text = try? String(contentsOf: u, encoding: .utf8)
        else { return }
        // Keep the newest half, cut at an entry boundary so the file never
        // starts halfway through a report.
        let keep = text.index(text.endIndex, offsetBy: -min(text.count, cap / 2))
        text = String(text[keep...])
        if let cut = text.range(of: "\n==== ") { text = String(text[cut.lowerBound...]) }
        try? ("(earlier entries trimmed)\n" + text).write(to: u, atomically: true, encoding: .utf8)
    }

    static func text() -> String {
        guard let u = url, let s = try? String(contentsOf: u, encoding: .utf8), !s.isEmpty
        else { return "" }
        return s
    }

    static func clear() {
        if let u = url { try? FileManager.default.removeItem(at: u) }
    }
}
