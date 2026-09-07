import UIKit
import Metal
import QuartzCore

/// A Windows program's frames, full screen, as they arrive.
///
/// The guest runs its own loop on a background queue — clear, draw, present,
/// repeat — and never waits for the display. Every frame it presents is copied
/// under a lock and given a sequence number (see winprobe.c); this side asks
/// for anything newer once per vsync and uploads it. Frames the display never
/// got to are dropped rather than queued, which is what keeps the rate the
/// guest reports honest: it is the rate the emulator managed, not the rate the
/// screen refreshed at.
///
/// The pixels are still computed on the CPU -- by the guest's own x86 code on
/// the dynarec, and, for a game that draws through Direct3D 9 or 11, by the
/// integer rasterizers in win32/raster.c and win32/d3d11_raster.c. What Metal
/// does here is upload and scale. When those draw calls are wired to d12mt,
/// this is the layer the GPU will render into instead.
final class GuestViewController: UIViewController {

    private let exeName: String
    /// Where the executable lives. nil means the guests we bundled; a library
    /// program passes the app's C: drive.
    private let root: URL?
    /// Where the program's own DLLs are. An imported game keeps them beside
    /// its executable, which for a deeply nested one is not the folder the
    /// library shows -- and without this the windowed run would find them and
    /// the full-screen run would not, which is a confusing way to fail.
    private let dllDir: String?
    private var layerView = MetalFrameView()
    private let input = GuestInputView()
    private let keys = OnScreenKeys()
    /// Shown instead of the game keys while a dialog is what is on screen.
    private let dialogKeys = DialogKeys()
    private let hud = UILabel()
    private var link: CADisplayLink?
    private var seq: UInt64 = 0
    private var scratch = [UInt8](repeating: 0, count: 4 * 1920 * 1080)
    private var running = false
    private var framesSeen = 0
    private var started = CFAbsoluteTimeGetCurrent()
    private var output = ""

    /// True when this screen only *shows* a guest somebody else started.
    /// A visible install is that case: the run belongs to the importer, which
    /// has to look at the drive before and after it, so this screen cannot be
    /// the thing that starts it.
    private let watching: Bool
    /// What Close should do when the run is not ours to end.
    private var onClose: (() -> Void)?

    init(exe: String, root: URL? = nil, dllDir: String? = nil) {
        self.exeName = exe
        self.root = root
        self.dllDir = dllDir
        self.watching = false
        super.init(nibName: nil, bundle: nil)
    }

    /// Show whatever the guest draws, without starting one. The owner runs
    /// the guest and calls `finished` when it is over.
    init(watching title: String, onClose: (() -> Void)? = nil) {
        self.exeName = title
        self.root = nil
        self.dllDir = nil
        self.watching = true
        self.onClose = onClose
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    /// The owner's run has ended. Stops the display link and leaves the last
    /// frame up; the owner dismisses when it has said what happened.
    func finished(_ summary: String) {
        link?.invalidate(); link = nil
        running = false
        hud.text = summary
    }

    /// A running guest gets the whole screen, the long way round. A Windows
    /// game draws a 16:9 or 4:3 picture and there is nothing to be gained
    /// from showing it in a portrait letterbox, so this asks for landscape
    /// while it is up and the rest of the app goes back to whatever it was.
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black

        layerView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(layerView)

        // Over the frame, so it sees every touch first; the frame view has
        // nothing to interact with.
        input.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(input)

        keys.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(keys)

        dialogKeys.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(dialogKeys)
        // A dialog is what a visible install puts on screen, and WASD is no
        // use against one. Only one of the two is ever up.
        keys.isHidden = watching || !Settings.onScreenKeys
        dialogKeys.isHidden = !watching

        hud.translatesAutoresizingMaskIntoConstraints = false
        hud.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        hud.textColor = .white
        hud.numberOfLines = 0
        hud.text = "starting \(exeName)…"
        view.addSubview(hud)

        let close = UIButton(configuration: .bordered())
        close.setTitle("Close", for: .normal)
        close.translatesAutoresizingMaskIntoConstraints = false
        close.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        view.addSubview(close)

        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            layerView.topAnchor.constraint(equalTo: g.topAnchor),
            layerView.leadingAnchor.constraint(equalTo: g.leadingAnchor),
            layerView.trailingAnchor.constraint(equalTo: g.trailingAnchor),
            layerView.bottomAnchor.constraint(equalTo: g.bottomAnchor),
            input.topAnchor.constraint(equalTo: layerView.topAnchor),
            input.leadingAnchor.constraint(equalTo: layerView.leadingAnchor),
            input.trailingAnchor.constraint(equalTo: layerView.trailingAnchor),
            input.bottomAnchor.constraint(equalTo: layerView.bottomAnchor),
            keys.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 16),
            keys.trailingAnchor.constraint(lessThanOrEqualTo: g.trailingAnchor, constant: -16),
            keys.bottomAnchor.constraint(equalTo: g.bottomAnchor, constant: -12),
            // Centred along the very bottom, below where a centred dialog
            // reaches -- so the row cannot cover the buttons it exists to
            // let you press.
            dialogKeys.centerXAnchor.constraint(equalTo: g.centerXAnchor),
            dialogKeys.bottomAnchor.constraint(equalTo: g.bottomAnchor, constant: -8),
            hud.topAnchor.constraint(equalTo: g.topAnchor, constant: 12),
            hud.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 16),
            close.topAnchor.constraint(equalTo: g.topAnchor, constant: 12),
            close.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -16),
        ])
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard !running else { return }
        running = true
        started = CFAbsoluteTimeGetCurrent()
        // A hardware keyboard's presses arrive through the responder chain,
        // so something has to be first responder for them to arrive at all.
        input.becomeFirstResponder()

        let l = CADisplayLink(target: self, selector: #selector(tick))
        l.add(to: .main, forMode: .common)
        link = l

        // Watching only: the frames are already on their way from somebody
        // else's run, and starting one here would be a second guest.
        if watching { hud.text = "\(exeName)…"; return }

        guard let dir = root ?? Bundle.main.resourceURL?.appendingPathComponent("win32") else {
            hud.text = "guests not bundled"
            return
        }
        let exe = dir.appendingPathComponent(exeName).path
        let dll = dllDir ?? ""
        // Copied out before the closure: the log call below runs inside a
        // [weak self] block, where a bare property reference would not
        // resolve, and `self?.exeName` inside a nested async block is a way
        // to end up logging nothing because self went away.
        let name = exeName
        // Foreground band: a guest drawing frames is exactly the work this
        // app exists to do, and the efficiency cores would halve it.
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            var out = [CChar](repeating: 0, count: 4096)
            var ns: UInt64 = 0
            Settings.apply()          /* what the guest is told the screen is */
            xc_jit_enable(1)
            // No frame limit and no time limit: it draws until Close makes
            // Present fail. The DLL directory goes with it so an imported
            // game loads its own libraries here too.
            let rc = exe.withCString { p in
                dll.withCString { d in
                    win_probe_run_dir(p, dll.isEmpty ? nil : d, 0, 0, &out, out.count, &ns)
                }
            }
            xc_jit_enable(0)
            let text = String(cString: out)
            DispatchQueue.main.async {
                self?.output = text
                Logs.record(program: name, exit: rc, ms: ns / 1_000_000, report: text)
                self?.guestFinished(rc: rc, ns: ns)
            }
        }
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        input.resignFirstResponder()
        win_probe_request_stop()
        link?.invalidate()
        link = nil
    }

    @objc private func closeTapped() {
        // When the run is somebody else's, stopping it is all this button
        // does: the owner is still going to look at what it left behind and
        // has to be the one to take the screen down.
        if watching {
            hud.text = "Stopping…"
            onClose?()
            win_probe_request_stop()
            return
        }
        win_probe_request_stop()          // the guest exits on its next Present
        dismiss(animated: true)
    }

    private func guestFinished(rc: Int32, ns: UInt64) {
        link?.invalidate(); link = nil
        running = false
        hud.text = "\(exeName) exited \(rc)\n" + output
    }

    @objc private func tick() {
        var w: Int32 = 0, h: Int32 = 0, pitch: Int32 = 0
        let got = scratch.withUnsafeMutableBytes { buf -> Int32 in
            guard let base = buf.baseAddress else { return 0 }
            return win_probe_copy_frame(&seq, base, buf.count, &w, &h, &pitch)
        }
        guard got == 1, w > 0, h > 0 else {
            input.syncCursor()
            return
        }
        framesSeen += 1
        // The frame's size is what a touch has to be scaled against, and the
        // guest may have changed it (a mode switch), so it is set every frame
        // rather than once.
        input.frameSize = CGSize(width: CGFloat(w), height: CGFloat(h))
        input.syncCursor()
        scratch.withUnsafeBytes { buf in
            if let base = buf.baseAddress {
                layerView.upload(base, width: Int(w), height: Int(h), pitch: Int(pitch))
            }
        }
        layerView.draw()

        let dt = CFAbsoluteTimeGetCurrent() - started
        if dt > 0.5 {
            hud.text = String(format: "%@  %dx%d   %.1f guest fps   %d frames",
                              exeName, w, h, Double(framesSeen) / dt, framesSeen)
        }
    }
}

/// A CAMetalLayer with one texture and one full-screen triangle. The frame is
/// uploaded as BGRA8 (which is what X8R8G8B8 already is in memory) and sampled
/// with nearest filtering, so what appears is the pixels the guest wrote.
final class MetalFrameView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    private var metal: CAMetalLayer { layer as! CAMetalLayer }

    private let device = MTLCreateSystemDefaultDevice()
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState?
    private var texture: MTLTexture?
    private var sampler: MTLSamplerState?

    private static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut v_main(uint vid [[vertex_id]]) {
        // one triangle that covers the viewport
        float2 p = float2((vid << 1) & 2, vid & 2);
        VOut o;
        o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
        o.uv = p;
        return o;
    }
    fragment float4 f_main(VOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler s [[sampler(0)]]) {
        return tex.sample(s, in.uv);
    }
    """

    override init(frame: CGRect) { super.init(frame: frame); setup() }
    required init?(coder: NSCoder) { super.init(coder: coder); setup() }

    private func setup() {
        guard let device else { return }
        metal.device = device
        metal.pixelFormat = .bgra8Unorm
        metal.framebufferOnly = true
        metal.isOpaque = true
        queue = device.makeCommandQueue()

        guard let lib = try? device.makeLibrary(source: Self.source, options: nil) else { return }
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = lib.makeFunction(name: "v_main")
        d.fragmentFunction = lib.makeFunction(name: "f_main")
        d.colorAttachments[0].pixelFormat = .bgra8Unorm
        pipeline = try? device.makeRenderPipelineState(descriptor: d)

        let sd = MTLSamplerDescriptor()
        sd.minFilter = .nearest
        sd.magFilter = .nearest
        sd.sAddressMode = .clampToEdge
        sd.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: sd)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        metal.drawableSize = CGSize(width: bounds.width * (window?.screen.scale ?? 2),
                                    height: bounds.height * (window?.screen.scale ?? 2))
    }

    func upload(_ pixels: UnsafeRawPointer, width: Int, height: Int, pitch: Int) {
        guard let device else { return }
        if texture == nil || texture!.width != width || texture!.height != height {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                             width: width, height: height,
                                                             mipmapped: false)
            d.usage = .shaderRead
            texture = device.makeTexture(descriptor: d)
        }
        texture?.replace(region: MTLRegionMake2D(0, 0, width, height),
                         mipmapLevel: 0, withBytes: pixels, bytesPerRow: pitch)
    }

    func draw() {
        guard let queue, let pipeline, let texture, let sampler,
              let drawable = metal.nextDrawable() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
        pass.colorAttachments[0].storeAction = .store
        guard let cb = queue.makeCommandBuffer(),
              let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        enc.setRenderPipelineState(pipeline)
        enc.setFragmentTexture(texture, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }
}
