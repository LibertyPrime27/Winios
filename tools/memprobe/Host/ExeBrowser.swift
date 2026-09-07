import UIKit
import UniformTypeIdentifiers

/// Run a Windows executable the user chose, rather than one we shipped.
///
/// This is the point at which the project stops being a test harness. A game
/// is an .exe plus a folder of DLLs and data, so a folder can be picked as
/// well as a single file, and everything is copied into the app's own storage
/// — a virtual C: — because a security-scoped URL from the Files app is not
/// something a PE loader can keep hold of.
///
/// This only copies and reports where things landed. What to do next — the
/// import report, or a run — is the caller's decision, so the picking is not
/// entangled with what the library happens to want today.
final class ExeBrowser: NSObject, UIDocumentPickerDelegate {

    static let shared = ExeBrowser()

    private weak var host: UIViewController?
    private var onReport: ((String, URL?) -> Void)?

    /// The app's virtual C: drive. Whatever the user picks is copied here, so
    /// a program can be run again later without picking it a second time.
    static var driveC: URL? {
        guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        else { return nil }
        let c = docs.appendingPathComponent("c")
        try? FileManager.default.createDirectory(at: c, withIntermediateDirectories: true)
        return c
    }

    func pick(from vc: UIViewController, then: @escaping (String, URL?) -> Void) {
        host = vc
        onReport = then
        // .item rather than a .exe type: iOS has no UTI for a PE binary, and a
        // folder is a legitimate pick too (a game is never one file).
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item, .folder],
                                                    asCopy: false)
        picker.delegate = self
        picker.allowsMultipleSelection = false
        vc.present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let src = urls.first, let dest = Self.driveC else { return }
        let fm = FileManager.default

        // A picked URL is only usable inside this scope, and only after asking.
        let scoped = src.startAccessingSecurityScopedResource()
        defer { if scoped { src.stopAccessingSecurityScopedResource() } }

        var isDir: ObjCBool = false
        fm.fileExists(atPath: src.path, isDirectory: &isDir)
        let target = dest.appendingPathComponent(src.lastPathComponent)
        do {
            if fm.fileExists(atPath: target.path) { try fm.removeItem(at: target) }
            try fm.copyItem(at: src, to: target)
        } catch {
            onReport?("could not copy \(src.lastPathComponent): \(error.localizedDescription)", nil)
            return
        }

        // A folder: find the executables inside it. The first one is offered
        // as the thing to run; a program with several is rare and the library
        // entry can be pointed at a different one later.
        var exes: [URL] = []
        if isDir.boolValue {
            let all = (try? fm.contentsOfDirectory(at: target, includingPropertiesForKeys: nil)) ?? []
            exes = all.filter { $0.pathExtension.lowercased() == "exe" }
            if exes.isEmpty {
                onReport?("copied \(src.lastPathComponent), but it contains no .exe", target)
                return
            }
        } else {
            exes = [target]
        }

        var text = "copied to C:\\ as \(target.lastPathComponent)"
        if isDir.boolValue { text += " — \(exes.count) executable(s) inside" }
        onReport?(text, exes.first)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        onReport?("cancelled", nil)
    }
}
