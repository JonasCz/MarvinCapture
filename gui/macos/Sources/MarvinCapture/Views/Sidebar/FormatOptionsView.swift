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
import CMarvinCore

/// The gear right of a format picker: opens the format's settings in a popover; disabled when the
/// selected format has none (raw DV, HDV, ...). The caller disables it while capturing.
struct FormatOptionsButton: View {
    let options: FormatOptionsModel
    let accessibilityName: String
    @State private var shown = false

    var body: some View {
        Button { shown = true } label: {
            Image(systemName: "gearshape").frame(width: 18)
        }
        .disabled(!options.options.hasAny)
        .help("Format settings")
        .accessibilityLabel(accessibilityName)
        .accessibilityHint("Deinterlacing, quality or compression of the selected format, where it has any")
        .popover(isPresented: $shown, arrowEdge: .bottom) {
            FormatOptionsView(options: options)
        }
    }
}

/// The settings of one output format, in the gear's popover. Built from the core's pin_format_options() for
/// the selected format: only the parts whose has_* flag is set, with the core's labels, range and help texts.
/// Locked while capturing. Port of FormatOptionsView.xaml.
struct FormatOptionsView: View {
    @Bindable var options: FormatOptionsModel

    var body: some View {
        let o = options.options
        VStack(alignment: .leading, spacing: 14) {
            if !o.note.isEmpty {
                Text(o.note).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if o.hasDeinterlace {
                // two choices in pin_deinterlace_t order
                Field(o.deinterlaceLabel) {
                    VStack(alignment: .leading, spacing: 8) {
                        Picker(o.deinterlaceLabel, selection: $options.deinterlace) {
                            Text(o.keepFieldsLabel).tag(Int(PIN_DEINTERLACE_KEEP_FIELDS.rawValue))
                            Text(o.doubleRateLabel).tag(Int(PIN_DEINTERLACE_DOUBLE_RATE.rawValue))
                        }
                        .pickerStyle(.radioGroup)
                        .labelsHidden()
                        .accessibilityLabel(o.deinterlaceLabel)
                        if options.keepFieldsChosen {
                            InfoBannerContent(severity: .warning, title: "", message: o.keepFieldsHelp)
                        } else {
                            Text(o.doubleRateHelp).font(.caption).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            }

            if o.hasQuality {
                // lower is better; the data-rate line follows the slider
                let lo = Double(o.qualityMin)
                let hi = Swift.max(Double(o.qualityMax), lo + 1)
                let value = Binding<Double>(
                    get: { Double(options.quality) },
                    set: { options.quality = Int($0.rounded()) })
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(o.qualityLabel)
                        Spacer()
                        Text(String(options.quality)).font(.caption).monospacedDigit().foregroundStyle(.secondary)
                    }
                    Slider(value: value, in: lo...hi)   // no `step:`: it would draw a tick per step
                        .accessibilityLabel(o.qualityLabel)
                        .accessibilityValue(String(options.quality))
                    HStack {
                        Text(o.qualityLowLabel)
                        Spacer()
                        Text(o.qualityHighLabel)
                    }
                    .font(.caption).foregroundStyle(.tertiary)
                    Text(options.qualityHint).padding(.top, 4)
                    Text(o.qualityHelp).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            if o.hasFfv1 {
                // three levels in pin_ffv1_compression_t order, all lossless
                Field(o.ffv1Label) {
                    VStack(alignment: .leading, spacing: 8) {
                        Picker(o.ffv1Label, selection: $options.ffv1) {
                            ForEach(Array(o.ffv1LevelLabels.enumerated()), id: \.offset) { i, label in
                                Text(label).tag(i)
                            }
                        }
                        .pickerStyle(.radioGroup)
                        .labelsHidden()
                        .accessibilityLabel(o.ffv1Label)
                        Text(o.ffv1Help).font(.caption).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
        .padding(16)
        .frame(width: 340, alignment: .leading)
        .disabled(!options.isEditable)
    }
}
