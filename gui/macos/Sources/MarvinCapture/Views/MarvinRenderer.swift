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
import Metal
import QuartzCore

/// Marvin's drawing as a Metal fragment shader, compiled at run time like the preview's (the Command Line
/// Tools ship no `metal` compiler). A port of the former SwiftUI Canvas drawing, in the same order: every
/// shape is a signed distance in the logo's 64 x 64 grid, turned into an antialiased coverage and
/// composited in linear light over the card's background (the drawable is `bgra8Unorm_srgb`, so the
/// hardware encodes the result). Compositing everything, the outer edge included, in linear light is what
/// keeps the long, nearly horizontal edges from looking ropey as Marvin tilts.
///
/// The label and its bars are drawn oversized and the body on top as a frame with the label cut out, so
/// the label's edge is antialiased once (filling the label over the body and the bars clipped to the same
/// edge antialiases it twice: a light line along the bottom of the bars).
enum MarvinShaders {
    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    struct Params {
        float4 xf;       // a b c d: device pixel -> logo grid (x' = a x + c y + tx, y' = b x + d y + ty)
        float4 a;        // tx, ty, logo units per pixel, mouth openness
        float4 b;        // gaze x, gaze y, lid, groove opacity
        float4 bg;       // background, sRGB
        float4 counts;   // outline points, groove points
        float2 outline[24];
        float2 groove[8];
    };

    struct VsOut { float4 pos [[position]]; };

    vertex VsOut marvinVertex(uint id [[vertex_id]]) {
        float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
        VsOut o;
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
        return o;
    }

    float3 lin(float3 c) { return select(c / 12.92, pow((c + 0.055) / 1.055, 2.4), c > 0.04045); }
    float3 hex(uint h) { return lin(float3(float((h >> 16) & 255u), float((h >> 8) & 255u), float(h & 255u)) / 255.0); }

    float3 over(float3 col, float3 c, float k) { return mix(col, c, saturate(k)); }

    // Signed distances (negative inside) in logo units.
    float sdBox(float2 p, float2 lo, float2 hi) {
        float2 q = abs(p - (lo + hi) * 0.5) - (hi - lo) * 0.5;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
    }
    float sdRound(float2 p, float2 lo, float2 hi, float r) {
        float2 q = abs(p - (lo + hi) * 0.5) - (hi - lo) * 0.5 + r;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    }
    float sdSeg(float2 p, float2 a, float2 b) {
        float2 pa = p - a, ba = b - a;
        return length(pa - ba * saturate(dot(pa, ba) / max(dot(ba, ba), 1e-12)));
    }

    // The mouth's curves are quadratic Beziers whose control point sits at the middle, so x is linear in t
    // and each curve is a parabola: y = y0 + k * 2 t (1 - t), t = (x - xl) / (xr - xl). Returns the offset
    // from y0 and the slope; outside the mouth the curve is the flat line y0.
    float2 curveAt(float x, float xl, float xr, float k) {
        float w = xr - xl, t = (x - xl) / w, tc = saturate(t);
        return float2(2.0 * k * tc * (1.0 - tc), (t > 0.0 && t < 1.0) ? 2.0 * k * (1.0 - 2.0 * t) / w : 0.0);
    }

    fragment float4 marvinFragment(VsOut in [[stage_in]], constant Params &P [[buffer(0)]]) {
        float2 pix = in.pos.xy;
        float2 p = float2(P.xf.x * pix.x + P.xf.z * pix.y + P.a.x, P.xf.y * pix.x + P.xf.w * pix.y + P.a.y);
        float px = P.a.z, m = P.a.w;
        #define COV(d) saturate(0.5 - (d) / px)

        float3 ink = hex(0x1a1d42u), body = hex(0x2d3180u), cream = hex(0xfbf3dcu), white = float3(1.0);
        float3 coral = hex(0xff5f5au), coralDark = hex(0xd43a48u), cheek = hex(0xff7a6eu);
        float3 col = lin(P.bg.rgb);

        // label and bars (oversized), then the body as a frame with the label's window cut out
        col = over(col, cream, COV(sdBox(p, float2(8.0, 7.5), float2(56.0, 31.5))));
        const uint bars[7] = { 0xddd8c6u, 0xdcc050u, 0x62b9bfu, 0x6fb57cu, 0xb76fabu, 0xd2604fu, 0x5069b5u };
        for (int i = 0; i < 7; i++) {
            float x0 = i == 0 ? 8.0 : 9.0 + float(i) * 46.0 / 7.0;
            float x1 = i == 6 ? 56.0 : 9.0 + float(i + 1) * 46.0 / 7.0 + 0.1;
            col = over(col, hex(bars[i]), COV(sdBox(p, float2(x0, 26.2), float2(x1, 31.5))));
        }
        float dFrame = max(sdRound(p, float2(4.0, 4.0), float2(60.0, 44.0), 8.0),
                           -sdRound(p, float2(9.0, 8.5), float2(55.0, 30.5), 5.0));
        col = over(col, body, COV(dFrame));
        col = over(col, ink, COV(sdRound(p, float2(14.0, 11.5), float2(50.0, 24.5), 6.5)));

        // eyes: white, the pupil clipped to the white, the lid
        for (int e = 0; e < 2; e++) {
            float cx = e == 0 ? 23.0 : 41.0;
            float eye = COV(length(p - float2(cx, 18.0)) - 4.4);
            col = over(col, white, eye);
            float2 pc = float2(cx + P.b.x * 2.0, 18.0 + P.b.y * 1.7);
            col = over(col, ink, eye * COV(length(p - pc) - 2.1));
            if (P.b.z > 0.0) col = over(col, ink, COV(sdBox(p, float2(cx - 5.0, 13.2), float2(cx + 5.0, 13.2 + P.b.z * 9.2))));
        }
        col = over(col, cheek, COV(length(p - float2(15.5, 37.5)) - 2.6));
        col = over(col, cheek, COV(length(p - float2(48.5, 37.5)) - 2.6));

        // mouth by openness m: 0 is the closed smile (a stroked curve), 1 fully open (a filled white shape)
        float xl = 26.0 - 1.5 * m, xr = 38.0 + 1.5 * m, y0 = 35.5 - 1.5 * m, top = 40.5 - 6.5 * m, bot = 40.5 + 8.5 * m;
        float hw = (xr - xl) * 0.5;
        float2 cu = curveAt(p.x, xl, xr, top - y0), cl = curveAt(p.x, xl, xr, bot - y0);
        float yu = y0 + cu.x, yl = y0 + cl.x;
        float nu = sqrt(1.0 + cu.y * cu.y), nl = sqrt(1.0 + cl.y * cl.y);
        float dU = (yu - p.y) / nu;   // > 0 above the upper curve
        float dL = (p.y - yl) / nl;   // > 0 below the lower curve
        float mouth = saturate(COV(dU) + COV(dL) - 1.0) * COV(abs(p.x - 32.0) - hw);
        col = over(col, white, mouth);
        if (m < 1.0) {
            float dxo = max(0.0, abs(p.x - 32.0) - hw);
            float dist = min(length(float2(dxo, (p.y - yu) / nu)), length(float2(dxo, (p.y - yl) / nl)));
            col = over(col, cream, COV(dist - 1.3 * (1.0 - m)));
        }
        col = over(col, ink, mouth * COV(sdBox(p, float2(22.0, y0), float2(42.0, y0 + 1.8))) * 0.2 * m);

        // the tongue shows only below the upper lip
        int n = int(P.counts.x);
        if (n >= 3) {
            float lip = COV(dU);
            float d = dot(p - P.outline[0], p - P.outline[0]);
            float s = 1.0;
            for (int i = 0, j = n - 1; i < n; j = i, i++) {
                float2 ed = P.outline[j] - P.outline[i], w = p - P.outline[i];
                float2 bb = w - ed * saturate(dot(w, ed) / max(dot(ed, ed), 1e-12));
                d = min(d, dot(bb, bb));
                bool3 c = bool3(p.y >= P.outline[i].y, p.y < P.outline[j].y, ed.x * w.y > ed.y * w.x);
                if (all(c) || all(!c)) s = -s;
            }
            col = over(col, coral, COV(s * sqrt(d) - 0.7) * lip);   // fill + 1.4 wide stroke, same colour

            int g = int(P.counts.y);
            float dg = 1e9;
            for (int i = 0; i + 1 < g; i++) dg = min(dg, sdSeg(p, P.groove[i], P.groove[i + 1]));
            col = over(col, coralDark, COV(dg - 0.65) * lip * P.b.w);
        }
        return float4(col, 1.0);
    }
    """
}

/// The layer-backed view that shows Marvin: a CAMetalLayer, and a display link at 30 fps only while the
/// animation runs (animated, and the window visible). Still (reduced motion) it draws on demand and has
/// no display link activity. It costs SwiftUI and the window nothing per frame: no hosting view, no
/// constraints.
@MainActor
final class MarvinMetalView: NSView {
    static let size = CGSize(width: 144, height: 120)

    var animated = true { didSet { if animated != oldValue { updateRunning(); draw() } } }

    private let idle = MarvinIdle()
    private let queue: MTLCommandQueue?
    private let pipeline: MTLRenderPipelineState?
    private var link: CADisplayLink?
    private var observers: [NSObjectProtocol] = []

    override init(frame: NSRect) {
        var q: MTLCommandQueue?, p: MTLRenderPipelineState?
        if let device = MTLCreateSystemDefaultDevice() {
            q = device.makeCommandQueue()
            do {
                let lib = try device.makeLibrary(source: MarvinShaders.source, options: nil)
                let d = MTLRenderPipelineDescriptor()
                d.vertexFunction = lib.makeFunction(name: "marvinVertex")
                d.fragmentFunction = lib.makeFunction(name: "marvinFragment")
                d.colorAttachments[0].pixelFormat = .bgra8Unorm_srgb
                p = try device.makeRenderPipelineState(descriptor: d)
            } catch {
                NSLog("Marvin: Metal shader failed: \(error)")
            }
            metalLayerDevice = device
        }
        queue = q
        pipeline = p
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        configureLayer()
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private var metalLayerDevice: MTLDevice?

    override func makeBackingLayer() -> CALayer { CAMetalLayer() }
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    private func configureLayer() {
        let l = metalLayer
        l.device = metalLayerDevice
        l.pixelFormat = .bgra8Unorm_srgb
        l.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        l.framebufferOnly = true
        l.isOpaque = true
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func isAccessibilityElement() -> Bool { false }
    override var intrinsicContentSize: NSSize { Self.size }

    // MARK: lifecycle

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        link?.invalidate()
        link = nil
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers = []
        guard let window else { return }
        let l = displayLink(target: self, selector: #selector(tick(_:)))
        l.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 30, preferred: 30)
        l.isPaused = true
        l.add(to: .main, forMode: .common)
        link = l
        let nc = NotificationCenter.default
        for name in [NSWindow.didChangeOcclusionStateNotification, NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification] {
            observers.append(nc.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.updateRunning() }
            })
        }
        updateRunning()
        draw()
    }

    private func updateRunning() {
        let visible = window.map { $0.occlusionState.contains(.visible) && !$0.isMiniaturized } ?? false
        link?.isPaused = !(animated && visible)
    }

    override func viewDidChangeBackingProperties() { super.viewDidChangeBackingProperties(); draw() }
    override func viewDidChangeEffectiveAppearance() { super.viewDidChangeEffectiveAppearance(); draw() }
    override func layout() { super.layout(); draw() }

    @objc private func tick(_ l: CADisplayLink) { draw() }

    // MARK: drawing

    /// The card's background, which the shader composites Marvin over.
    private func backgroundSRGB() -> SIMD3<Float> {
        var out = SIMD3<Float>(repeating: 0.5)
        effectiveAppearance.performAsCurrentDrawingAppearance {
            if let c = NSColor.controlBackgroundColor.usingColorSpace(.sRGB) {
                out = SIMD3(Float(c.redComponent), Float(c.greenComponent), Float(c.blueComponent))
            }
        }
        return out
    }

    private func draw() {
        guard let queue, let pipeline, window != nil, bounds.width > 0, bounds.height > 0 else { return }
        let scale = window?.backingScaleFactor ?? 2
        let pixels = CGSize(width: (bounds.width * scale).rounded(), height: (bounds.height * scale).rounded())
        let layer = metalLayer
        if layer.drawableSize != pixels { layer.drawableSize = pixels }
        layer.contentsScale = scale

        let pose = animated ? idle.pose(at: Date()) : MarvinIdle.stillPose
        var f = params(pose, scale: scale)

        guard let drawable = layer.nextDrawable(), let cb = queue.makeCommandBuffer() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        enc.setRenderPipelineState(pipeline)
        enc.setFragmentBytes(&f, length: f.count * MemoryLayout<Float>.stride, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    /// The shader's `Params`: 5 float4 (device pixel -> logo grid, scale, mouth, gaze, lid, groove opacity,
    /// background, point counts), then the tongue outline (24 float2) and groove (8 float2).
    private func params(_ pose: MarvinPose, scale: CGFloat) -> [Float] {
        // The logo's x 0..64 and y 5..58 (body top to tongue tip) scaled into the view, the body bobbing
        // and tilting about (32, 28); the shader wants the inverse, from device pixels.
        let w = bounds.width, h = bounds.height
        let s = min(w / 64, h / 53)
        let drop = 2.5   // centres body plus tongue in the 64 grid
        let forward = CGAffineTransform.identity
            .translatedBy(x: (w - 64 * s) / 2, y: (h - 53 * s) / 2 - 5 * s)
            .scaledBy(x: s, y: s)
            .translatedBy(x: 0, y: drop + pose.y)
            .translatedBy(x: 32, y: 28)
            .rotated(by: pose.rotation * .pi / 180)
            .translatedBy(x: -32, y: -28)
        let t = CGAffineTransform(scaleX: 1 / scale, y: 1 / scale).concatenating(forward.inverted())

        var f = [Float](repeating: 0, count: 84)
        func set(_ i: Int, _ v: [Double]) { for (k, x) in v.enumerated() { f[i + k] = Float(x) } }
        set(0, [t.a, t.b, t.c, t.d])
        set(4, [t.tx, t.ty, 1 / (s * scale), pose.mouth])
        set(8, [pose.gaze.x, pose.gaze.y, pose.lid, 0])
        let bg = backgroundSRGB()
        set(12, [Double(bg.x), Double(bg.y), Double(bg.z), 0])
        if let tongue = MarvinIdle.tongue(pose) {
            f[11] = Float(tongue.grooveOpacity)
            f[16] = Float(tongue.outline.count)
            f[17] = Float(tongue.groove.count)
            for (i, p) in tongue.outline.enumerated() { f[20 + 2 * i] = Float(p.x); f[21 + 2 * i] = Float(p.y) }
            for (i, p) in tongue.groove.enumerated() { f[68 + 2 * i] = Float(p.x); f[69 + 2 * i] = Float(p.y) }
        }
        return f
    }
}
