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

import SwiftUI

/// The animated Marvin of the no-signal screen: a port of the drawing and the idle animation in
/// docs/logo.html (the source of truth for the look, motion and timings; keep them in step), drawn with
/// a SwiftUI Canvas in the logo's 64 x 64 grid. Decorative only: no hit testing, hidden from
/// accessibility. It exists only while the no-video card does, so a live picture costs nothing; the
/// timeline runs at the display's rate and SwiftUI pauses it while the window is hidden or occluded. With
/// "Reduce motion" on it draws the still pose and has no timeline at all.
///
/// Edges are blended in linear light (`colorMode: .linear`): gamma-space blending makes the antialiased
/// edges of the long, nearly horizontal lines look ropey, and their steps crawl visibly as Marvin tilts.
struct MarvinIdleView: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var idle = MarvinIdle()

    var body: some View {
        Group {
            if reduceMotion {
                Canvas(colorMode: .linear) { ctx, size in MarvinIdle.draw(MarvinIdle.stillPose, in: &ctx, size: size) }
            } else {
                // Display rate (up to 120 Hz on ProMotion), so each frame's sub-pixel step is small. To cap it
                // at 30 fps again: TimelineView(.animation(minimumInterval: 1.0 / 30))
                TimelineView(.animation) { timeline in
                    Canvas(colorMode: .linear) { ctx, size in MarvinIdle.draw(idle.pose(at: timeline.date), in: &ctx, size: size) }
                }
            }
        }
        .frame(width: 144, height: 120)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// One frame of Marvin: everything the drawing needs.
struct MarvinPose {
    var mouth = 1.0                 // openness: 0 closed smile .. 1 open
    var tongue = 1.0                // how far out, 0 .. 1
    var bend: (Double) -> Double = { _ in 0 }   // the tongue spine's angle (radians) at u = 0 (root) .. 1 (tip)
    var y = 0.0, rotation = 0.0     // body bob and tilt (degrees)
    var gaze = CGPoint(x: 0, y: 1)
    var lid = 0.0                   // 0 open .. 1 shut
}

/// The animation state (class Idle in docs/logo.html), advanced by the timeline's dates.
final class MarvinIdle {
    private enum Phase { case out, retract, close, smile, open, extend }

    private var phase = Phase.out
    private var clock = 0.0, since = 0.0, until = Double.random(in: 4...7), popAt = -10.0
    private var nextGaze = Double.random(in: 0.8...2), nextBlink = Double.random(in: 1.5...3), blinkStart = -1.0
    private var gaze = CGPoint(x: 0, y: 1), target = CGPoint(x: 0, y: 0.2)
    private var last: Date?

    private static func smooth(_ k: Double) -> Double { k * k * (3 - 2 * k) }
    private static func backOut(_ k: Double) -> Double { 1 + 2.70158 * pow(k - 1, 3) + 1.70158 * pow(k - 1, 2) }  // overshoots, then settles

    func pose(at date: Date) -> MarvinPose {
        // a gap (the timeline was paused) is not animated through
        let dt = last.map { min(0.05, max(0, date.timeIntervalSince($0))) } ?? 0
        last = date
        clock += dt
        let t = clock

        // tongue and mouth: out -> retract -> close -> smile -> open -> extend -> out
        var len = 0.0, m = 1.0
        func k(_ dur: Double) -> Double { min(1, (t - since) / dur) }
        func go(_ p: Phase) { phase = p; since = t }
        switch phase {
        case .out:
            len = 1
            if t > until { go(.retract) }
        case .retract:
            let p = k(0.32)
            len = 1 - p * p * p
            if p >= 1 { go(.close) }
        case .close:
            let p = k(0.2)
            m = 1 - Self.smooth(p)
            if p >= 1 { go(.smile); until = t + .random(in: 3...6); nextBlink = min(nextBlink, t + 0.05) }
        case .smile:
            m = 0
            if t > until { go(.open) }
        case .open:
            let p = k(0.2)
            m = Self.smooth(p)
            if p >= 1 { go(.extend); popAt = t }
        case .extend:
            let p = k(0.45)
            len = Self.backOut(p)
            if p >= 1 { go(.out); until = t + .random(in: 5...9) }
        }

        // body
        let y = 1.3 * sin(2 * .pi * t / 3.2), rot = 1.4 * sin(2 * .pi * t / 5.3)

        // tongue bend: hangs with gravity as the body tilts, sways slowly, carries a ripple that travels from
        // root to tip, and wobbles for a moment after popping out
        let sincePop = t - popAt, pop = 24 * exp(-3.2 * sincePop)
        let bend: (Double) -> Double = { u in
            (rot * u + 6 * sin(2 * .pi * t / 2.6) * pow(u, 1.2) + 10 * sin(2 * .pi * t / 1.5 - 2.4 * u) * u
                + pop * sin(15 * sincePop - 2 * u) * u) * .pi / 180
        }

        // gaze: watch the tongue come out, otherwise a random glance every second or three
        if phase == .extend || (phase == .out && sincePop < 1.1) {
            target = CGPoint(x: 0, y: 1)
            nextGaze = t + 0.3
        } else if t > nextGaze {
            let c = Double.random(in: 0..<1)
            target = c < 0.35 ? CGPoint(x: 0, y: 0.2) : c < 0.5 ? CGPoint(x: 0, y: 1)
                : CGPoint(x: .random(in: -1...1), y: .random(in: -0.7...0.8))
            nextGaze = t + .random(in: 0.8...3)
        }
        let e = 1 - exp(-14 * dt)
        gaze = CGPoint(x: gaze.x + (target.x - gaze.x) * e, y: gaze.y + (target.y - gaze.y) * e)

        // blinks, occasionally double; a closed eye keeps a thin sliver at the bottom
        var lid = 0.0
        if blinkStart < 0 && t > nextBlink { blinkStart = t }
        if blinkStart >= 0 {
            let b = (t - blinkStart) / 0.17
            if b >= 1 {
                blinkStart = -1
                nextBlink = t + (Double.random(in: 0..<1) < 0.2 ? 0.12 : .random(in: 2.2...6))
            } else {
                lid = 0.86 * pow(sin(.pi * b), 0.6)
            }
        }
        return MarvinPose(mouth: m, tongue: len, bend: bend, y: y, rotation: rot, gaze: gaze, lid: lid)
    }

    // ------------------------------------------------------------------ drawing

    /// Mouth open, tongue straight out, eyes looking down at it.
    static let stillPose = MarvinPose()

    private static func rgb(_ hex: UInt32) -> Color {
        Color(red: Double((hex >> 16) & 255) / 255, green: Double((hex >> 8) & 255) / 255, blue: Double(hex & 255) / 255)
    }
    private static let ink = rgb(0x1a1d42), body = rgb(0x2d3180), cream = rgb(0xfbf3dc)
    private static let coral = rgb(0xff5f5a), coralDark = rgb(0xd43a48), cheek = rgb(0xff7a6e)
    private static let bars = [0xddd8c6, 0xdcc050, 0x62b9bf, 0x6fb57c, 0xb76fab, 0xd2604f, 0x5069b5].map { rgb(UInt32($0)) }  // muted SMPTE bars
    private static let drop = 2.5   // centres body plus tongue in the 64 grid

    static func draw(_ pose: MarvinPose, in ctx: inout GraphicsContext, size: CGSize) {
        // the logo's x 0..64 and y 5..58 (body top to tongue tip) scaled into the view
        let s = min(size.width / 64, size.height / 53)
        ctx.translateBy(x: (size.width - 64 * s) / 2, y: (size.height - 53 * s) / 2 - 5 * s)
        ctx.scaleBy(x: s, y: s)
        ctx.translateBy(x: 0, y: drop + pose.y)
        ctx.translateBy(x: 32, y: 28)
        ctx.rotate(by: .degrees(pose.rotation))
        ctx.translateBy(x: -32, y: -28)

        ctx.fill(Path(roundedRect: CGRect(x: 4, y: 4, width: 56, height: 40), cornerRadius: 8), with: .color(body))
        let label = Path(roundedRect: CGRect(x: 9, y: 8.5, width: 46, height: 22), cornerRadius: 5)
        ctx.fill(label, with: .color(cream))
        do {
            var c = ctx
            c.clip(to: label)
            for (i, color) in bars.enumerated() {
                c.fill(Path(CGRect(x: 9 + Double(i) * 46 / 7, y: 26.2, width: 46 / 7 + 0.1, height: 4.3)), with: .color(color))
            }
        }
        ctx.fill(Path(roundedRect: CGRect(x: 14, y: 11.5, width: 36, height: 13), cornerRadius: 6.5), with: .color(ink))
        for cx in [23.0, 41.0] {
            let eye = Path(ellipseIn: CGRect(x: cx - 4.4, y: 18 - 4.4, width: 8.8, height: 8.8))
            var c = ctx
            c.clip(to: eye)
            c.fill(eye, with: .color(.white))
            let px = cx + pose.gaze.x * 2, py = 18 + pose.gaze.y * 1.7
            c.fill(Path(ellipseIn: CGRect(x: px - 2.1, y: py - 2.1, width: 4.2, height: 4.2)), with: .color(ink))
            // the lid is the window colour and stays inside the window, so it needs no clip
            ctx.fill(Path(CGRect(x: cx - 5, y: 13.2, width: 10, height: pose.lid * 9.2)), with: .color(ink))
        }
        for cx in [15.5, 48.5] {
            ctx.fill(Path(ellipseIn: CGRect(x: cx - 2.6, y: 37.5 - 2.6, width: 5.2, height: 5.2)), with: .color(cheek))
        }

        // mouth, by openness m: 0 is the closed smile (a stroked curve), 1 fully open (a filled white shape)
        let m = pose.mouth
        let xl = 26 - 1.5 * m, xr = 38 + 1.5 * m, y0 = 35.5 - 1.5 * m, top = 40.5 - 6.5 * m, bot = 40.5 + 8.5 * m
        var mouth = Path()
        mouth.move(to: CGPoint(x: xl, y: y0))
        mouth.addQuadCurve(to: CGPoint(x: xr, y: y0), control: CGPoint(x: 32, y: top))
        mouth.addQuadCurve(to: CGPoint(x: xl, y: y0), control: CGPoint(x: 32, y: bot))
        mouth.closeSubpath()
        ctx.fill(mouth, with: .color(.white))
        if m < 1 { ctx.stroke(mouth, with: .color(cream), style: StrokeStyle(lineWidth: 2.6 * (1 - m), lineJoin: .round)) }
        do {
            var c = ctx
            c.clip(to: mouth)
            c.fill(Path(CGRect(x: 22, y: y0, width: 20, height: 1.8)), with: .color(ink.opacity(0.2 * m)))
        }

        // the tongue shows only below the upper lip
        var lip = Path()
        lip.move(to: CGPoint(x: -40, y: y0))
        lip.addLine(to: CGPoint(x: xl, y: y0))
        lip.addQuadCurve(to: CGPoint(x: xr, y: y0), control: CGPoint(x: 32, y: top))
        lip.addLine(to: CGPoint(x: 104, y: y0))
        lip.addLine(to: CGPoint(x: 104, y: 120))
        lip.addLine(to: CGPoint(x: -40, y: 120))
        lip.closeSubpath()
        var c = ctx
        c.clip(to: lip)
        drawTongue(in: &c, rootY: (y0 + top) / 2 - 1, len: pose.tongue, bend: pose.bend)
    }

    /// An arrow laid along a bending spine (tongueGeom in docs/logo.html): the shaft slides out first, the
    /// head shrinks away last.
    private static func drawTongue(in ctx: inout GraphicsContext, rootY: Double, len: Double, bend: (Double) -> Double) {
        guard len > 0.01 else { return }
        let shaft = 12.5, head = 9.0, full = shaft + head
        let k = min(1, max(0, len / 0.35)), sl = shaft * max(0, (len - 0.35) / 0.65), hl = head * k
        let wS = 3.1 * k, wH = 7.4 * k
        let stops = (0..<7).map { sl * Double($0) / 6 } + [sl + hl * 0.5, sl + hl]
        var x = 32.0, y = rootY, s = 0.0
        var pts: [(x: Double, y: Double, nx: Double, ny: Double)] = []
        for target in stops {
            let ds = (target - s) / 4
            for _ in 0..<4 {
                let a = bend((s + ds / 2) / full)
                x += sin(a) * ds
                y += cos(a) * ds
                s += ds
            }
            let a = bend(s / full)
            pts.append((x, y, cos(a), -sin(a)))
        }
        func at(_ i: Int, _ w: Double) -> CGPoint { CGPoint(x: pts[i].x + pts[i].nx * w, y: pts[i].y + pts[i].ny * w) }
        var outline = Path()
        outline.move(to: CGPoint(x: 32 - wS, y: rootY - 2))
        outline.addLine(to: CGPoint(x: 32 + wS, y: rootY - 2))
        for i in 0..<7 { outline.addLine(to: at(i, wS)) }
        outline.addLine(to: at(6, wH))
        outline.addLine(to: at(7, wH / 2))
        outline.addLine(to: at(8, 0))
        outline.addLine(to: at(7, -wH / 2))
        outline.addLine(to: at(6, -wH))
        for i in (0..<7).reversed() { outline.addLine(to: at(i, -wS)) }
        outline.closeSubpath()
        ctx.fill(outline, with: .color(coral))
        ctx.stroke(outline, with: .color(coral), style: StrokeStyle(lineWidth: 1.4, lineJoin: .round))

        var groove = Path()
        groove.move(to: at(2, 0))
        for i in 3..<7 { groove.addLine(to: at(i, 0)) }
        groove.addLine(to: CGPoint(x: (pts[6].x * 2 + pts[7].x) / 3, y: (pts[6].y * 2 + pts[7].y) / 3))
        let opacity = 0.8 * min(1, max(0, (len - 0.5) / 0.3))
        ctx.stroke(groove, with: .color(coralDark.opacity(opacity)), style: StrokeStyle(lineWidth: 1.3, lineCap: .round, lineJoin: .round))
    }
}
