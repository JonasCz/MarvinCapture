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

/// The right-hand pane: a frame of the picture's aspect, as large as fits, centred; the window
/// background around it is the letterbox. Port of UpdatePreviewFrame / TrackPreviewAspect (the model
/// tracks the aspect, `Pin.fitRect` is the core's own fit rule).
struct PreviewArea: View {
    let model: WindowModel

    var body: some View {
        GeometryReader { geo in
            let dar = model.previewDar
            let fit = Pin.fitRect(darNum: dar.num, darDen: dar.den,
                                  width: Int(geo.size.width), height: Int(geo.size.height))
            ZStack {
                ZStack {
                    PreviewView(model: model)
                    if !model.previewHasVideo { NoVideoCard(model: model) }
                }
                .frame(width: CGFloat(Swift.max(1, fit.width)), height: CGFloat(Swift.max(1, fit.height)))
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .padding(MainView.padding)
    }
}

/// Why there is no picture: the core's sentence, the preparing progress, the no-signal countdown.
struct NoVideoCard: View {
    let model: WindowModel

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "video.slash")
                .font(.system(size: 48)).foregroundStyle(.tertiary)
                .accessibilityHidden(true)
            Text(model.noVideoText)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 420)
                .textSelection(.enabled)
            if model.progressVisible {
                Group {
                    if model.progressIndeterminate { ProgressView().progressViewStyle(.linear) }
                    else { ProgressView(value: model.progressValue, total: 100).progressViewStyle(.linear) }
                }
                .frame(width: 240)
                .accessibilityLabel("Preparing the device")
            }
            if model.noSignalCountdownVisible {
                VStack(spacing: 6) {
                    Text(model.noSignalCountdownText).fontWeight(.semibold)
                    ProgressView(value: model.noSignalCountdownFraction, total: 1)
                        .progressViewStyle(.linear)
                        .frame(width: 240)
                        .accessibilityLabel(model.noSignalCountdownText)
                }
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .controlBackgroundColor))
        .overlay(Rectangle().strokeBorder(.separator))
    }
}
