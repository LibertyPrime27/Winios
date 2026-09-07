import UIKit

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
    /// The device's own panel, in pixels, landscape. Offered because it is
    /// the one resolution where nothing is being scaled: the guest draws
    /// exactly the pixels the display has. It is also the slowest, which is
    /// the trade -- on a modern phone it is three or four times the pixels of
    /// 720p through a software rasterizer.
    static var nativeMode: (w: Int, h: Int) {
        let b = UIScreen.main.nativeBounds          // always portrait-oriented
        let w = Int(max(b.width, b.height)), h = Int(min(b.width, b.height))
        return (w, h)
    }

    static var displayModes: [(w: Int, h: Int, label: String)] {
        let n = nativeMode
        return [
            (n.w, n.h, "\(n.w) × \(n.h) — native, nothing scaled, slowest"),
            (1920, 1080, "1920 × 1080"),
            (1280, 720, "1280 × 720"),
            (1024, 576, "1024 × 576"),
            (848,  480, "848 × 480"),
            (640,  360, "640 × 360 — fastest, softest"),
        ]
    }

    /// 1280x720 by default rather than native: it is a mode every game from
    /// the D3D9 era recognises, and half the pixels of 1080p. Someone who
    /// wants native can pick it.
    private static let defaultDisplayIndex = 2

    static var displayIndex: Int {
        get {
            let i = store.object(forKey: "displayIndex") as? Int ?? defaultDisplayIndex
            return displayModes.indices.contains(i) ? i : defaultDisplayIndex
        }
        set { store.set(newValue, forKey: "displayIndex") }
    }
    static var displayMode: (w: Int, h: Int, label: String) { displayModes[displayIndex] }

    // MARK: how big a Windows dialog should be
    //
    // Every desktop installer was drawn for a 96 DPI monitor, where its
    // wizard is about 500 by 350 pixels -- a comfortable window on a desk.
    // The same dialog on a tablet is a postage stamp, and a frame rendered
    // small enough to make it fill the screen is a postage stamp enlarged.
    //
    // Windows' own answer is DPI scaling, and it is the right one here:
    // tell the program the display is denser and it lays its dialog out
    // proportionally bigger, in its own units, with the text drawn at a
    // size where the strokes have room. Applied by `apply()` before a guest
    // starts, in the same place as the display mode.

    static let dialogScales = [100, 125, 150, 200, 250]

    /// 150% by default. A tablet held at arm's length is about that much
    /// further away than a monitor, and it is the setting where a wizard
    /// looks like a window rather than a stamp or a poster.
    static var dialogScale: Int {
        get {
            let v = store.object(forKey: "dialogScale") as? Int ?? 150
            return dialogScales.contains(v) ? v : 150
        }
        set { store.set(newValue, forKey: "dialogScale") }
    }

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
        // A dialog is laid out from the DPI, so this has to be set before
        // one is created -- changing it later would move nothing.
        w32_set_ui_dpi(Int32(96 * dialogScale / 100))
    }

    /// Whether the dynarec can actually be used. See JIT.swift -- this used
    /// to read xc_jit_available() directly, which reports 0 on iOS until an
    /// arena has been handed over, so it said "disabled" on devices where the
    /// JIT was perfectly usable and nobody had pressed the button.
    static var jitReady: Bool { JIT.isReady }
}
