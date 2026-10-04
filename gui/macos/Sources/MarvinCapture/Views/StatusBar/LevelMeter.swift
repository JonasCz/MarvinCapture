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

/// Thin horizontal audio level meter: a bar for the current peak (dBFS), coloured by zone
/// (green, yellow near full scale, red at it), and a tick for the held peak of the last 10 s.
/// Port of Controls/LevelMeter.cs; drawn in a Canvas so a 30 Hz update stays cheap.
struct LevelMeter: View {
    static let floorDb = WindowModel.meterFloorDb
    static let width: CGFloat = 100
    static let height: CGFloat = 4

    let level: Double
    let hold: Double

    /// Zone limits (dBFS): green up to -18, yellow up to -6, red above.
    private static let yellowFrom = -18.0
    private static let redFrom = -6.0

    private static func fraction(_ db: Double) -> CGFloat {
        CGFloat(Swift.min(Swift.max((db - floorDb) / -floorDb, 0), 1))
    }

    var body: some View {
        Canvas { ctx, size in
            let w = size.width, h = size.height
            let track = Path(roundedRect: CGRect(x: 0, y: 0, width: w, height: h), cornerRadius: h / 2)
            ctx.fill(track, with: .color(.primary.opacity(0.12)))

            let barW = Self.fraction(level) * w
            if barW > 0 {
                var inner = ctx
                inner.clip(to: track)
                let zones: [(from: Double, to: Double, color: Color)] = [
                    (Self.floorDb, Self.yellowFrom, .green),
                    (Self.yellowFrom, Self.redFrom, .yellow),
                    (Self.redFrom, 0, .red),
                ]
                for z in zones {
                    let x0 = Self.fraction(z.from) * w
                    let x1 = Swift.min(Self.fraction(z.to) * w, barW)
                    if x1 > x0 { inner.fill(Path(CGRect(x: x0, y: 0, width: x1 - x0, height: h)), with: .color(z.color)) }
                }
            }
            if hold > Self.floorDb {
                // The tick turns red when the last 10 s came within 1 dB of clipping.
                let x = Swift.max(0, Self.fraction(hold) * w - 2)
                ctx.fill(Path(CGRect(x: x, y: 0, width: 2, height: h)), with: .color(hold > -1 ? .red : .primary))
            }
        }
        .frame(width: Self.width, height: Self.height)
    }
}

/// Both channels, labelled L and R, as one accessible element reading the peak text.
struct LevelMeters: View {
    let model: WindowModel

    var body: some View {
        VStack(spacing: 2) {
            row("L", model.audioPeakLeft, model.audioHoldLeft)
            row("R", model.audioPeakRight, model.audioHoldRight)
        }
        .help(model.audioPeakText)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(model.audioPeakText)
    }

    private func row(_ name: String, _ level: Double, _ hold: Double) -> some View {
        HStack(spacing: 6) {
            Text(name).font(.system(size: 9)).foregroundStyle(.secondary).frame(width: 10, alignment: .leading)
            LevelMeter(level: level, hold: hold)
        }
    }
}
