// Pinnacle Studio 500-USB open driver
// Copyright (C) 2026 Jonas Cz.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

import AppKit
import CMarvinCore
import ImageIO
import Metal
import QuartzCore
import UniformTypeIdentifiers

/// The Metal side of the preview: a render thread, three sets of plane textures and the pipeline.
/// Port of Windows' D3DPreview.
///
/// Threads and pacing
/// - The render thread blocks in `pin_preview_wait` (100 ms slices) while nothing arrives, so an idle
///   or signal-less preview costs nothing. When a frame is queued it switches the view's display link
///   on; every display-link tick (main run loop, cheap: it only stores a timestamp and signals) wakes
///   the thread, which calls `pin_preview_lock_due(now: targetTimestamp)`, uploads the planes,
///   unlocks, draws and presents. When the core has no frame queued any more the link is switched
///   off again and the thread goes back to waiting.
/// - Which timestamp: a CADisplayLink callback fires about one refresh BEFORE the picture it is
///   producing reaches the screen; `targetTimestamp` is the time of that refresh, `timestamp` the
///   previous one. A frame submitted in this callback is shown at `targetTimestamp`, so asking the core
///   for the frames due at `targetTimestamp` shows each one on the first refresh at or after its
///   present_time (with `timestamp` every frame would wait one refresh longer for no gain). Both are
///   on the CACurrentMediaTime clock, which is pin_clock_now()'s clock, so no conversion is needed.
///   Presenting is a plain `present(drawable)`: it lands on the next vsync, which is that refresh.
///
/// Session lifetime: the model detaches the preview before pin_close. `detach()` is synchronous: it
/// clears the session under `state` and waits until the thread is out of every pin_preview_* call
/// (`inCore`). The thread only enters the core through `enterCore()`, which hands out the session
/// only while it is still attached. (D3DPreview holds a SafeHandle reference for the same reason.)
///
/// Locks: `state` guards the session handshake, pause flag, tick and the reported frame info;
/// `gate` guards all Metal / CAMetalLayer use (render thread vs. the main thread's resize), like
/// D3DPreview's `_gate`. They are never held together in the order gate -> state.
final class PreviewRenderer: @unchecked Sendable {
    typealias Session = OpaquePointer

    private struct PlaneSet {
        var y: MTLTexture?, u: MTLTexture?, v: MTLTexture?
        var width = 0, height = 0, shiftX = 0, shiftY = 0
    }

    let layer: CAMetalLayer
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let dumpPath: String?

    // state lock
    private let state = NSCondition()
    private var session: Session?
    private var inCore = false
    private var paused = false
    private var running = true
    private var tickPending = false
    private var tickTarget = 0.0
    private var lastSeq: UInt64 = 0
    private var lastFrameClock = 0.0         // pin_clock_now() at the last presented frame, 0 = none
    private var frameDar: (num: Int, den: Int)?
    private var linkAvailable = false
    private var thread: Thread?

    // gate
    private let gate = NSLock()
    private var sets = [PlaneSet](repeating: PlaneSet(), count: 3)
    private var nextSet = 0
    private var lastSet = -1                  // index of the set the last frame is in (for redraws)
    private var lastMatrix = [Float](repeating: 0, count: 12)
    private var lastDar = (num: 4, den: 3)
    private var dumped = false
    private let inflight = DispatchSemaphore(value: 3)

    /// Asks the main thread to switch the display link on or off (set by the view).
    var setLinkActive: (@Sendable (Bool) -> Void)?

    /// nil when Metal is not available (no device, shader compile failure): `reason` says why.
    init?(layer: CAMetalLayer, failure: inout String) {
        guard let device = MTLCreateSystemDefaultDevice() else { failure = "No Metal device found."; return nil }
        guard let queue = device.makeCommandQueue() else { failure = "Could not create a Metal command queue."; return nil }
        let library: MTLLibrary
        do { library = try device.makeLibrary(source: PreviewShaders.source, options: nil) }
        catch { failure = "The preview shader did not compile: \(error.localizedDescription)"; return nil }
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "previewVertex")
        desc.fragmentFunction = library.makeFunction(name: "previewFragment")
        desc.colorAttachments[0].pixelFormat = .bgra8Unorm
        let pipeline: MTLRenderPipelineState
        do { pipeline = try device.makeRenderPipelineState(descriptor: desc) }
        catch { failure = "Could not create the preview pipeline: \(error.localizedDescription)"; return nil }

        self.device = device
        self.queue = queue
        self.pipeline = pipeline
        self.layer = layer
        let dump = ProcessInfo.processInfo.environment["MARVIN_PREVIEW_DUMP"]
        self.dumpPath = (dump?.isEmpty ?? true) ? nil : dump
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        layer.isOpaque = true
        // The Y'CbCr -> R'G'B' matrix yields gamma-encoded video R'G'B', which on a PC display is meant
        // to be shown as is (Windows does exactly that). Tagging the layer sRGB makes macOS treat those
        // values as sRGB-encoded and colour-match them to the display; Rec.709 would apply the camera
        // transfer curve's inverse (lifted blacks / wrong mid-tones) and an untagged layer would skip
        // colour management, oversaturating on wide-gamut screens. 601 vs 709 primaries differ by
        // little, and the matrix already follows the frame's own 601/709 flag.
        layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        layer.backgroundColor = CGColor(gray: 0, alpha: 1)

        let t = Thread { self.run() }   // strong: the renderer lives as long as the window; stop() ends the thread
        t.name = "Preview render"
        t.qualityOfService = .userInteractive
        thread = t
        t.start()
    }

    /// Ends the render thread (it does not wait for it).
    func stop() {
        state.lock()
        running = false
        state.broadcast()
        state.unlock()
    }

    // MARK: session handshake (main thread)

    func attach(_ s: Session, enabled: Bool) {
        state.lock()
        session = s
        lastSeq = 0
        paused = !enabled
        pin_preview_enable(s, enabled ? 1 : 0)
        state.broadcast()
        state.unlock()
    }

    /// Synchronous: when this returns the render thread is out of the core and will not enter it again
    /// with the old session. Then resets the picture to black.
    func detach() {
        state.lock()
        session = nil
        state.broadcast()
        while inCore { state.wait() }
        lastFrameClock = 0
        frameDar = nil
        lastSeq = 0
        state.unlock()
        gate.lock()
        lastSet = -1
        clearToBlack()
        gate.unlock()
    }

    /// Window minimised / fully covered / app hidden: stop the core's decoding and park the thread.
    func setVisible(_ visible: Bool) {
        state.lock()
        paused = !visible
        if let s = session { pin_preview_enable(s, visible ? 1 : 0) }
        state.broadcast()
        state.unlock()
    }

    var hasRecentFrame: Bool {
        state.lock(); defer { state.unlock() }
        return lastFrameClock > 0 && pin_clock_now() - lastFrameClock < 1.0
    }

    var currentDar: (num: Int, den: Int)? {
        state.lock(); defer { state.unlock() }
        return frameDar
    }

    // MARK: display link (main thread callbacks)

    func displayLinkTick(target: Double) {
        state.lock()
        tickPending = true
        tickTarget = target
        state.signal()
        state.unlock()
    }

    func setLinkAvailable(_ available: Bool) {
        state.lock(); linkAvailable = available; state.unlock()
    }

    // MARK: layout (main thread)

    /// New size in pixels (and scale): resizes the drawables and re-presents the last frame so a
    /// paused or still source doesn't leave a stretched or blank surface.
    func resize(pixelWidth: Int, pixelHeight: Int, scale: CGFloat) {
        gate.lock(); defer { gate.unlock() }
        layer.contentsScale = scale
        let size = CGSize(width: Swift.max(1, pixelWidth), height: Swift.max(1, pixelHeight))
        guard layer.drawableSize != size else { return }
        layer.drawableSize = size
        if lastSet >= 0 { drawLast() }
    }

    // MARK: render thread

    /// Hands out the session only while attached and visible, and marks the thread as inside the core.
    private func enterCore() -> Session? {
        state.lock(); defer { state.unlock() }
        guard running, !paused, let s = session else { return nil }
        inCore = true
        return s
    }

    private func leaveCore() {
        state.lock()
        inCore = false
        state.broadcast()
        state.unlock()
    }

    private func run() {
        while true {
            state.lock()
            while running && (session == nil || paused) { state.wait() }   // parked
            let alive = running
            let seq = lastSeq
            state.unlock()
            if !alive { return }

            guard let s = enterCore() else { continue }
            let r = pin_preview_wait(s, seq, 100)
            leaveCore()
            if r < 0 { Thread.sleep(forTimeInterval: 0.05); continue }   // closing (the model detaches first)
            if r == 0 { continue }
            presentLoop()
        }
    }

    /// Frames are queued: run the display link and show them as they come due.
    private func presentLoop() {
        setLinkActive?(true)
        defer { setLinkActive?(false) }
        while true {
            state.lock()
            let wait = linkAvailable ? 0.1 : 0.017
            if !tickPending && running && !paused && session != nil { _ = state.wait(until: Date(timeIntervalSinceNow: wait)) }
            let ticked = tickPending
            tickPending = false
            let target = tickTarget
            state.unlock()
            // No link tick (no window / link unavailable): fall back to the clock, so the picture still moves.
            let now = ticked ? target : pin_clock_now()

            guard let s = enterCore() else { return }
            var frame = pin_frame_t()
            frame.size = UInt32(MemoryLayout<pin_frame_t>.size)
            var more = false
            if pin_preview_lock_due(s, now, &frame) == PIN_OK {
                present(frame)
                pin_preview_unlock(s)   // right after the upload; drawing needs only our textures
                state.lock()
                lastSeq = frame.seq
                state.unlock()
            }
            var t = 0.0
            more = pin_preview_next_time(s, &t) != 0
            leaveCore()
            if !more { return }
        }
    }

    // MARK: upload + draw (render thread, planes locked)

    private func present(_ f: pin_frame_t) {
        gate.lock(); defer { gate.unlock() }
        guard inflight.wait(timeout: .now() + 0.2) == .success else { return }
        guard upload(f) else { inflight.signal(); return }
        var m = [Float](repeating: 0, count: 12)
        pin_yuv_to_rgb_matrix(f.matrix, f.full_range, &m)
        lastMatrix = m
        lastDar = (Int(f.dar_num), Int(f.dar_den))
        if !draw() { return }   // draw() signals `inflight` itself on every path

        if let path = dumpPath, !dumped {
            dumped = true
            dumpFrame(f, matrix: m, to: path)
        }
        state.lock()
        lastFrameClock = pin_clock_now()
        frameDar = lastDar
        state.unlock()
    }

    /// Copies the three planes into the next texture set of the ring. The GPU may still be reading
    /// the sets of the last two frames, so each frame gets its own (replaceRegion does not wait for it).
    private func upload(_ f: pin_frame_t) -> Bool {
        let w = Int(f.width), h = Int(f.height)
        guard w > 0, h > 0, let p0 = f.plane.0, let p1 = f.plane.1, let p2 = f.plane.2 else { return false }
        let sx = Int(f.chroma_shift_x), sy = Int(f.chroma_shift_y)
        let cw = Swift.max(1, w >> sx), ch = Swift.max(1, h >> sy)
        let i = nextSet
        if sets[i].y == nil || sets[i].width != w || sets[i].height != h || sets[i].shiftX != sx || sets[i].shiftY != sy {
            guard let ty = makePlane(w, h), let tu = makePlane(cw, ch), let tv = makePlane(cw, ch) else { return false }
            sets[i] = PlaneSet(y: ty, u: tu, v: tv, width: w, height: h, shiftX: sx, shiftY: sy)
        }
        sets[i].y!.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: p0, bytesPerRow: Int(f.stride.0))
        sets[i].u!.replace(region: MTLRegionMake2D(0, 0, cw, ch), mipmapLevel: 0, withBytes: p1, bytesPerRow: Int(f.stride.1))
        sets[i].v!.replace(region: MTLRegionMake2D(0, 0, cw, ch), mipmapLevel: 0, withBytes: p2, bytesPerRow: Int(f.stride.2))
        lastSet = i
        nextSet = (i + 1) % sets.count
        return true
    }

    private func makePlane(_ w: Int, _ h: Int) -> MTLTexture? {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: w, height: h, mipmapped: false)
        d.usage = .shaderRead
        d.storageMode = .shared
        return device.makeTexture(descriptor: d)
    }

    /// Draws the last uploaded frame into the next drawable and presents it. Takes over the caller's
    /// `inflight` slot: it is released when the command buffer completes (or at once on failure).
    private func draw() -> Bool {
        guard lastSet >= 0, let drawable = layer.nextDrawable(), let cb = encode(into: drawable.texture, set: lastSet) else {
            inflight.signal()
            return false
        }
        cb.present(drawable)
        cb.addCompletedHandler { [inflight] _ in inflight.signal() }
        cb.commit()
        return true
    }

    /// Resize path (main thread, gate held): its own `inflight` slot.
    private func drawLast() {
        guard inflight.wait(timeout: .now() + 0.2) == .success else { return }
        _ = draw()
    }

    /// Black fill of the drawable (session detached).
    private func clearToBlack() {
        guard layer.drawableSize.width >= 1, let drawable = layer.nextDrawable(), let cb = queue.makeCommandBuffer() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        cb.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    /// Clear to black, then the fullscreen triangle inside the viewport the core's fit rule gives for
    /// the frame's DAR (the view already has the picture's aspect, so normally that is all of it; while
    /// the window is still catching up with a changed DAR it is bars instead of a stretched picture).
    private func encode(into target: MTLTexture, set index: Int) -> MTLCommandBuffer? {
        let s = sets[index]
        guard let y = s.y, let u = s.u, let v = s.v, let cb = queue.makeCommandBuffer() else { return nil }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return nil }
        var x: Int32 = 0, yy: Int32 = 0, rw: Int32 = 0, rh: Int32 = 0
        pin_fit_rect(Int32(clamping: lastDar.num), Int32(clamping: lastDar.den),
                     Int32(target.width), Int32(target.height), &x, &yy, &rw, &rh)
        enc.setViewport(MTLViewport(originX: Double(x), originY: Double(yy),
                                    width: Double(Swift.max(1, rw)), height: Double(Swift.max(1, rh)),
                                    znear: 0, zfar: 1))
        enc.setRenderPipelineState(pipeline)
        enc.setFragmentTexture(y, index: 0)
        enc.setFragmentTexture(u, index: 1)
        enc.setFragmentTexture(v, index: 2)
        lastMatrix.withUnsafeBytes { enc.setFragmentBytes($0.baseAddress!, length: 48, index: 0) }
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        return cb
    }

    // MARK: MARVIN_PREVIEW_DUMP

    /// Developer aid: writes the first presented frame to `path` (PNG), twice. `path` itself is the GPU
    /// result (the real pipeline rendered into an offscreen texture of the frame's size and read back,
    /// which a CAMetalLayer's drawable cannot be); `<path>.cpu.png` is the same conversion done on the
    /// CPU from the locked planes with the same matrix (nearest chroma), to tell shader bugs from
    /// upload bugs.
    private func dumpFrame(_ f: pin_frame_t, matrix m: [Float], to path: String) {
        let w = Int(f.width), h = Int(f.height)
        // CPU
        var cpu = [UInt8](repeating: 255, count: w * h * 4)
        let sx = Int(f.chroma_shift_x), sy = Int(f.chroma_shift_y)
        for yy in 0..<h {
            for xx in 0..<w {
                let yv = Float(f.plane.0![yy * Int(f.stride.0) + xx]) / 255
                let uv = Float(f.plane.1![(yy >> sy) * Int(f.stride.1) + (xx >> sx)]) / 255
                let vv = Float(f.plane.2![(yy >> sy) * Int(f.stride.2) + (xx >> sx)]) / 255
                func ch(_ r: Int) -> UInt8 {
                    let v = m[r * 4] * yv + m[r * 4 + 1] * uv + m[r * 4 + 2] * vv + m[r * 4 + 3]
                    return UInt8(Swift.max(0, Swift.min(1, v)) * 255 + 0.5)
                }
                let o = (yy * w + xx) * 4
                cpu[o] = ch(2); cpu[o + 1] = ch(1); cpu[o + 2] = ch(0)   // BGRA
            }
        }
        Self.writePNG(cpu, w, h, to: path + ".cpu.png")
        // GPU: same encode path, DAR forced to the frame's pixel shape so the whole texture is filled.
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        d.usage = .renderTarget
        d.storageMode = .shared
        let saved = lastDar
        lastDar = (w, h)
        defer { lastDar = saved }
        guard let tex = device.makeTexture(descriptor: d), let cb = encode(into: tex, set: lastSet) else { return }
        cb.commit()
        cb.waitUntilCompleted()
        var gpu = [UInt8](repeating: 0, count: w * h * 4)
        tex.getBytes(&gpu, bytesPerRow: w * 4, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        Self.writePNG(gpu, w, h, to: path)
    }

    private static func writePNG(_ bgra: [UInt8], _ w: Int, _ h: Int, to path: String) {
        let info = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue
        guard let provider = CGDataProvider(data: Data(bgra) as CFData),
              let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: info),
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
              let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(dest, image, nil)
        CGImageDestinationFinalize(dest)
    }
}
