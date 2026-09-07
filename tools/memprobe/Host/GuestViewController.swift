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
/// The pixels are still drawn by the guest's own x86 code on the dynarec, so
/// what Metal does here is upload and scale. When DrawPrimitive is wired to
/// d12mt, this is the layer the GPU will render into instead.
final class GuestViewController: UIViewController {

    private let exeName: String
    private var layerView = MetalFrameView()
    private let hud = UILabel()
    private var link: CADisplayLink?
    private var seq: UInt64 = 0
    private var scratch = [UInt8](repeating: 0, count: 4 * 1920 * 1080)
    private var running = false
    private var framesSeen = 0
    private var started = CFAbsoluteTimeGetCurrent()
    private var output = ""

    init(exe: String) {
        self.exeName = exe
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black

        layerView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(layerView)

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

        let l = CADisplayLink(target: self, selector: #selector(tick))
        l.add(to: .main, forMode: .common)
        link = l

        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("win32") else {
            hud.text = "guests not bundled"
            return
        }
        let exe = dir.appendingPathComponent(exeName).path
        // Foreground band: a guest drawing frames is exactly the work this
        // app exists to do, and the efficiency cores would halve it.
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            var out = [CChar](repeating: 0, count: 4096)
            var ns: UInt64 = 0
            xc_jit_enable(1)
            // No frame limit: it draws until Close makes Present fail.
            let rc = exe.withCString { win_probe_run($0, nil, nil, &out, out.count, &ns, nil, nil) }
            xc_jit_enable(0)
            let text = String(cString: out)
            DispatchQueue.main.async {
                self?.output = text
                self?.guestFinished(rc: rc, ns: ns)
            }
        }
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        win_probe_request_stop()
        link?.invalidate()
        link = nil
    }

    @objc private func closeTapped() {
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
        guard got == 1, w > 0, h > 0 else { return }
        framesSeen += 1
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
