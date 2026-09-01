import Foundation

/// Durable, append-only record of the ladder, shared between the host app and
/// the extension via an App Group container.
///
/// It has to be a file rather than in-memory state or an XPC reply, because the
/// interesting outcome is the process dying. Whatever reports the result must
/// outlive the thing being measured.
enum ResultStore {
    /// Must match the App Group in both .entitlements files.
    static let appGroup = "group.winios.memprobe"

    static var url: URL? {
        FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup)?
            .appendingPathComponent("ladder.jsonl")
    }

    static func append(_ rung: Rung) {
        guard let url,
              let line = try? JSONEncoder().encode(rung) else { return }
        var data = line
        data.append(0x0A)

        if let handle = try? FileHandle(forWritingTo: url) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
            // fsync, not just write: a buffered line is lost when jetsam kills
            // the process, which is precisely the case being measured.
            fsync(handle.fileDescriptor)
        } else {
            try? data.write(to: url, options: .atomic)
        }
    }

    static func readAll() -> [Rung] {
        guard let url, let blob = try? Data(contentsOf: url) else { return [] }
        let decoder = JSONDecoder()
        return blob.split(separator: 0x0A).compactMap { try? decoder.decode(Rung.self, from: Data($0)) }
    }

    static func reset() {
        guard let url else { return }
        try? Data().write(to: url)
    }

    /// Highest rung reached per host, which is the actual measurement.
    static func highWater() -> [String: Int] {
        readAll().reduce(into: [:]) { acc, r in
            acc[r.host] = max(acc[r.host] ?? 0, r.residentMB)
        }
    }
}
