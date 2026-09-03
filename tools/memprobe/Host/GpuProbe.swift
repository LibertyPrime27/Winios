import Foundation
import Metal

/// The GPU binding probe.
///
/// d12mt (gpu/d12mt) translates D3D12 root signatures into Metal argument-buffer
/// layouts on one assumption that can only be checked on real silicon: that a
/// Metal 3 argument buffer is a flat array of 8-byte slots, `[[id(k)]]` at byte
/// 8*k, so a D3D12 descriptor heap can be an MTLBuffer the CPU writes directly
/// and a descriptor table is that buffer bound at an offset. This draws one quad
/// through shaders d12mt produced (Host/Shaders/probe.*.msl, from
/// gpu/d12mt/tests/shaders/probe.hlsl), with every binding path fed by hand-
/// written descriptors, and reads the result back pixel by pixel.
///
/// Column layout and expected values are set by probe.hlsl; keep the two in step.
enum GpuProbe {

    // --- the binding plan for probe.hlsl, as d12mt-inspect prints it -------
    // root argument buffer [[buffer(30)]]: 40 bytes
    //   +0   8B  [1] root CBV  (root descriptor #0, device pointer)
    //   +8  16B  [0] constants (root constant words 0..3)
    //   +24  8B  [2] table -> argument buffer [[buffer(0)]]   (SRV x4 at id 0, CBV at id 4)
    //   +32  8B  [3] table -> argument buffer [[buffer(1)]]   (Sampler x2 at id 0)
    private static let rootBufferSlot = 30
    private static let descriptorStride = 8
    /// Vertex buffers are given slot 8 in this probe; d12mt's runtime will
    /// standardise this later, the shader does not care.
    private static let vertexBufferSlot = 8
    /// Non-zero bases so the "bind at an offset" half of the claim is tested too.
    private static let srvTableBase = 10
    private static let samplerTableBase = 3

    struct Column {
        let name: String
        let expected: [Float]
    }

    // Texel bytes -> the float the sampler must return.
    private static func unorm(_ r: UInt8, _ g: UInt8, _ b: UInt8, _ a: UInt8) -> [Float] {
        [Float(r) / 255, Float(g) / 255, Float(b) / 255, Float(a) / 255]
    }

    private static let t0Texels: [[UInt8]] = [[64, 0, 0, 255], [0, 64, 0, 255]]        // 2x1
    private static let t1Texels: [[UInt8]] = [[0, 0, 128, 255], [128, 128, 0, 255]]    // 2x1
    private static let t2Texel:  [UInt8]   = [192, 64, 32, 255]                        // 1x1
    private static let t3Texel:  [UInt8]   = [16, 32, 48, 128]                         // 1x1
    private static let rootConstants: [Float] = [0.1, 0.2, 0.3, 0.4]
    private static let rootCbv:       [Float] = [0.5, 0.6, 0.7, 0.8]
    private static let tableCbv:      [Float] = [0.9, 1.0, 1.1, 1.2]
    private static let vertexColor:   [Float] = [0.125, 0.25, 0.375, 0.5]

    static let columns: [Column] = [
        Column(name: "t0 via table sampler s0 (WRAP)  -> texel 0", expected: unorm(64, 0, 0, 255)),
        Column(name: "t0 via table sampler s1 (CLAMP) -> texel 1", expected: unorm(0, 64, 0, 255)),
        Column(name: "t1 via static sampler (WRAP)    -> texel 1", expected: unorm(128, 128, 0, 255)),
        Column(name: "t2  [[id(2)]]", expected: unorm(192, 64, 32, 255)),
        Column(name: "t3  [[id(3)]]", expected: unorm(16, 32, 48, 128)),
        Column(name: "root constants (push-constant words)", expected: rootConstants),
        Column(name: "root CBV (device pointer in root buffer)", expected: rootCbv),
        Column(name: "table CBV [[id(4)]]", expected: tableCbv),
        Column(name: "vertex color [[attribute(1)]] -> varying", expected: vertexColor),
    ]

    /// Runs the whole thing synchronously. Never throws: every failure is a
    /// line in the report, because the report is the point.
    static func run() -> String {
        var out: [String] = []
        guard let device = MTLCreateSystemDefaultDevice() else { return "no Metal device" }
        out.append("device: \(device.name)")
        out.append("argument buffers: tier \(device.argumentBuffersSupport == .tier2 ? 2 : 1)"
                   + (device.argumentBuffersSupport == .tier2 ? "" : "  (tier 2 required)"))
        if #available(iOS 16.0, *) {
            out.append("GPU family: apple\(highestAppleFamily(device))  (d12mt needs apple6+ / A13+)")
            do {
                out.append(contentsOf: try draw(device: device))
            } catch let e as ProbeError {
                out.append("FAILED: \(e.message)")
            } catch {
                out.append("FAILED: \(error)")
            }
        } else {
            out.append("iOS 16 or newer required (gpuResourceID)")
        }
        return out.joined(separator: "\n")
    }

    struct ProbeError: Error { let message: String }

    @available(iOS 16.0, *)
    private static func highestAppleFamily(_ d: MTLDevice) -> Int {
        if #available(iOS 17.0, *), d.supportsFamily(.apple9) { return 9 }
        let fams: [(MTLGPUFamily, Int)] = [(.apple8, 8), (.apple7, 7), (.apple6, 6), (.apple5, 5), (.apple4, 4)]
        for (f, n) in fams where d.supportsFamily(f) { return n }
        return 0
    }

    private static func source(_ name: String) throws -> String {
        // XcodeGen copies Host/Shaders as a folder reference, so the files land
        // in MemProbe.app/Shaders/. Check there first, then the bundle root.
        let url = Bundle.main.url(forResource: name, withExtension: "msl", subdirectory: "Shaders")
              ?? Bundle.main.url(forResource: name, withExtension: "msl")
        guard let u = url, let s = try? String(contentsOf: u, encoding: .utf8) else {
            let listing = (try? FileManager.default.contentsOfDirectory(atPath: Bundle.main.bundlePath))?.joined(separator: " ") ?? "?"
            throw ProbeError(message: "\(name).msl is not in the app bundle. Bundle root: \(listing)")
        }
        return s
    }

    @available(iOS 16.0, *)
    private static func compile(_ device: MTLDevice, _ name: String) throws -> MTLFunction {
        let src = try source(name)                 // a missing file is its own error, not a compiler one
        let opts = MTLCompileOptions()
        opts.languageVersion = .version3_0
        let lib: MTLLibrary
        do {
            lib = try device.makeLibrary(source: src, options: opts)
        } catch {
            throw ProbeError(message: "Metal compiler rejected \(name).msl:\n\(error.localizedDescription)")
        }
        guard let f = lib.makeFunction(name: "main0") else { throw ProbeError(message: "\(name): no main0") }
        return f
    }

    private static func texture(_ device: MTLDevice, width: Int, texels: [[UInt8]]) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: 1, mipmapped: false)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard let t = device.makeTexture(descriptor: d) else { throw ProbeError(message: "makeTexture failed") }
        var bytes: [UInt8] = []
        for tx in texels { bytes.append(contentsOf: tx) }
        t.replace(region: MTLRegionMake2D(0, 0, width, 1), mipmapLevel: 0, withBytes: bytes, bytesPerRow: width * 4)
        return t
    }

    private static func sampler(_ device: MTLDevice, _ mode: MTLSamplerAddressMode) throws -> MTLSamplerState {
        let d = MTLSamplerDescriptor()
        d.minFilter = .nearest; d.magFilter = .nearest; d.mipFilter = .notMipmapped
        d.sAddressMode = mode; d.tAddressMode = mode; d.rAddressMode = mode
        d.supportArgumentBuffers = true       // required before gpuResourceID is valid
        guard let s = device.makeSamplerState(descriptor: d) else { throw ProbeError(message: "makeSamplerState failed") }
        return s
    }

    private static func buffer(_ device: MTLDevice, _ floats: [Float]) throws -> MTLBuffer {
        guard let b = device.makeBuffer(bytes: floats, length: floats.count * 4, options: .storageModeShared)
        else { throw ProbeError(message: "makeBuffer failed") }
        return b
    }

    /// Writes an 8-byte descriptor at heap index `index`: this is the whole
    /// CreateShaderResourceView / CreateSampler / CreateConstantBufferView story.
    private static func writeDescriptor(_ heap: MTLBuffer, index: Int, _ value: UInt64) {
        heap.contents().advanced(by: index * descriptorStride).storeBytes(of: value, as: UInt64.self)
    }

    /// MTLResourceID is an opaque 8-byte struct; its bytes are what goes in the heap.
    @available(iOS 16.0, *)
    private static func id(_ r: MTLResourceID) -> UInt64 {
        withUnsafeBytes(of: r) { $0.load(as: UInt64.self) }
    }

    @available(iOS 16.0, *)
    private static func draw(device: MTLDevice) throws -> [String] {
        var report: [String] = []

        // Shaders straight from d12mt.
        let vs = try compile(device, "probe.vs")
        let ps = try compile(device, "probe.ps")
        report.append("MSL compiled on device: probe.vs, probe.ps")

        // Pipeline. The vertex descriptor is what d12mt's VertexInputSlot list
        // says: POSITION -> attribute 0 (float2), COLOR0 -> attribute 1 (float4).
        let vd = MTLVertexDescriptor()
        vd.attributes[0].format = .float2; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = vertexBufferSlot
        vd.attributes[1].format = .float4; vd.attributes[1].offset = 8;  vd.attributes[1].bufferIndex = vertexBufferSlot
        vd.layouts[vertexBufferSlot].stride = 24
        let pd = MTLRenderPipelineDescriptor()
        pd.vertexFunction = vs
        pd.fragmentFunction = ps
        pd.vertexDescriptor = vd
        pd.colorAttachments[0].pixelFormat = .rgba32Float
        let pso: MTLRenderPipelineState
        do { pso = try device.makeRenderPipelineState(descriptor: pd) }
        catch { throw ProbeError(message: "makeRenderPipelineState: \(error.localizedDescription)") }

        // Resources the descriptors will point at.
        let t0 = try texture(device, width: 2, texels: t0Texels)
        let t1 = try texture(device, width: 2, texels: t1Texels)
        let t2 = try texture(device, width: 1, texels: [t2Texel])
        let t3 = try texture(device, width: 1, texels: [t3Texel])
        let s0 = try sampler(device, .repeat)
        let s1 = try sampler(device, .clampToEdge)
        let rootCbvBuf  = try buffer(device, rootCbv)
        let tableCbvBuf = try buffer(device, tableCbv)

        // The descriptor heaps: plain shared buffers, written like D3D12 writes
        // descriptors, 8 bytes each, at arbitrary indices.
        guard let heap = device.makeBuffer(length: 4096, options: .storageModeShared),
              let samplerHeap = device.makeBuffer(length: 1024, options: .storageModeShared),
              let root = device.makeBuffer(length: 64, options: .storageModeShared)
        else { throw ProbeError(message: "heap buffers") }
        writeDescriptor(heap, index: srvTableBase + 0, id(t0.gpuResourceID))
        writeDescriptor(heap, index: srvTableBase + 1, id(t1.gpuResourceID))
        writeDescriptor(heap, index: srvTableBase + 2, id(t2.gpuResourceID))
        writeDescriptor(heap, index: srvTableBase + 3, id(t3.gpuResourceID))
        writeDescriptor(heap, index: srvTableBase + 4, tableCbvBuf.gpuAddress)
        writeDescriptor(samplerHeap, index: samplerTableBase + 0, id(s0.gpuResourceID))
        writeDescriptor(samplerHeap, index: samplerTableBase + 1, id(s1.gpuResourceID))

        // The root argument buffer: root descriptor address, then the constants.
        root.contents().storeBytes(of: rootCbvBuf.gpuAddress, as: UInt64.self)
        rootConstants.withUnsafeBytes { root.contents().advanced(by: 8).copyMemory(from: $0.baseAddress!, byteCount: 16) }

        // A full-target quad, every vertex the same colour.
        let quad: [(Float, Float)] = [(-1, -1), (1, -1), (-1, 1), (1, -1), (1, 1), (-1, 1)]
        var verts: [Float] = []
        for (x, y) in quad { verts += [x, y]; verts += vertexColor }
        let vb = try buffer(device, verts)

        // 9x1 float target: one column per test.
        let rtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: columns.count, height: 1, mipmapped: false)
        rtd.usage = [.renderTarget]
        rtd.storageMode = .shared
        guard let rt = device.makeTexture(descriptor: rtd) else { throw ProbeError(message: "render target") }

        guard let queue = device.makeCommandQueue(), let cb = queue.makeCommandBuffer() else { throw ProbeError(message: "command queue") }
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = rt
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColor(red: -1, green: -1, blue: -1, alpha: -1)
        rp.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rp) else { throw ProbeError(message: "encoder") }
        enc.setRenderPipelineState(pso)
        enc.setVertexBuffer(vb, offset: 0, index: vertexBufferSlot)
        // This is SetGraphicsRootDescriptorTable: the heap, at the table's base.
        enc.setFragmentBuffer(heap, offset: srvTableBase * descriptorStride, index: 0)
        enc.setFragmentBuffer(samplerHeap, offset: samplerTableBase * descriptorStride, index: 1)
        enc.setFragmentBuffer(root, offset: 0, index: rootBufferSlot)
        // Residency: Metal must be told what the argument buffers point at.
        let resident: [MTLResource] = [t0, t1, t2, t3, tableCbvBuf, rootCbvBuf]
        enc.useResources(resident, usage: .read, stages: .fragment)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        if let e = cb.error { throw ProbeError(message: "GPU error: \(e.localizedDescription)") }

        var pixels = [Float](repeating: .nan, count: columns.count * 4)
        rt.getBytes(&pixels, bytesPerRow: columns.count * 16, from: MTLRegionMake2D(0, 0, columns.count, 1), mipmapLevel: 0)

        var passed = 0
        for (i, c) in columns.enumerated() {
            let got = Array(pixels[i * 4 ..< i * 4 + 4])
            let ok = zip(got, c.expected).allSatisfy { abs($0 - $1) <= 1.0 / 255 + 1e-4 }
            if ok { passed += 1 }
            report.append("  \(ok ? "PASS" : "FAIL")  col \(i)  \(c.name)")
            if !ok { report.append("        expected \(fmt(c.expected))\n        got      \(fmt(got))") }
        }
        report.append(passed == columns.count
            ? "\(passed)/\(columns.count) PASS — descriptor heap == Metal argument buffer holds on this GPU. d12mt's binding model is validated here."
            : "\(passed)/\(columns.count) passed — see the FAIL lines; each names the binding path that is wrong.")
        return report
    }

    private static func fmt(_ v: [Float]) -> String {
        "(" + v.map { String(format: "%.4f", $0) }.joined(separator: ", ") + ")"
    }
}
