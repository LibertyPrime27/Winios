import Foundation
import Metal

/// The GPU binding probe, for all three Direct3D generations.
///
/// d12mt (gpu/d12mt) translates Direct3D shaders and binding models into Metal
/// argument buffers on one assumption that can only be checked on real silicon:
/// that a Metal 3 argument buffer is a flat array of 8-byte slots, `[[id(k)]]`
/// at byte 8*k, so a D3D descriptor heap (D3D12) or a stage's register file
/// (D3D9/11) can be an MTLBuffer the CPU writes directly, and a table is that
/// buffer bound at an offset.
///
/// Three shader pairs, compiled by d12mt from the same nine-column probe
/// written three ways (Host/Shaders/*.msl, from gpu/d12mt/tests/shaders/):
///
///   D3D12  probe.hlsl    DXIL, root signature: root constants, root CBV,
///                        SRV+CBV table, sampler table, static sampler
///   D3D11  probe11.hlsl  SM5 DXBC, fixed slots: b0-2, t0-3, s0/s1/s4
///   D3D9   probe9.hlsl   SM3 bytecode: c0-2 constants, sampler stages 0-4,
///                        plus the fixed-function state blocks dxbc-spirv
///                        turns into constant buffers
///
/// Each draws one quad with every binding fed by hand-written descriptors and
/// reads the nine pixels back. The expected values are identical for all
/// three; only the way they are bound differs.
enum GpuProbe {

    enum API: CaseIterable {
        case d3d12, d3d11, d3d9
        var name: String {
            switch self {
            case .d3d12: return "D3D12 (DXIL, root signature)"
            case .d3d11: return "D3D11 (SM5, fixed slots)"
            case .d3d9:  return "D3D9 (SM3, constant registers)"
            }
        }
        var shaderBase: String { switch self { case .d3d12: return "probe"; case .d3d11: return "probe11"; case .d3d9: return "probe9" } }
    }

    // --- plan constants, mirrored from gpu/d12mt/include/d12mt/binding_plan.h
    private static let rootBufferSlot = 30          // D3D12 root buffer
    private static let descriptorStride = 8
    private static let legacySrvBase = 16           // LegacyPlan::kSrvBase
    private static let sm3AlphaTestCbv = 3, sm3ClipPlanesCbv = 5, sm3PointArgsCbv = 6, sm3SamplerStateCbv = 7
    /// Vertex buffers take slot 8 in this probe; the shader does not care.
    private static let vertexBufferSlot = 8
    /// Non-zero bases so the "bind at an offset" half of the claim is tested too.
    private static let srvTableBase = 10
    private static let samplerTableBase = 3

    struct Column { let name: String; let expected: [Float] }

    private static func unorm(_ r: UInt8, _ g: UInt8, _ b: UInt8, _ a: UInt8) -> [Float] {
        [Float(r) / 255, Float(g) / 255, Float(b) / 255, Float(a) / 255]
    }
    private static let t0Texels: [[UInt8]] = [[64, 0, 0, 255], [0, 64, 0, 255]]        // 2x1
    private static let t1Texels: [[UInt8]] = [[0, 0, 128, 255], [128, 128, 0, 255]]    // 2x1
    private static let t2Texel:  [UInt8]   = [192, 64, 32, 255]
    private static let t3Texel:  [UInt8]   = [16, 32, 48, 128]
    private static let rootConstants: [Float] = [0.1, 0.2, 0.3, 0.4]
    private static let rootCbv:       [Float] = [0.5, 0.6, 0.7, 0.8]
    private static let tableCbv:      [Float] = [0.9, 1.0, 1.1, 1.2]
    private static let vertexColor:   [Float] = [0.125, 0.25, 0.375, 0.5]

    static let columns: [Column] = [
        Column(name: "t0 via sampler s0 (WRAP)  -> texel 0", expected: unorm(64, 0, 0, 255)),
        Column(name: "t0 via sampler s1 (CLAMP) -> texel 1", expected: unorm(0, 64, 0, 255)),
        Column(name: "t1 via s4 / static (WRAP) -> texel 1", expected: unorm(128, 128, 0, 255)),
        Column(name: "t2", expected: unorm(192, 64, 32, 255)),
        Column(name: "t3", expected: unorm(16, 32, 48, 128)),
        Column(name: "constants b0 / c0", expected: rootConstants),
        Column(name: "constants b1 / c1", expected: rootCbv),
        Column(name: "constants b2 / c2", expected: tableCbv),
        Column(name: "vertex color [[attribute(1)]] -> varying", expected: vertexColor),
    ]

    struct ProbeError: Error { let message: String }

    /// Runs every API synchronously. Never throws: every failure is a line in
    /// the report, because the report is the point.
    static func run() -> String {
        var out: [String] = []
        guard let device = MTLCreateSystemDefaultDevice() else { return "no Metal device" }
        out.append("argument buffers: tier \(device.argumentBuffersSupport == .tier2 ? 2 : 1)"
                   + (device.argumentBuffersSupport == .tier2 ? "" : "  (tier 2 required)"))
        guard #available(iOS 16.0, *) else { out.append("iOS 16 or newer required (gpuResourceID)"); return out.joined(separator: "\n") }
        var total = 0, passed = 0
        for api in API.allCases {
            out.append("")
            out.append("— \(api.name)")
            do {
                let (lines, p, n) = try draw(device: device, api: api)
                out.append(contentsOf: lines); passed += p; total += n
            } catch let e as ProbeError {
                out.append("  FAILED before drawing: \(e.message)"); total += columns.count
            } catch {
                out.append("  FAILED before drawing: \(error)"); total += columns.count
            }
        }
        out.append("")
        out.append(passed == total
            ? "\(passed)/\(total) PASS across D3D9, D3D11 and D3D12 — one binding model, argument buffers as descriptor heaps, holds on this GPU."
            : "\(passed)/\(total) passed — each FAIL line names the API and the binding path that is wrong.")
        return out.joined(separator: "\n")
    }

    // MARK: - building blocks

    private static func source(_ name: String) throws -> String {
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
        let src = try source(name)
        let opts = MTLCompileOptions()
        opts.languageVersion = .version3_0
        let lib: MTLLibrary
        do { lib = try device.makeLibrary(source: src, options: opts) }
        catch { throw ProbeError(message: "Metal compiler rejected \(name).msl:\n\(error.localizedDescription)") }
        guard let f = lib.makeFunction(name: "main0") else { throw ProbeError(message: "\(name): no main0") }
        return f
    }

    private static func texture(_ device: MTLDevice, width: Int, texels: [[UInt8]]) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: 1, mipmapped: false)
        d.usage = .shaderRead; d.storageMode = .shared
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
        d.supportArgumentBuffers = true
        guard let s = device.makeSamplerState(descriptor: d) else { throw ProbeError(message: "makeSamplerState failed") }
        return s
    }

    private static func buffer(_ device: MTLDevice, _ floats: [Float]) throws -> MTLBuffer {
        guard let b = device.makeBuffer(bytes: floats, length: floats.count * 4, options: .storageModeShared)
        else { throw ProbeError(message: "makeBuffer failed") }
        return b
    }
    private static func buffer(_ device: MTLDevice, words: [UInt32], minLength: Int = 0) throws -> MTLBuffer {
        var w = words
        while w.count * 4 < max(minLength, 16) { w.append(0) }
        guard let b = device.makeBuffer(bytes: w, length: w.count * 4, options: .storageModeShared)
        else { throw ProbeError(message: "makeBuffer failed") }
        return b
    }

    /// Writes one 8-byte descriptor at heap index `index`. In D3D12 terms this is
    /// CreateShaderResourceView / CreateSampler / CreateConstantBufferView; in
    /// D3D11 terms it is what PSSetShaderResources does per slot.
    private static func writeDescriptor(_ heap: MTLBuffer, index: Int, _ value: UInt64) {
        heap.contents().advanced(by: index * descriptorStride).storeBytes(of: value, as: UInt64.self)
    }
    @available(iOS 16.0, *)
    private static func id(_ r: MTLResourceID) -> UInt64 { withUnsafeBytes(of: r) { $0.load(as: UInt64.self) } }

    // MARK: - one API

    @available(iOS 16.0, *)
    private static func draw(device: MTLDevice, api: API) throws -> ([String], Int, Int) {
        var report: [String] = []

        let vs = try compile(device, api.shaderBase + ".vs")
        let ps = try compile(device, api.shaderBase + ".ps")
        report.append("  MSL compiled on device")

        let vd = MTLVertexDescriptor()
        vd.attributes[0].format = .float2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = vertexBufferSlot
        vd.attributes[1].format = .float4; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = vertexBufferSlot
        vd.layouts[vertexBufferSlot].stride = 24
        let pd = MTLRenderPipelineDescriptor()
        pd.vertexFunction = vs; pd.fragmentFunction = ps; pd.vertexDescriptor = vd
        pd.colorAttachments[0].pixelFormat = .rgba32Float
        let pso: MTLRenderPipelineState
        do { pso = try device.makeRenderPipelineState(descriptor: pd) }
        catch { throw ProbeError(message: "makeRenderPipelineState: \(error.localizedDescription)") }

        // Resources shared by every API.
        let t0 = try texture(device, width: 2, texels: t0Texels)
        let t1 = try texture(device, width: 2, texels: t1Texels)
        let t2 = try texture(device, width: 1, texels: [t2Texel])
        let t3 = try texture(device, width: 1, texels: [t3Texel])
        let wrap = try sampler(device, .repeat)
        let clamp = try sampler(device, .clampToEdge)
        let constBuf0 = try buffer(device, rootConstants)
        let constBuf1 = try buffer(device, rootCbv)
        let constBuf2 = try buffer(device, tableCbv)

        guard let heap = device.makeBuffer(length: 8192, options: .storageModeShared),
              let samplerHeap = device.makeBuffer(length: 1024, options: .storageModeShared),
              let root = device.makeBuffer(length: 64, options: .storageModeShared)
        else { throw ProbeError(message: "heap buffers") }
        var resident: [MTLResource] = [t0, t1, t2, t3, constBuf0, constBuf1, constBuf2]

        // The part that differs: where the descriptors go.
        var heapBase = srvTableBase, samplerBase = samplerTableBase
        switch api {
        case .d3d12:
            // SRV table: t0..t3 at ids 0-3, the table CBV at id 4. Sampler table: s0, s1.
            writeDescriptor(heap, index: heapBase + 0, id(t0.gpuResourceID))
            writeDescriptor(heap, index: heapBase + 1, id(t1.gpuResourceID))
            writeDescriptor(heap, index: heapBase + 2, id(t2.gpuResourceID))
            writeDescriptor(heap, index: heapBase + 3, id(t3.gpuResourceID))
            writeDescriptor(heap, index: heapBase + 4, constBuf2.gpuAddress)
            writeDescriptor(samplerHeap, index: samplerBase + 0, id(wrap.gpuResourceID))
            writeDescriptor(samplerHeap, index: samplerBase + 1, id(clamp.gpuResourceID))
            // Root buffer: root descriptor (b1) address, then the root constants (b0).
            root.contents().storeBytes(of: constBuf1.gpuAddress, as: UInt64.self)
            rootConstants.withUnsafeBytes { root.contents().advanced(by: 8).copyMemory(from: $0.baseAddress!, byteCount: 16) }

        case .d3d11:
            // The legacy plan: one table per stage. b# at id #, t# at 16+#, s# in the sampler buffer.
            heapBase = 300                       // arbitrary: where this stage's table lives in the heap
            writeDescriptor(heap, index: heapBase + 0, constBuf0.gpuAddress)
            writeDescriptor(heap, index: heapBase + 1, constBuf1.gpuAddress)
            writeDescriptor(heap, index: heapBase + 2, constBuf2.gpuAddress)
            writeDescriptor(heap, index: heapBase + legacySrvBase + 0, id(t0.gpuResourceID))
            writeDescriptor(heap, index: heapBase + legacySrvBase + 1, id(t1.gpuResourceID))
            writeDescriptor(heap, index: heapBase + legacySrvBase + 2, id(t2.gpuResourceID))
            writeDescriptor(heap, index: heapBase + legacySrvBase + 3, id(t3.gpuResourceID))
            samplerBase = 20
            writeDescriptor(samplerHeap, index: samplerBase + 0, id(wrap.gpuResourceID))
            writeDescriptor(samplerHeap, index: samplerBase + 1, id(clamp.gpuResourceID))
            writeDescriptor(samplerHeap, index: samplerBase + 4, id(wrap.gpuResourceID))   // s4 is a plain sampler here

        case .d3d9:
            // c0..c2 live in the float-constant block at b0 (224 float4s); the
            // fixed-function blocks dxbc-spirv expects sit at their fixed b#s.
            // Alpha test MUST say ALWAYS (7): zero means NEVER and kills every pixel.
            var consts = [Float](repeating: 0, count: 224 * 4)
            consts.replaceSubrange(0..<4, with: rootConstants)
            consts.replaceSubrange(4..<8, with: rootCbv)
            consts.replaceSubrange(8..<12, with: tableCbv)
            let floatConsts = try buffer(device, consts)
            let alphaTest = try buffer(device, words: [7, 0, 0, 0])                  // compare ALWAYS
            let clipPlanes = try buffer(device, words: [0], minLength: 16 + 6 * 16)  // 0 planes enabled
            let pointArgs = try buffer(device, words: [0, 0, 0, 0])
            let samplerState = try buffer(device, words: [], minLength: 16 * 7 * 4)  // all zero: 2D, colour, not null
            resident += [floatConsts, alphaTest, clipPlanes, pointArgs, samplerState]
            heapBase = 0
            writeDescriptor(heap, index: heapBase + 0, floatConsts.gpuAddress)
            writeDescriptor(heap, index: heapBase + sm3AlphaTestCbv, alphaTest.gpuAddress)
            writeDescriptor(heap, index: heapBase + sm3ClipPlanesCbv, clipPlanes.gpuAddress)
            writeDescriptor(heap, index: heapBase + sm3PointArgsCbv, pointArgs.gpuAddress)
            writeDescriptor(heap, index: heapBase + sm3SamplerStateCbv, samplerState.gpuAddress)
            // Sampler stages: s0 = t0/wrap, s1 = t0/clamp, s2 = t1/wrap, s3 = t2, s4 = t3.
            let stageTex: [MTLTexture] = [t0, t0, t1, t2, t3]
            let stageSmp: [MTLSamplerState] = [wrap, clamp, wrap, wrap, clamp]
            for (i, t) in stageTex.enumerated() { writeDescriptor(heap, index: heapBase + legacySrvBase + i, id(t.gpuResourceID)) }
            samplerBase = 0
            for (i, s) in stageSmp.enumerated() { writeDescriptor(samplerHeap, index: samplerBase + i, id(s.gpuResourceID)) }
        }

        let quad: [(Float, Float)] = [(-1, -1), (1, -1), (-1, 1), (1, -1), (1, 1), (-1, 1)]
        var verts: [Float] = []
        for (x, y) in quad { verts += [x, y]; verts += vertexColor }
        let vb = try buffer(device, verts)

        let rtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: columns.count, height: 1, mipmapped: false)
        rtd.usage = [.renderTarget]; rtd.storageMode = .shared
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
        // Bind the heaps at the table's base: SetGraphicsRootDescriptorTable, or a D3D11 stage's register file.
        enc.setVertexBuffer(heap, offset: heapBase * descriptorStride, index: 0)
        enc.setFragmentBuffer(heap, offset: heapBase * descriptorStride, index: 0)
        enc.setFragmentBuffer(samplerHeap, offset: samplerBase * descriptorStride, index: 1)
        if api == .d3d12 { enc.setFragmentBuffer(root, offset: 0, index: rootBufferSlot) }
        enc.useResources(resident, usage: .read, stages: [.vertex, .fragment])
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
        return (report, passed, columns.count)
    }

    private static func fmt(_ v: [Float]) -> String {
        "(" + v.map { String(format: "%.4f", $0) }.joined(separator: ", ") + ")"
    }
}
