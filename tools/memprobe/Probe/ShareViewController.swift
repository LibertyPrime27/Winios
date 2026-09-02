import UIKit
import Social

/// The extension under test.
///
/// A share extension is used because it is a general-purpose, user-invocable
/// app extension -- the same broad class as LiveContainer's LiveProcess. The
/// extension POINT is a variable, not a constant: Apple states each point
/// defines its own limit, so a single number here does not generalise. Sweep
/// other points before drawing conclusions (see docs/MEMPROBE.md).
final class ShareViewController: UIViewController {

    private let label = UILabel()
    private var started = false

    /// When launched directly through NSExtension there may be no view
    /// hierarchy to trigger viewDidLoad, so the request callback is the
    /// reliable entry point. Both paths funnel here; it runs once.
    override func beginRequest(with context: NSExtensionContext) {
        super.beginRequest(with: context)
        startLadder()
    }

    private func startLadder() {
        guard !started else { return }
        started = true
        DispatchQueue.global(qos: .userInitiated).async {
            Ladder.climb(host: "extension")
            // Only reached if the ceiling was hit rather than the limit.
            DispatchQueue.main.async {
                self.label.text = "Reached the ceiling without being killed.\nRaise ceilingMB and run again."
                self.extensionContext?.completeRequest(returningItems: nil)
            }
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground

        label.numberOfLines = 0
        label.textAlignment = .center
        label.font = .monospacedSystemFont(ofSize: 14, weight: .regular)
        label.text = "Climbing…\nThis extension is expected to be killed.\nReopen MemProbe to read the result."
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            label.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            label.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
            label.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -24),
        ])

        startLadder()
    }
}
