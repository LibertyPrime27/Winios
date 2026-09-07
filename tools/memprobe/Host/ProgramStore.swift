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
    /// Where this program's own DLLs are, as a host path. A game finds them
    /// beside its executable, which for something like an Unreal title is
    /// three folders below the one the user sees -- so it cannot be worked out
    /// from `name` and has to be carried. Optional so that an entry written
    /// before the importer existed still decodes.
    var dllDir: String?
    /// True when this entry is what an installer produced rather than
    /// something copied in ready to run. Worth keeping: if it turns out to be
    /// broken, the question "did the install finish?" is the first one, and it
    /// cannot be asked later without knowing there was an install.
    var installed: Bool?
    /// Where the program's folder actually is, relative to the drive. For a
    /// copied game that is the same as `name` and this stays nil; for an
    /// installed one it need not be, because an installer the person answered
    /// themselves goes wherever they told it to -- "Program Files/Some Game"
    /// as often as not. Without this the library would look for a top-level
    /// folder called "Some Game" and find nothing.
    var dirRelative: String?

    /// The folder to look in, whichever way this entry got here.
    var folder: String { dirRelative?.isEmpty == false ? dirRelative! : name }

    var canProbablyRun: Bool { importsMissing == 0 }
    var subtitle: String {
        var s = is32 ? "32-bit" : "64-bit"
        s += importsMissing == 0 ? " · nothing missing"
                                 : " · \(importsMissing) missing of \(importsResolved + importsMissing)"
        if installed == true { s += " · installed here" }
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
        var claimed = Set<String>()

        // Remembered entries first, and located by their folder rather than
        // by their name. An installed program can be several levels down --
        // scanning only the top of the drive would lose it, and losing it
        // looks exactly like the install having failed.
        for p in cached.values {
            let dir = c.appendingPathComponent(p.folder)
            guard fm.fileExists(atPath: dir.path) else { continue }
            out.append(p)
            // So the scan below does not offer the same thing twice under a
            // different name.
            claimed.insert(p.folder.split(separator: "/").first.map(String.init) ?? p.folder)
        }

        for item in onDisk {
            let name = item.lastPathComponent
            if name == "library.json" { continue }
            if claimed.contains(name) { continue }
            // The skeleton the drive is set up with. These are where programs
            // are put, not programs, and listing them as entries would put
            // "Windows" in somebody's game library.
            if Self.systemFolders.contains(name.lowercased()) { continue }
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
                               lastExit: nil, lastRunMs: nil, dllDir: nil, installed: nil,
                               dirRelative: nil))
        }
        return out.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// The drive skeleton, which is scaffolding rather than content.
    private static let systemFolders: Set<String> = [
        "program files", "program files (x86)", "programdata", "users",
        "windows", "temp", "$recycle.bin",
    ]

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
        // The folder, not the name: they are the same for a copied game and
        // are not for an installed one, and removing by name would delete
        // nothing while the entry disappeared.
        try? FileManager.default.removeItem(at: c.appendingPathComponent(p.folder))
        save(load().filter { $0.name != p.name })
    }

    /// The executable to hand to winrun.
    static func exeURL(_ p: Program) -> URL? { exeURL(folder: p.folder, exeRelative: p.exeRelative) }

    /// The same, before there is a Program: the importer has the two strings
    /// and needs the path in order to ask what the program requires, which it
    /// does before deciding whether to offer the entry at all.
    static func exeURL(folder: String, exeRelative: String) -> URL? {
        guard let c = driveC, !folder.isEmpty else { return nil }
        let base = c.appendingPathComponent(folder)
        return exeRelative.isEmpty ? base : base.appendingPathComponent(exeRelative)
    }

    /// Where to look for the program's own DLLs. The recorded directory when
    /// the importer worked one out, and otherwise the executable's own folder,
    /// which is right for everything that is not deeply nested.
    static func dllDirURL(_ p: Program) -> URL? {
        if let d = p.dllDir, !d.isEmpty { return URL(fileURLWithPath: d) }
        return exeURL(p)?.deletingLastPathComponent()
    }
}
