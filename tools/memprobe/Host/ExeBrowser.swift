import UIKit

/// The app's virtual C: drive.
///
/// A game is an .exe plus a folder of DLLs and data, and a security-scoped URL
/// from the Files app is not something a PE loader can hold on to — so
/// everything the user brings in is copied into the app's own storage, and
/// that copy is what runs. This is where that lives.
///
/// It used to do the picking as well; `ImportViewController` owns that now,
/// because choosing a file is only the first of several decisions (a game or
/// an installer, which executable, where it lands) and they belong together
/// on a screen rather than split between a picker and a caller.
enum ExeBrowser {

    /// `Documents/c`, created on first use. The programs the user has imported
    /// are the folders directly inside it; `ProgramStore` treats the directory
    /// as the source of truth, so a folder copied in by any other means shows
    /// up too.
    static var driveC: URL? {
        guard let docs = FileManager.default.urls(for: .documentDirectory,
                                                  in: .userDomainMask).first
        else { return nil }
        let c = docs.appendingPathComponent("c")
        try? FileManager.default.createDirectory(at: c, withIntermediateDirectories: true)
        return c
    }
}
