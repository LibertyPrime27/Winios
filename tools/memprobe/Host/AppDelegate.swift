import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // First, before any of the app exists. Two reasons: the handlers can
        // only report a crash that happens after they are armed, and the
        // record of what the *previous* launch was doing has to be moved
        // somewhere safe before this launch overwrites it.
        CrashReports.prepare()

        let w = UIWindow(frame: UIScreen.main.bounds)
        // The library is the app; the probes are one tap away under
        // Diagnostics. They were the whole app for as long as there was
        // nothing to run.
        w.rootViewController = UINavigationController(rootViewController: LibraryViewController())
        w.makeKeyAndVisible()
        window = w
        return true
    }
}
