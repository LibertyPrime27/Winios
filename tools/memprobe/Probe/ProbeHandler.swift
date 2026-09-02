import Foundation

/// The extension under test — a headless process, not a share sheet target.
///
/// This mirrors LiveContainer's LiveProcess exactly, and the reasons matter:
///
///  - Extension point `com.apple.ar.viewer`, not `com.apple.share-services`.
///    A regular app is not permitted to *host* a share extension through
///    NSExtension — only the system share sheet is — so lookups return nil with
///    no error, which is precisely what we saw. Any app may host AR Quick Look
///    content, so that point can be instantiated on demand.
///  - `NSExtensionActivationRule: FALSEPREDICATE` keeps it out of every UI.
///  - The `XPCService` block marks it `_ProcessType: App`. This is the most
///    likely reason LiveContainer's author observes app-level memory limits in
///    LiveProcess despite Apple's documented extension caps — and it is exactly
///    the question this ladder exists to settle.
///
/// Principal class is a plain NSObject: no view controller, no window. The
/// request callback is the entry point.
@objc(ProbeHandler)
final class ProbeHandler: NSObject, NSExtensionRequestHandling {
    private var started = false

    func beginRequest(with context: NSExtensionContext) {
        guard !started else { return }
        started = true
        DispatchQueue.global(qos: .userInitiated).async {
            // Expected to be killed before this returns. If it does return, the
            // ceiling was reached rather than a limit.
            Ladder.climb(host: "extension")
            context.completeRequest(returningItems: nil)
        }
    }
}
