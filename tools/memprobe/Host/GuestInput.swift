import UIKit
import GameController

/// What is actually plugged in, asked rather than assumed.
///
/// Settings used to carry a note telling people to turn the on-screen keys
/// off when they attached a keyboard, which is asking someone to do something
/// the system already knows. GameController answers both questions directly —
/// `GCKeyboard.coalesced` is every attached keyboard merged into one,
/// `GCMouse.mice()` is every mouse — and posts a notification when either
/// changes, so the overlay can get out of the way on its own.
///
/// The line this puts on the Settings screen is the first thing to look at
/// when input is not working. If it says nothing is connected, nothing below
/// it can help: the keys are going somewhere else, or to nothing at all.
enum HardwareInput {

    static var keyboardConnected: Bool { GCKeyboard.coalesced != nil }
    static var mouseConnected: Bool { !GCMouse.mice().isEmpty }

    static var summary: String {
        "Hardware keyboard: \(keyboardConnected ? "connected" : "not connected"). "
        + "Mouse: \(mouseConnected ? "connected" : "not connected")."
    }

    /// Watch for either changing. The caller keeps the returned tokens and
    /// removes them when it goes away: a block-based observer holds its
    /// closure for as long as it is registered, and one that captures the
    /// screen it was made for would keep that screen alive for the life of
    /// the app.
    static func observe(_ changed: @escaping () -> Void) -> [NSObjectProtocol] {
        let c = NotificationCenter.default
        let names: [Notification.Name] = [
            .GCKeyboardDidConnect, .GCKeyboardDidDisconnect,
            .GCMouseDidConnect, .GCMouseDidDisconnect,
        ]
        return names.map { n in
            c.addObserver(forName: n, object: nil, queue: .main) { _ in changed() }
        }
    }
}

/// Keyboard and mouse for a running guest.
///
/// Three sources, in the order a game would prefer them:
///
/// 1. **A hardware keyboard.** `pressesBegan`/`pressesEnded` give the raw HID
///    usage *and* the layout-resolved character, which is exactly the pair
///    Windows wants: the usage becomes a virtual-key code for WM_KEYDOWN and
///    GetAsyncKeyState, the character becomes WM_CHAR. Deriving the character
///    ourselves would mean guessing the layout, and the system already knows.
/// 2. **A hardware mouse or trackpad**, through GameController's `GCMouse`.
///    That is the only iOS API that reports *relative* motion, which is what
///    mouselook needs — a pointer clamped to the edge of the screen stops
///    turning the view, and every FPS depends on it not doing that.
/// 3. **The touchscreen**, when there is neither. Dragging moves a cursor the
///    way a trackpad does, relatively, rather than teleporting it under the
///    finger: the finger would cover whatever it was aiming at, and a game
///    that has hidden the cursor wants deltas anyway. Tap is a left click,
///    two-finger tap a right click, and a long press holds the button down so
///    a drag can drag.
///
/// The guest tells us which mode it is in without being asked. A game that
/// hides the cursor (`ShowCursor(FALSE)`) is saying "I am reading relative
/// motion now" — so the crosshair disappears then, and reappears for menus.
/// With a real mouse connected that same signal is what captures the pointer
/// (`wantsPointerLock`), so the system pointer is out of the way for exactly
/// as long as the game is recentring one of its own.
///
/// Everything here runs on the main thread and calls into `w32_input_*`, which
/// takes a lock; the guest is reading that state from its own thread. Nothing
/// on this side touches guest memory.
final class GuestInputView: UIView, UIGestureRecognizerDelegate {

    /// Set by the owner each frame so touches can be mapped into the pixels
    /// the guest actually drew.
    var frameSize: CGSize = .zero

    private let crosshair = CALayer()
    private var pointer: CGPoint = .zero          // in client pixels
    private var heldByLongPress = false
    private var mouseConnected = false
    private var observers: [NSObjectProtocol] = []

    /// The modifier virtual keys the guest has been told are down. Kept so a
    /// modifier whose press or release never arrived can be corrected from
    /// `UIKey.modifierFlags`, which comes with every key event.
    private var heldModifiers: Set<Int> = []
    private var capsOn = false

    /// The mouse settings, read once a frame rather than once an event: a
    /// hardware mouse can report a thousand movements a second and going to
    /// UserDefaults on that path is not free.
    private var sensitivity = 1.0
    private var invertY = false
    /// The fraction of a pixel each axis is owed. See `sendMotion`.
    private var restX = 0.0
    private var restY = 0.0

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
        buildCrosshair()
        addGestures()
        watchForMice()
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    deinit { observers.forEach { NotificationCenter.default.removeObserver($0) } }

    // MARK: the cursor we draw

    private func buildCrosshair() {
        crosshair.bounds = CGRect(x: 0, y: 0, width: 22, height: 22)
        crosshair.borderColor = UIColor.white.withAlphaComponent(0.9).cgColor
        crosshair.borderWidth = 1.5
        crosshair.cornerRadius = 11
        crosshair.backgroundColor = UIColor.black.withAlphaComponent(0.25).cgColor
        crosshair.isHidden = true
        layer.addSublayer(crosshair)
    }

    /// Called from the frame tick: follow where the guest thinks the pointer
    /// is (which a game may have moved itself with SetCursorPos), and take the
    /// guest's word for whether it should be visible at all.
    func syncCursor() {
        keepKeyboardFocus()
        refreshMouseSettings()
        pollWheel()
        var gx: Int32 = 0, gy: Int32 = 0
        w32_cursor_pos(&gx, &gy)
        pointer = CGPoint(x: CGFloat(gx), y: CGFloat(gy))
        let show = w32_cursor_visible() != 0 && !mouseConnected && frameSize != .zero
        crosshair.isHidden = !show
        guard show else { return }
        let p = viewPoint(fromClient: pointer)
        crosshair.position = p
    }

    // MARK: mapping between the view and the guest's client area

    /// What a touch has to be expressed in: the client area of the guest's
    /// window, which is what WM_MOUSEMOVE's coordinates mean. Until the guest
    /// has made a window, the frame on screen is the best available answer.
    /// The frame is stretched to fill this view, so the two differ by scale
    /// alone and there is no letterbox to account for.
    private var clientSize: CGSize {
        if w32_has_window() != 0 {
            var cw: Int32 = 0, ch: Int32 = 0
            w32_client_size(&cw, &ch)
            if cw > 0 && ch > 0 { return CGSize(width: CGFloat(cw), height: CGFloat(ch)) }
        }
        if frameSize.width > 0 && frameSize.height > 0 { return frameSize }
        return CGSize(width: 1280, height: 720)
    }

    private func clientPoint(fromView p: CGPoint) -> CGPoint {
        let c = clientSize
        guard bounds.width > 0, bounds.height > 0 else { return .zero }
        return CGPoint(x: p.x / bounds.width * c.width, y: p.y / bounds.height * c.height)
    }
    private func viewPoint(fromClient p: CGPoint) -> CGPoint {
        let c = clientSize
        guard c.width > 0, c.height > 0 else { return .zero }
        return CGPoint(x: p.x / c.width * bounds.width, y: p.y / c.height * bounds.height)
    }
    /// A drag of `d` view points is this many client pixels.
    private func clientDelta(fromView d: CGPoint) -> CGPoint {
        let c = clientSize
        guard bounds.width > 0, bounds.height > 0 else { return .zero }
        return CGPoint(x: d.x / bounds.width * c.width, y: d.y / bounds.height * c.height)
    }

    // MARK: a hardware keyboard

    override var canBecomeFirstResponder: Bool { true }

    /// Presses only ever reach the first responder, so this view has to be
    /// one and has to stay one. There are several ways to quietly stop being
    /// one — anything presented over the guest takes it and nothing gives it
    /// back — and the symptom is a keyboard that worked a minute ago and now
    /// does nothing at all, with no way to tell from the screen. So it is
    /// re-taken from the frame tick, which costs a Bool comparison per frame
    /// in the case where nothing is wrong.
    func keepKeyboardFocus() {
        if window != nil && !isFirstResponder { becomeFirstResponder() }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil { becomeFirstResponder() }
    }

    /// The HID usages of the eight modifier keys, which are handled before
    /// everything else in an event.
    private static let modifierUsages = 0xE0...0xE7
    /// Caps Lock's usage. Deliberately not fed in as a key: see syncModifiers.
    private static let capsLockUsage = 0x39

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        // Modifiers first, then the flags, then everything else. The order
        // matters for a key pressed while Shift was already down: the guest
        // has to see the Shift before the key it modified, and a Shift held
        // from before this view had the keyboard exists only in the flags.
        for press in presses {
            guard let key = press.key, Self.modifierUsages.contains(key.keyCode.rawValue)
            else { continue }
            handled = true
            let vk = Self.virtualKey(forHID: key.keyCode.rawValue)
            guard vk != 0 else { continue }
            note(modifier: vk, down: true)
            w32_input_key(Int32(vk), 1)
        }
        if let flags = presses.first(where: { $0.key != nil })?.key?.modifierFlags {
            syncModifiers(flags)
        }
        for press in presses {
            guard let key = press.key else { continue }
            let hid = key.keyCode.rawValue
            if Self.modifierUsages.contains(hid) || hid == Self.capsLockUsage { continue }
            handled = true
            let vk = Self.virtualKey(forHID: hid)
            // The system resolved the layout, so hand the character over with
            // the key rather than guessing at one. It is not queued as a
            // WM_CHAR here: TranslateMessage is where Windows produces those,
            // and injecting one as well would give a text field two of every
            // letter.
            var ch: UInt32 = 0
            if let u = key.characters.unicodeScalars.first, u.value >= 0x20, u.value < 0x10000 { ch = u.value }
            if vk != 0 || ch != 0 { w32_input_key_ch(Int32(vk), 1, ch) }
        }
        if !handled { super.pressesBegan(presses, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            guard let key = press.key else { continue }
            let hid = key.keyCode.rawValue
            if hid == Self.capsLockUsage { continue }
            handled = true
            let vk = Self.virtualKey(forHID: hid)
            guard vk != 0 else { continue }
            note(modifier: vk, down: false)
            w32_input_key(Int32(vk), 0)
        }
        // After the releases, so anything the system says is still held gets
        // put back and anything it says is not gets let go.
        if let flags = presses.first(where: { $0.key != nil })?.key?.modifierFlags {
            syncModifiers(flags)
        }
        if !handled { super.pressesEnded(presses, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }

    /// Reconcile the modifier keys against what the system says is held.
    ///
    /// The press events on their own are not enough. A modifier pressed
    /// before this view became first responder never produces a press here,
    /// and one released while an alert or the app switcher was up never
    /// produces a release — and either way the guest is left believing Shift
    /// is down for the rest of the run, which a game shows as a walk key that
    /// has stuck. `modifierFlags` arrives with every key event and is the
    /// truth about right now, so every key event is a chance to correct it.
    ///
    /// The left-hand virtual key is the one sent for a flag with no press
    /// behind it, because the flags do not say which side it came from. There
    /// is no need to send VK_SHIFT, VK_CONTROL or VK_MENU as well: the guest
    /// derives each of those from its own pair (w32_input_key_ch in
    /// win32/user32.c), so sending it too would only give it a second opinion.
    private func syncModifiers(_ flags: UIKeyModifierFlags) {
        reconcile(flags.contains(.shift),     left: 0xA0, right: 0xA1)   // VK_LSHIFT/VK_RSHIFT
        reconcile(flags.contains(.control),   left: 0xA2, right: 0xA3)   // VK_LCONTROL/VK_RCONTROL
        reconcile(flags.contains(.alternate), left: 0xA4, right: 0xA5)   // VK_LMENU/VK_RMENU
        reconcile(flags.contains(.command),   left: 0x5B, right: 0x5C)   // VK_LWIN/VK_RWIN
        // Caps Lock is a lock, not a hold: the flag says the lock is on, not
        // that the key is down. So a change in it is one press and release,
        // and the physical key is skipped in pressesBegan/Ended — feeding in
        // both would flip the guest's toggle bit twice and leave it wrong.
        let caps = flags.contains(.alphaShift)
        if caps != capsOn {
            capsOn = caps
            w32_input_key(0x14, 1)          // VK_CAPITAL
            w32_input_key(0x14, 0)
        }
    }

    private func reconcile(_ down: Bool, left: Int, right: Int) {
        let held = heldModifiers.contains(left) || heldModifiers.contains(right)
        if down && !held {
            heldModifiers.insert(left)
            w32_input_key(Int32(left), 1)
        } else if !down && held {
            if heldModifiers.remove(left) != nil { w32_input_key(Int32(left), 0) }
            if heldModifiers.remove(right) != nil { w32_input_key(Int32(right), 0) }
        }
    }

    /// Keep the reconciler's idea of what is held in step with the presses it
    /// did see, so a real left/right key is not replaced by a synthetic one.
    private func note(modifier vk: Int, down: Bool) {
        guard (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C else { return }
        if down { heldModifiers.insert(vk) } else { heldModifiers.remove(vk) }
    }

    // MARK: a hardware mouse

    private func watchForMice() {
        let c = NotificationCenter.default
        observers.append(c.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] n in
            self?.bind(mouse: n.object as? GCMouse)
        })
        observers.append(c.addObserver(forName: .GCMouseDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            // Let go of the one that left before looking for another, or its
            // handlers keep it alive and the drawn crosshair stays hidden for
            // a mouse that is not there any more.
            self.boundMouse = nil
            self.mouseConnected = false
            self.bind(mouse: GCMouse.current)
        })
        bind(mouse: GCMouse.current)
    }

    private func bind(mouse: GCMouse?) {
        guard let input = mouse?.mouseInput else {
            mouseConnected = HardwareInput.mouseConnected
            return
        }
        mouseConnected = true
        // Relative, which is the whole reason to use this API. GameController
        // calls these on the main queue unless it is told otherwise, which is
        // what makes it safe for them to touch the remainders in sendMotion.
        input.mouseMovedHandler = { [weak self] _, dx, dy in
            // iOS reports y upward; Windows client coordinates go down.
            self?.sendMotion(dx: Double(dx), dy: Double(-dy))
        }
        input.leftButton.pressedChangedHandler = { _, _, pressed in
            w32_input_mouse_button(0, pressed ? 1 : 0)
        }
        input.rightButton?.pressedChangedHandler = { _, _, pressed in
            w32_input_mouse_button(1, pressed ? 1 : 0)
        }
        input.middleButton?.pressedChangedHandler = { _, _, pressed in
            w32_input_mouse_button(2, pressed ? 1 : 0)
        }
        // The wheel is read once a frame from the axis rather than through a
        // value-changed handler: `scroll` is typed as the protocol, and the
        // property read is the part of it that is certain.
        boundMouse = input
    }
    private var boundMouse: GCMouseInput?

    /// Called from the frame tick, with syncCursor.
    private func pollWheel() {
        guard let m = boundMouse else { return }
        let y = m.scroll.yAxis.value
        guard abs(y) > 0.01 else { return }
        // WHEEL_DELTA is 120 per notch, and the sign agrees: away from you is
        // positive on both sides.
        w32_input_mouse_wheel(Int32((y * 120).rounded()))
    }

    /// True when the system pointer should be taken away and the mouse read
    /// as motion alone.
    ///
    /// Only with a mouse connected, and only while the guest has hidden its
    /// own cursor — which is the guest saying it is doing mouselook and
    /// recentring the pointer itself with SetCursorPos every frame, exactly
    /// the case where a visible system pointer sliding to the edge of the
    /// screen is wrong. A menu leaves the cursor visible and keeps the
    /// pointer, so Close and the dialog keys stay reachable with the mouse
    /// rather than needing the screen touched.
    var wantsPointerLock: Bool { mouseConnected && w32_cursor_visible() == 0 }

    private func refreshMouseSettings() {
        sensitivity = Settings.mouseSensitivity
        invertY = Settings.mouseInvertY
    }

    /// Every source of motion goes through here, so the feel does not change
    /// with the hardware.
    ///
    /// This is where the sensitivity and the invert switch are applied, which
    /// until now was nowhere: Settings described both as being applied "to
    /// every source of motion" and nothing was reading either of them, so the
    /// slider moved and nothing happened.
    ///
    /// The remainder is kept rather than rounded away, per axis. At 0.3x a
    /// one-pixel report rounds to zero, and a sensitivity that low would
    /// otherwise not slow the pointer down but stop it dead.
    private func sendMotion(dx: Double, dy: Double) {
        restX += dx * sensitivity
        restY += dy * sensitivity * (invertY ? -1.0 : 1.0)
        let ix = restX.rounded(.towardZero)
        let iy = restY.rounded(.towardZero)
        restX -= ix
        restY -= iy
        guard ix != 0 || iy != 0 else { return }
        w32_input_mouse_delta(Int32(ix), Int32(iy))
    }

    // MARK: the touchscreen

    private var pan: UIPanGestureRecognizer!

    private func addGestures() {
        pan = UIPanGestureRecognizer(target: self, action: #selector(panned(_:)))
        pan.maximumNumberOfTouches = 1
        addGestureRecognizer(pan)

        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        addGestureRecognizer(tap)

        let two = UITapGestureRecognizer(target: self, action: #selector(twoFingerTapped(_:)))
        two.numberOfTouchesRequired = 2
        addGestureRecognizer(two)

        let hold = UILongPressGestureRecognizer(target: self, action: #selector(held(_:)))
        hold.minimumPressDuration = 0.35
        addGestureRecognizer(hold)

        // A hold and a pan have to run together, or press-and-drag never
        // works: the hold puts the button down, the pan moves while it is.
        hold.delegate = self
        pan.delegate = self
    }

    @objc private func panned(_ g: UIPanGestureRecognizer) {
        guard g.state == .changed || g.state == .began else {
            if g.state == .ended || g.state == .cancelled {
                if heldByLongPress { w32_input_mouse_button(0, 0); heldByLongPress = false }
            }
            g.setTranslation(.zero, in: self)
            return
        }
        let t = g.translation(in: self)
        g.setTranslation(.zero, in: self)
        let d = clientDelta(fromView: t)
        // Relative in both modes. When the guest is showing a cursor this
        // reads as a trackpad; when it has hidden one it *is* mouselook.
        sendMotion(dx: Double(d.x), dy: Double(d.y))
    }

    @objc private func tapped(_ g: UITapGestureRecognizer) {
        // A tap where the finger is: move there first, then click, so a menu
        // button under the finger is the one that gets pressed.
        if w32_cursor_visible() != 0 {
            let p = clientPoint(fromView: g.location(in: self))
            w32_input_mouse_move(Int32(p.x.rounded()), Int32(p.y.rounded()))
        }
        w32_input_mouse_button(0, 1)
        w32_input_mouse_button(0, 0)
    }

    @objc private func twoFingerTapped(_ g: UITapGestureRecognizer) {
        if w32_cursor_visible() != 0 {
            let p = clientPoint(fromView: g.location(in: self))
            w32_input_mouse_move(Int32(p.x.rounded()), Int32(p.y.rounded()))
        }
        w32_input_mouse_button(1, 1)
        w32_input_mouse_button(1, 0)
    }

    /// Press and hold, then drag: the button stays down for the drag, which is
    /// how a slider or a look-and-shoot works with one finger.
    @objc private func held(_ g: UILongPressGestureRecognizer) {
        switch g.state {
        case .began:
            if w32_cursor_visible() != 0 {
                let p = clientPoint(fromView: g.location(in: self))
                w32_input_mouse_move(Int32(p.x.rounded()), Int32(p.y.rounded()))
            }
            w32_input_mouse_button(0, 1)
            heldByLongPress = true
        case .ended, .cancelled, .failed:
            if heldByLongPress { w32_input_mouse_button(0, 0); heldByLongPress = false }
        default: break
        }
    }

    /// Scroll from a trackpad or a mouse wheel that came through UIKit rather
    /// than GameController.
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        if let t = touches.first, t.type == .indirectPointer {
            let p = clientPoint(fromView: t.location(in: self))
            w32_input_mouse_move(Int32(p.x.rounded()), Int32(p.y.rounded()))
            w32_input_mouse_button(0, 1)
        }
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        if let t = touches.first, t.type == .indirectPointer {
            w32_input_mouse_button(0, 0)
        }
    }

    func gestureRecognizer(_ g: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }

    // MARK: HID usage -> Windows virtual key
    //
    // Raw HID numbers with the names in comments rather than the
    // UIKeyboardHIDUsage cases: the numbers are the wire format, they are
    // stable, and a table of them can be checked against the HID spec in one
    // pass. 0 means "no virtual key we know", and those keys still produce a
    // character through key.characters.
    static func virtualKey(forHID hid: Int) -> Int {
        switch hid {
        case 0x04...0x1D: return 0x41 + (hid - 0x04)          // A-Z -> VK_A..VK_Z
        case 0x1E...0x26: return 0x31 + (hid - 0x1E)          // 1-9 -> VK_1..VK_9
        case 0x27: return 0x30                                 // 0
        case 0x28: return 0x0D                                 // Return    VK_RETURN
        case 0x29: return 0x1B                                 // Escape    VK_ESCAPE
        case 0x2A: return 0x08                                 // Backspace VK_BACK
        case 0x2B: return 0x09                                 // Tab       VK_TAB
        case 0x2C: return 0x20                                 // Space     VK_SPACE
        case 0x2D: return 0xBD                                 // -         VK_OEM_MINUS
        case 0x2E: return 0xBB                                 // =         VK_OEM_PLUS
        case 0x2F: return 0xDB                                 // [         VK_OEM_4
        case 0x30: return 0xDD                                 // ]         VK_OEM_6
        case 0x31: return 0xDC                                 // \         VK_OEM_5
        case 0x33: return 0xBA                                 // ;         VK_OEM_1
        case 0x34: return 0xDE                                 // '         VK_OEM_7
        case 0x35: return 0xC0                                 // `         VK_OEM_3
        case 0x36: return 0xBC                                 // ,         VK_OEM_COMMA
        case 0x37: return 0xBE                                 // .         VK_OEM_PERIOD
        case 0x38: return 0xBF                                 // /         VK_OEM_2
        case 0x39: return 0x14                                 // CapsLock  VK_CAPITAL
        case 0x3A...0x45: return 0x70 + (hid - 0x3A)           // F1-F12 -> VK_F1..VK_F12
        case 0x46: return 0x2C                                 // PrintScr  VK_SNAPSHOT
        case 0x48: return 0x13                                 // Pause     VK_PAUSE
        case 0x49: return 0x2D                                 // Insert    VK_INSERT
        case 0x4A: return 0x24                                 // Home      VK_HOME
        case 0x4B: return 0x21                                 // PageUp    VK_PRIOR
        case 0x4C: return 0x2E                                 // Delete    VK_DELETE
        case 0x4D: return 0x23                                 // End       VK_END
        case 0x4E: return 0x22                                 // PageDown  VK_NEXT
        case 0x4F: return 0x27                                 // Right     VK_RIGHT
        case 0x50: return 0x25                                 // Left      VK_LEFT
        case 0x51: return 0x28                                 // Down      VK_DOWN
        case 0x52: return 0x26                                 // Up        VK_UP
        case 0x53: return 0x90                                 // NumLock   VK_NUMLOCK
        case 0x54: return 0x6F                                 // KP /      VK_DIVIDE
        case 0x55: return 0x6A                                 // KP *      VK_MULTIPLY
        case 0x56: return 0x6D                                 // KP -      VK_SUBTRACT
        case 0x57: return 0x6B                                 // KP +      VK_ADD
        case 0x58: return 0x0D                                 // KP Enter  VK_RETURN
        case 0x59...0x61: return 0x61 + (hid - 0x59)           // KP 1-9 -> VK_NUMPAD1..9
        case 0x62: return 0x60                                 // KP 0      VK_NUMPAD0
        case 0x63: return 0x6E                                 // KP .      VK_DECIMAL
        case 0xE0: return 0xA2                                 // LCtrl     VK_LCONTROL
        case 0xE1: return 0xA0                                 // LShift    VK_LSHIFT
        case 0xE2: return 0xA4                                 // LAlt      VK_LMENU
        case 0xE3: return 0x5B                                 // LGUI      VK_LWIN
        case 0xE4: return 0xA3                                 // RCtrl     VK_RCONTROL
        case 0xE5: return 0xA1                                 // RShift    VK_RSHIFT
        case 0xE6: return 0xA5                                 // RAlt      VK_RMENU
        case 0xE7: return 0x5C                                 // RGUI      VK_RWIN
        default: return 0
        }
    }
}

/// The keys a game needs when there is no keyboard: movement, and the few
/// actions that are not reachable any other way.
///
/// Deliberately small. A full on-screen keyboard is worse than the system one
/// for typing, and a game's controls are a handful of keys *held down* rather
/// than typed -- so these are hold-to-press, and every way a touch can end
/// releases the key. A key left stuck down because a finger slid off the
/// button is the one failure that makes an on-screen control unusable.
/// The controls a dialog needs, which are not the controls a game needs.
///
/// A game wants WASD under a thumb. An installer wants a way to press the
/// button it is waiting on — and on a phone that is genuinely hard: the Next
/// button is a small target on a frame that has been scaled to the panel,
/// and a tap that lands two pixels off does nothing with no feedback.
///
/// So: Enter presses the dialog's default button, which is the one an
/// installer is waiting on at every step; Tab moves between the controls and
/// draws a box round the one it landed on; Space toggles a check box; Click
/// presses wherever the pointer already is, for when a drag has put it in the
/// right place and letting go would move it. Esc is Cancel.
///
/// These are not a substitute for tapping — tapping works and is quicker.
/// They are what makes the difference between an installer you can *usually*
/// get through and one you always can.
final class DialogKeys: UIView {

    /// label, virtual-key code, and a wider button for the ones that matter.
    private static let keys: [(String, Int32, Bool)] = [
        ("⏎ Enter", 0x0D, true), ("Tab", 0x09, false), ("Space", 0x20, false),
        ("Esc", 0x1B, false), ("← Bksp", 0x08, false),
    ]

    init() {
        super.init(frame: .zero)
        isUserInteractionEnabled = true

        var items: [UIView] = Self.keys.map { key($0.0, $0.1, wide: $0.2) }
        // Click is not a key, so it is built separately: it presses the
        // mouse where the pointer already is rather than where a finger is.
        items.append(clickButton("Click", right: false))
        items.append(clickButton("Right", right: true))

        let row = UIStackView(arrangedSubviews: items)
        row.spacing = 6
        row.alignment = .center
        row.translatesAutoresizingMaskIntoConstraints = false
        addSubview(row)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            row.topAnchor.constraint(equalTo: topAnchor),
            row.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    private func style(_ b: UIButton) {
        b.titleLabel?.font = .systemFont(ofSize: 15, weight: .semibold)
        b.setTitleColor(.white, for: .normal)
        b.backgroundColor = UIColor.white.withAlphaComponent(0.22)
        b.layer.cornerRadius = 8
        b.translatesAutoresizingMaskIntoConstraints = false
        b.heightAnchor.constraint(equalToConstant: 44).isActive = true
    }

    private func key(_ title: String, _ vk: Int32, wide: Bool) -> UIButton {
        let b = UIButton(type: .system)
        b.setTitle(title, for: .normal)
        b.tag = Int(vk)
        style(b)
        b.widthAnchor.constraint(greaterThanOrEqualToConstant: wide ? 96 : 62).isActive = true
        b.addTarget(self, action: #selector(keyTapped(_:)), for: .touchUpInside)
        return b
    }

    /// Down and up together on release. A dialog key is a press, not a hold
    /// — holding Enter on a wizard would run through several pages.
    @objc private func keyTapped(_ b: UIButton) {
        w32_input_key(Int32(b.tag), 1)
        w32_input_key(Int32(b.tag), 0)
    }

    private func clickButton(_ title: String, right: Bool) -> UIButton {
        let b = UIButton(type: .system)
        b.setTitle(title, for: .normal)
        b.tag = right ? 1 : 0
        style(b)
        b.widthAnchor.constraint(greaterThanOrEqualToConstant: 68).isActive = true
        b.addTarget(self, action: #selector(clickTapped(_:)), for: .touchUpInside)
        return b
    }

    @objc private func clickTapped(_ b: UIButton) {
        w32_input_mouse_button(Int32(b.tag), 1)
        w32_input_mouse_button(Int32(b.tag), 0)
    }
}

final class OnScreenKeys: UIView {

    /// label, virtual-key code
    private static let movement: [(String, Int32)] = [("W", 0x57), ("A", 0x41), ("S", 0x53), ("D", 0x44)]
    private static let actions: [(String, Int32)] = [
        ("Spc", 0x20), ("Shft", 0xA0), ("Ctrl", 0xA2), ("E", 0x45), ("R", 0x52), ("Tab", 0x09), ("Esc", 0x1B),
    ]

    init() {
        super.init(frame: .zero)
        isUserInteractionEnabled = true

        // W above A S D, the shape a thumb expects
        let top = UIStackView(arrangedSubviews: [key(Self.movement[0])])
        top.spacing = 6
        let bottom = UIStackView(arrangedSubviews: Self.movement[1...].map { key($0) })
        bottom.spacing = 6
        let cluster = UIStackView(arrangedSubviews: [top, bottom])
        cluster.axis = .vertical
        cluster.spacing = 6
        cluster.alignment = .center

        let acts = UIStackView(arrangedSubviews: Self.actions.map { key($0) })
        acts.spacing = 6
        acts.alignment = .bottom

        let row = UIStackView(arrangedSubviews: [cluster, acts])
        row.axis = .horizontal
        row.spacing = 24
        row.alignment = .bottom
        row.translatesAutoresizingMaskIntoConstraints = false
        addSubview(row)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            row.topAnchor.constraint(equalTo: topAnchor),
            row.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    private func key(_ k: (String, Int32)) -> UIButton {
        let b = UIButton(type: .system)
        b.setTitle(k.0, for: .normal)
        b.titleLabel?.font = .systemFont(ofSize: 15, weight: .semibold)
        b.setTitleColor(.white, for: .normal)
        b.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        b.layer.cornerRadius = 8
        b.tag = Int(k.1)
        b.translatesAutoresizingMaskIntoConstraints = false
        b.heightAnchor.constraint(equalToConstant: 44).isActive = true
        b.widthAnchor.constraint(greaterThanOrEqualToConstant: 46).isActive = true
        b.addTarget(self, action: #selector(keyDown(_:)), for: .touchDown)
        b.addTarget(self, action: #selector(keyUp(_:)),
                    for: [.touchUpInside, .touchUpOutside, .touchCancel, .touchDragExit])
        return b
    }

    @objc private func keyDown(_ b: UIButton) {
        b.backgroundColor = UIColor.white.withAlphaComponent(0.45)
        w32_input_key(Int32(b.tag), 1)
    }
    @objc private func keyUp(_ b: UIButton) {
        b.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        w32_input_key(Int32(b.tag), 0)
    }
}
