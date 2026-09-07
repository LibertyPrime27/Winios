import UIKit
import GameController

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

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            guard let key = press.key else { continue }
            handled = true
            let vk = Self.virtualKey(forHID: key.keyCode.rawValue)
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
            handled = true
            let vk = Self.virtualKey(forHID: key.keyCode.rawValue)
            if vk != 0 { w32_input_key(Int32(vk), 0) }
        }
        if !handled { super.pressesEnded(presses, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }

    // MARK: a hardware mouse

    private func watchForMice() {
        let c = NotificationCenter.default
        observers.append(c.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] n in
            self?.bind(mouse: n.object as? GCMouse)
        })
        observers.append(c.addObserver(forName: .GCMouseDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.mouseConnected = GCMouse.mice().isEmpty == false
        })
        bind(mouse: GCMouse.current)
    }

    private func bind(mouse: GCMouse?) {
        guard let input = mouse?.mouseInput else { return }
        mouseConnected = true
        // Relative, which is the whole reason to use this API.
        input.mouseMovedHandler = { _, dx, dy in
            // iOS reports y upward; Windows client coordinates go down.
            w32_input_mouse_delta(Int32(dx.rounded()), Int32((-dy).rounded()))
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
        let dx = Int32(d.x.rounded()), dy = Int32(d.y.rounded())
        guard dx != 0 || dy != 0 else { return }
        // Relative in both modes. When the guest is showing a cursor this
        // reads as a trackpad; when it has hidden one it *is* mouselook.
        w32_input_mouse_delta(dx, dy)
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
