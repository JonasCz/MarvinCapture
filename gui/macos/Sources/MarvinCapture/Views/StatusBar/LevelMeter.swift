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
import SwiftUI

/// Thin horizontal audio level meter: a bar for the current peak (dBFS), coloured by zone
/// (green, yellow near full scale, red at it), and a tick for the held peak of the last 10 s.
/// Port of Controls/LevelMeter.cs. A layer-backed NSView the model pushes values into at 30 Hz, so
/// a meter update repaints these 100x4 pt and nothing else (no SwiftUI graph update, no layout).
final class LevelMeterView: NSView {
    static let floorDb = WindowModel.meterFloorDb
    static let width: CGFloat = 100
    static let height: CGFloat = 4

    /// Zone limits (dBFS): green up to -18, yellow up to -6, red above.
    private static let yellowFrom = -18.0
    private static let redFrom = -6.0

    private var level = LevelMeterView.floorDb
    private var hold = LevelMeterView.floorDb

    // Plain layers, no drawRect: an update only moves a few frames (actions off).
    private let track = CALayer()
    private let zoneLayers = [CALayer(), CALayer(), CALayer()]   // green, yellow, red
    private let holdLayer = CALayer()

    override var intrinsicContentSize: NSSize { NSSize(width: Self.width, height: Self.height) }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func isAccessibilityElement() -> Bool { false }

    override init(frame: NSRect) {
        super.init(frame: NSRect(x: 0, y: 0, width: Self.width, height: Self.height))
        wantsLayer = true
        layer?.masksToBounds = false
        track.frame = CGRect(x: 0, y: 0, width: Self.width, height: Self.height)
        track.cornerRadius = Self.height / 2
        track.masksToBounds = true            // clips the zone bars to the rounded track
        for z in zoneLayers { track.addSublayer(z) }
        layer?.addSublayer(track)
        layer?.addSublayer(holdLayer)
        applyColors()
        layoutBars()
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        applyColors()
    }

    private func applyColors() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            track.backgroundColor = NSColor.labelColor.withAlphaComponent(0.12).cgColor
            zoneLayers[0].backgroundColor = NSColor.systemGreen.cgColor
            zoneLayers[1].backgroundColor = NSColor.systemYellow.cgColor
            zoneLayers[2].backgroundColor = NSColor.systemRed.cgColor
            holdLayer.backgroundColor = (hold > -1 ? NSColor.systemRed : NSColor.labelColor).cgColor
        }
    }

    func set(level: Double, hold: Double) {
        guard level != self.level || hold != self.hold else { return }
        let redChanged = (hold > -1) != (self.hold > -1)
        self.level = level
        self.hold = hold
        if redChanged { applyColors() }
        layoutBars()
    }

    private static func fraction(_ db: Double) -> CGFloat {
        CGFloat(Swift.min(Swift.max((db - floorDb) / -floorDb, 0), 1))
    }

    private func layoutBars() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let w = Self.width, h = Self.height
        let barW = Self.fraction(level) * w
        let zones: [(from: Double, to: Double)] = [(Self.floorDb, Self.yellowFrom), (Self.yellowFrom, Self.redFrom), (Self.redFrom, 0)]
        for (i, z) in zones.enumerated() {
            let x0 = Self.fraction(z.from) * w
            let x1 = Swift.min(Self.fraction(z.to) * w, barW)
            zoneLayers[i].frame = x1 > x0 ? CGRect(x: x0, y: 0, width: x1 - x0, height: h) : .zero
        }
        if hold > Self.floorDb {
            // The tick turns red when the last 10 s came within 1 dB of clipping.
            holdLayer.frame = CGRect(x: Swift.max(0, Self.fraction(hold) * w - 2), y: 0, width: 2, height: h)
            holdLayer.isHidden = false
        } else {
            holdLayer.isHidden = true
        }
        CATransaction.commit()
    }
}

/// SwiftUI host of one meter. Deliberately reads no observed state: the model pushes values into the
/// view it registers here (`WindowModel.leftMeter` / `rightMeter`).
private struct LevelMeterRepresentable: NSViewRepresentable {
    let register: (LevelMeterView) -> Void
    func makeNSView(context: Context) -> LevelMeterView {
        let v = LevelMeterView()
        register(v)
        return v
    }
    func updateNSView(_ v: LevelMeterView, context: Context) {}
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: LevelMeterView, context: Context) -> CGSize? {
        CGSize(width: LevelMeterView.width, height: LevelMeterView.height)
    }
}

/// Both channels, labelled L and R, as one accessible element (the label is refreshed by the model at
/// ~2 Hz while the peaks change).
struct LevelMeters: View {
    let model: WindowModel

    var body: some View {
        VStack(spacing: 2) {
            row("L") { model.leftMeter = $0 }
            row("R") { model.rightMeter = $0 }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Audio level meters")
        .accessibilityValue(model.meterAccessibilityText)
    }

    private func row(_ name: String, _ register: @escaping (LevelMeterView) -> Void) -> some View {
        HStack(spacing: 6) {
            Text(name).font(.system(size: 9)).foregroundStyle(.secondary).frame(width: 10, alignment: .leading)
            LevelMeterRepresentable(register: register).frame(width: LevelMeterView.width, height: LevelMeterView.height)
        }
    }
}
