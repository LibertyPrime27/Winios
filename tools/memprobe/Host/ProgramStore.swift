import Foundation

/// The programs on the app's C: drive, and what we know about each.
///
/// The drive is the source of truth: a program is a folder (or a loose .exe)
/// under `Documents/c/`, and the library is whatever is there. Metadata —
/// which executable to run, what the last run did — is cached alongside, but a
/// missing cache is not an error, only a reason to look again. That way
/// copying a folder in by any means (Files, iTunes, a future import) makes it
/// appear, and deleting one makes it go away, without a database to keep in
/// step with the filesystem.
struct Program: Codable {
    var name: String                 // the folder or file name on the drive
    var exeRelative: String          // which executable inside it to run
    var is32: Bool
    var importsResolved: Int
    var importsMissing: Int
    var lastExit: Int32?
    var lastRunMs: UInt64?

    var canProbablyRun: Bool { importsMissing == 0 }
    var subtitle: String {
        var s = is32 ? "32-bit" : "64-bit"
        s += importsMissing == 0 ? " · nothing missing"
                                 : " · \(importsMissing) missing of \(importsResolved + importsMissing)"
        if let e = lastExit { s += " · last exit \(e)" }
        return s
    }
}

enum ProgramStore {

    static var driveC: URL? { ExeBrowser.driveC }

    private static var indexURL: URL? {
        driveC?.deletingLastPathComponent().appendingPathComponent("library.json")
    }

    /// Everything on the drive, with cached metadata where we have it. Entries
    /// whose files are gone are dropped; files with no entry get a placeholder
    /// so they are at least visible and can be examined.
    static func load() -> [Program] {
        guard let c = driveC else { return [] }
        let fm = FileManager.default
        let onDisk = (try? fm.contentsOfDirectory(at: c, includingPropertiesForKeys: nil)) ?? []
        var cached: [String: Program] = [:]
        if let u = indexURL, let d = try? Data(contentsOf: u),
           let list = try? JSONDecoder().decode([Program].self, from: d) {
            for p in list { cached[p.name] = p }
        }

        var out: [Program] = []
        for item in onDisk {
            let name = item.lastPathComponent
            if name == "library.json" { continue }
            if let p = cached[name] { out.append(p); continue }
            // Not seen before: find an executable inside and record what we can.
            var isDir: ObjCBool = false
            fm.fileExists(atPath: item.path, isDirectory: &isDir)
            var exe = name
            if isDir.boolValue {
                let inside = (try? fm.contentsOfDirectory(at: item, includingPropertiesForKeys: nil)) ?? []
                guard let first = inside.first(where: { $0.pathExtension.lowercased() == "exe" }) else { continue }
                exe = first.lastPathComponent
            } else if !name.lowercased().hasSuffix(".exe") {
                continue
            }
            out.append(Program(name: name, exeRelative: isDir.boolValue ? exe : "",
                               is32: true, importsResolved: 0, importsMissing: -1,
                               lastExit: nil, lastRunMs: nil))
        }
        return out.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    static func save(_ list: [Program]) {
        guard let u = indexURL, let d = try? JSONEncoder().encode(list) else { return }
        try? d.write(to: u)
    }

    static func update(_ p: Program) {
        var list = load().filter { $0.name != p.name }
        list.append(p)
        save(list)
    }

    static func remove(_ p: Program) {
        guard let c = driveC else { return }
        try? FileManager.default.removeItem(at: c.appendingPathComponent(p.name))
        save(load().filter { $0.name != p.name })
    }

    /// The executable to hand to winrun.
    static func exeURL(_ p: Program) -> URL? {
        guard let c = driveC else { return nil }
        let base = c.appendingPathComponent(p.name)
        return p.exeRelative.isEmpty ? base : base.appendingPathComponent(p.exeRelative)
    }
}
