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
/// docs/logo.html (the source of truth for the look, motion and timings; keep them in step), drawn by
/// a Metal fragment shader in the logo's 64 x 64 grid (`MarvinMetalView`, MarvinRenderer.swift). Decorative
/// only: no hit testing, hidden from accessibility. It exists only while the no-video card does, so a
/// live picture costs nothing. With "Reduce motion" on it draws the still pose once and has no display
/// link at all.
///
/// Not SwiftUI's Canvas + TimelineView: every frame of those made AppKit run a layout and constraint
/// pass over the whole window (about 15 % CPU at 30 fps, none of it the drawing). A plain layer-backed
/// view with its own display link leaves SwiftUI and the window alone.
struct MarvinIdleView: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        MarvinMetal(animated: !reduceMotion)
            .frame(width: MarvinMetalView.size.width, height: MarvinMetalView.size.height)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

private struct MarvinMetal: NSViewRepresentable {
    let animated: Bool

    func makeNSView(context: Context) -> MarvinMetalView {
        let v = MarvinMetalView()
        v.animated = animated
        return v
    }

    func updateNSView(_ view: MarvinMetalView, context: Context) { view.animated = animated }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: MarvinMetalView, context: Context) -> CGSize? { MarvinMetalView.size }
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

    /// Mouth open, tongue straight out, eyes looking down at it.
    static let stillPose = MarvinPose()

    // ------------------------------------------------------------------ tongue geometry

    /// What the shader needs of the tongue (tongueGeom in docs/logo.html): an arrow laid along a bending
    /// spine, as the outline polygon (21 points, filled and stroked 1.4 wide in the same colour) and the
    /// groove polyline (6 points). The shaft slides out first, the head shrinks away last. Nil when the
    /// tongue is in.
    static func tongue(_ pose: MarvinPose) -> (outline: [CGPoint], groove: [CGPoint], grooveOpacity: Double)? {
        let len = pose.tongue
        guard len > 0.01 else { return nil }
        let m = pose.mouth
        let y0 = 35.5 - 1.5 * m, top = 40.5 - 6.5 * m
        let rootY = (y0 + top) / 2 - 1
        let shaft = 12.5, head = 9.0, full = shaft + head
        let k = min(1, max(0, len / 0.35)), sl = shaft * max(0, (len - 0.35) / 0.65), hl = head * k
        let wS = 3.1 * k, wH = 7.4 * k
        let stops = (0..<7).map { sl * Double($0) / 6 } + [sl + hl * 0.5, sl + hl]
        var x = 32.0, y = rootY, s = 0.0
        var pts: [(x: Double, y: Double, nx: Double, ny: Double)] = []
        for target in stops {
            let ds = (target - s) / 4
            for _ in 0..<4 {
                let a = pose.bend((s + ds / 2) / full)
                x += sin(a) * ds
                y += cos(a) * ds
                s += ds
            }
            let a = pose.bend(s / full)
            pts.append((x, y, cos(a), -sin(a)))
        }
        func at(_ i: Int, _ w: Double) -> CGPoint { CGPoint(x: pts[i].x + pts[i].nx * w, y: pts[i].y + pts[i].ny * w) }
        var outline = [CGPoint(x: 32 - wS, y: rootY - 2), CGPoint(x: 32 + wS, y: rootY - 2)]
        for i in 0..<7 { outline.append(at(i, wS)) }
        outline += [at(6, wH), at(7, wH / 2), at(8, 0), at(7, -wH / 2), at(6, -wH)]
        for i in (0..<7).reversed() { outline.append(at(i, -wS)) }

        var groove = [at(2, 0)]
        for i in 3..<7 { groove.append(at(i, 0)) }
        groove.append(CGPoint(x: (pts[6].x * 2 + pts[7].x) / 3, y: (pts[6].y * 2 + pts[7].y) / 3))
        return (outline, groove, 0.8 * min(1, max(0, (len - 0.5) / 0.3)))
    }
}
