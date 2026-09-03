import Foundation
import Metal
import UIKit

/// One block at the top of every report saying which device produced it.
/// Results from two devices pasted into a chat are useless without this.
enum DeviceInfo {

    /// `hw.machine`, e.g. "iPad16,3" or "iPhone18,3" — the one identifier that
    /// is exact. Everything else is derived or looked up.
    static var modelIdentifier: String {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: size)
        sysctlbyname("hw.machine", &buf, &size, nil, 0)
        return String(cString: buf)
    }

    /// Marketing name and chip for the identifiers likely to show up here.
    /// Unknown identifiers fall through as themselves; the identifier is always
    /// printed too, so a miss in this table costs nothing but a friendly name.
    private static let known: [String: (name: String, chip: String)] = [
        // iPad Air / Pro with M-series
        "iPad13,1": ("iPad Air (5th gen)", "M1"),          "iPad13,2": ("iPad Air (5th gen)", "M1"),
        "iPad13,4": ("iPad Pro 11\" (3rd gen)", "M1"),     "iPad13,5": ("iPad Pro 11\" (3rd gen)", "M1"),
        "iPad13,6": ("iPad Pro 11\" (3rd gen)", "M1"),     "iPad13,7": ("iPad Pro 11\" (3rd gen)", "M1"),
        "iPad13,8": ("iPad Pro 12.9\" (5th gen)", "M1"),   "iPad13,9": ("iPad Pro 12.9\" (5th gen)", "M1"),
        "iPad13,10": ("iPad Pro 12.9\" (5th gen)", "M1"),  "iPad13,11": ("iPad Pro 12.9\" (5th gen)", "M1"),
        "iPad13,16": ("iPad Air 11\" (M2)", "M2"),         "iPad13,17": ("iPad Air 11\" (M2)", "M2"),
        "iPad13,18": ("iPad Air 13\" (M2)", "M2"),         "iPad13,19": ("iPad Air 13\" (M2)", "M2"),
        "iPad14,3": ("iPad Pro 11\" (4th gen)", "M2"),     "iPad14,4": ("iPad Pro 11\" (4th gen)", "M2"),
        "iPad14,5": ("iPad Pro 12.9\" (6th gen)", "M2"),   "iPad14,6": ("iPad Pro 12.9\" (6th gen)", "M2"),
        "iPad14,8": ("iPad Air 11\" (M2)", "M2"),          "iPad14,9": ("iPad Air 11\" (M2)", "M2"),
        "iPad14,10": ("iPad Air 13\" (M2)", "M2"),         "iPad14,11": ("iPad Air 13\" (M2)", "M2"),
        "iPad15,3": ("iPad Air 11\" (M3)", "M3"),          "iPad15,4": ("iPad Air 11\" (M3)", "M3"),
        "iPad15,5": ("iPad Air 13\" (M3)", "M3"),          "iPad15,6": ("iPad Air 13\" (M3)", "M3"),
        "iPad16,3": ("iPad Pro 11\" (M4)", "M4"),          "iPad16,4": ("iPad Pro 11\" (M4)", "M4"),
        "iPad16,5": ("iPad Pro 13\" (M4)", "M4"),          "iPad16,6": ("iPad Pro 13\" (M4)", "M4"),
        // iPhones with A15+ (TXM-era JIT protocol applies from here)
        "iPhone14,2": ("iPhone 13 Pro", "A15"),  "iPhone14,3": ("iPhone 13 Pro Max", "A15"),
        "iPhone14,4": ("iPhone 13 mini", "A15"), "iPhone14,5": ("iPhone 13", "A15"),
        "iPhone14,7": ("iPhone 14", "A15"),      "iPhone14,8": ("iPhone 14 Plus", "A15"),
        "iPhone15,2": ("iPhone 14 Pro", "A16"),  "iPhone15,3": ("iPhone 14 Pro Max", "A16"),
        "iPhone15,4": ("iPhone 15", "A16"),      "iPhone15,5": ("iPhone 15 Plus", "A16"),
        "iPhone16,1": ("iPhone 15 Pro", "A17 Pro"), "iPhone16,2": ("iPhone 15 Pro Max", "A17 Pro"),
        "iPhone17,1": ("iPhone 16 Pro", "A18 Pro"), "iPhone17,2": ("iPhone 16 Pro Max", "A18 Pro"),
        "iPhone17,3": ("iPhone 16", "A18"),      "iPhone17,4": ("iPhone 16 Plus", "A18"),
        "iPhone17,5": ("iPhone 16e", "A18"),
        "iPhone18,1": ("iPhone 17 Pro", "A19 Pro"), "iPhone18,2": ("iPhone 17 Pro Max", "A19 Pro"),
        "iPhone18,3": ("iPhone 17", "A19"),      "iPhone18,4": ("iPhone Air", "A19 Pro"),
    ]

    static func summary() -> String {
        let id = modelIdentifier
        let (name, chip) = known[id] ?? (id, "unknown chip")
        let gpu = MTLCreateSystemDefaultDevice()
        let gpuName = gpu?.name ?? "no Metal device"
        let ram = ProcessInfo.processInfo.physicalMemory
        let ramGB = String(format: "%.1f", Double(ram) / 1_073_741_824)
        let cores = ProcessInfo.processInfo.activeProcessorCount
        let os = UIDevice.current.systemName + " " + UIDevice.current.systemVersion
        var family = ""
        if #available(iOS 16.0, *), let d = gpu {
            if #available(iOS 17.0, *), d.supportsFamily(.apple9) { family = "apple9" }
            else if d.supportsFamily(.apple8) { family = "apple8" }
            else if d.supportsFamily(.apple7) { family = "apple7" }
            else if d.supportsFamily(.apple6) { family = "apple6" }
            else { family = "apple5 or older" }
        }
        let tier = gpu.map { $0.argumentBuffersSupport == .tier2 ? "tier 2" : "tier 1" } ?? "-"
        let build = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
        let idiom = UIDevice.current.userInterfaceIdiom == .pad ? "iPadOS" : "iOS"
        return """
        DEVICE
            \(name) — \(chip)  [\(id)]
            \(os) (\(idiom))   \(cores) cores   \(ramGB) GB RAM
            GPU \(gpuName)   \(family)   argument buffers \(tier)
            memprobe build \(build)
        """
    }
}
