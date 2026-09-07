import Foundation

/// The handful of things worth letting someone change, and where they live.
///
/// Deliberately small. Every setting here is one that changes what the
/// emulator actually does, and each is applied in exactly one place, named in
/// its comment. A settings screen full of switches that do not measurably
/// change anything is worse than no settings screen.
enum Settings {

    private static let store = UserDefaults.standard

    // MARK: what the game is told the screen is
    //
    // A game reads GetSystemMetrics or EnumDisplaySettings before it picks a
    // backbuffer, so this decides how many pixels it draws -- and until
    // DrawPrimitive reaches Metal those pixels go through a software
    // rasterizer. That makes it the biggest lever on frame rate after the
    // dynarec. Applied by `apply()` before a guest starts.

    /// Offered modes, widest first. All 16:9 so nothing is letterboxed twice,
    /// and all modes a Windows game from the D3D9 era will recognise.
    static let displayModes: [(w: Int, h: Int, label: String)] = [
        (1280, 720, "1280 × 720 — sharpest, slowest"),
        (1024, 576, "1024 × 576"),
        (848,  480, "848 × 480"),
        (640,  360, "640 × 360 — fastest, softest"),
    ]

    static var displayIndex: Int {
        get {
            let i = store.object(forKey: "displayIndex") as? Int ?? 0
            return displayModes.indices.contains(i) ? i : 0
        }
        set { store.set(newValue, forKey: "displayIndex") }
    }
    static var displayMode: (w: Int, h: Int, label: String) { displayModes[displayIndex] }

    // MARK: mouse
    //
    // Applied in GuestInput.swift, to every source of motion -- touch drag,
    // trackpad and a real mouse -- so the feel does not change with the
    // hardware. The scaling keeps a fractional remainder rather than
    // rounding each delta, or a sensitivity below 1 would throw away most
    // small movements instead of slowing them down.

    static var mouseSensitivity: Double {
        get {
            let v = store.object(forKey: "mouseSensitivity") as? Double ?? 1.0
            return min(max(v, 0.2), 4.0)
        }
        set { store.set(min(max(newValue, 0.2), 4.0), forKey: "mouseSensitivity") }
    }

    /// Some games treat upward mouse movement as looking down. This is the
    /// switch people go looking for first when a game feels wrong.
    static var mouseInvertY: Bool {
        get { store.object(forKey: "mouseInvertY") as? Bool ?? false }
        set { store.set(newValue, forKey: "mouseInvertY") }
    }

    // MARK: keyboard
    //
    // Applied in GuestViewController, which owns the overlay.

    /// The hold-to-press WASD overlay. On by default because without a
    /// hardware keyboard there is otherwise no way to move; off is for when
    /// a keyboard is attached and the overlay is just covering the game.
    static var onScreenKeys: Bool {
        get { store.object(forKey: "onScreenKeys") as? Bool ?? true }
        set { store.set(newValue, forKey: "onScreenKeys") }
    }

    /// Send the display setting into the emulator. Called before a guest
    /// starts rather than when the setting changes: a game reads the mode
    /// once, at startup, so changing it mid-run would have no effect and
    /// pretending otherwise would be misleading.
    static func apply() {
        let m = displayMode
        w32_set_screen_size(Int32(m.w), Int32(m.h))
    }

    /// Whether the dynarec can actually be used, which on iOS is not a
    /// question about the build: the JIT needs an executable arena that a
    /// debugger has authorised, once per launch, and `xc_jit_available()`
    /// reports 0 until one has been handed over. So this is the truth about
    /// this launch and not a capability flag.
    static var jitReady: Bool { xc_jit_available() != 0 }
}
