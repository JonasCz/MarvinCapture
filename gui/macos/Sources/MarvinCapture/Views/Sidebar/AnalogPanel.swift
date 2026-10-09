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

/// S-Video / Composite: Output card, Signal card (standard, picture adjustments, audio gain) and the
/// Capture button.
struct AnalogPanel: View {
    let model: WindowModel

    var body: some View {
        @Bindable var m = model
        Card {
            HStack {
                CardHeader("Output")
                Spacer()
                // Invisible copy of the DV panel's DV/HDV switch, so the header row is as tall there
                // and the content below lines up the same in both modes.
                Picker("", selection: .constant(0)) {
                    Text("DV").tag(0)
                    Text("HDV").tag(1)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 110)
                .hidden()
                .accessibilityHidden(true)
            }
            Group {
                OutputNameRow(
                    name: $m.analogName, directory: model.analogOutputDir,
                    nameLabel: "Analog name and title",
                    nameHelp: "File name (the extension follows the format), also stored as the title metadata of the file",
                    folderLabel: "Choose analog output folder",
                    onChooseFolder: { model.chooseOutputFolder() })
                HStack(alignment: .top, spacing: 8) {
                    Field("Format") {
                        Picker("Format", selection: $m.analogFormat) {
                            ForEach(model.analogFormats) { Text($0.label).tag(Optional($0)) }
                        }
                        .labelsHidden().accessibilityLabel("Analog format")
                    }
                    Field("Aspect") {
                        AspectPicker(selection: $m.analogAspectIndex, label: "Analog aspect ratio",
                                     help: "4:3, or 16:9 for anamorphic widescreen tapes. Affects the preview and the file.")
                    }
                    .frame(width: 104)
                }
                HStack(alignment: .top, spacing: 12) {
                    NumberField(title: "Stop no signal (min)", value: $m.analogIdleStopMinutes,
                                help: "Stops the capture after this many minutes without signal. 0 disables it (never stops automatically).", width: 76)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    NumberField(title: "Stop after (min)", value: $m.analogMaxDurationMinutes,
                                help: "Stops the capture after this many minutes, whether or not signal is present. 0 disables it (never stops automatically).", width: 76)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .disabled(!model.isIdle)
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Analog capture options")

        Divider()

        Card {
            HStack {
                CardHeader("Signal")
                Spacer()
                Picker("Video standard", selection: $m.selectedStandard) {
                    ForEach(model.standards) { Text($0.name).tag(Optional($0)) }
                }
                .labelsHidden()
                .frame(width: 150)
                .disabled(!model.isIdle)
                .help("Video standard. Auto picks PAL or NTSC from the 50 or 60 Hz signal")
                .accessibilityLabel("Video standard")
            }
            CardHeader("Picture adjustments").padding(.top, 2)
            VStack(spacing: 6) {
                ForEach(model.pictureSliders) { SliderRow(slider: $0) }
            }
            SliderRow(slider: model.audioGain, label: "Audio gain", accessibilityName: "Audio gain in decibels")

            // The primary action, last: Capture, which turns into Stop capture in place.
            CaptureButton(
                title: model.captureButtonText, caption: model.analogCaptureCaption,
                symbol: model.isCapturing ? "stop.circle.fill" : "arrow.down.circle.fill",
                prominent: true, destructive: model.isCapturing, help: model.analogCaptureHelp,
                action: { CaptureFlow.captureClicked(model) })
                .disabled(!model.captureEnabled)
        }
    }
}

/// Auto / 4:3 / 16:9 (pin_aspect_t order).
struct AspectPicker: View {
    @Binding var selection: Int
    let label: String
    let help: String

    var body: some View {
        Picker("Aspect", selection: $selection) {
            Text("Auto").tag(0)
            Text("4:3").tag(1)
            Text("16:9").tag(2)
        }
        .labelsHidden()
        .help(help)
        .accessibilityLabel(label)
    }
}

/// One slider with its label, value and reset button. Live: stays enabled while capturing.
struct SliderRow: View {
    @Bindable var slider: ControlSliderModel
    var label: String?
    var accessibilityName: String?

    var body: some View {
        let lo = Double(slider.min)
        let hi = Swift.max(Double(slider.max), lo + 1)   // before the core reported a range
        let step = Double(Swift.max(slider.step, 1))
        let snapped = Binding<Double>(
            get: { slider.sliderValue },
            set: { slider.sliderValue = (($0 - lo) / step).rounded() * step + lo })
        VStack(spacing: 0) {
            HStack {
                Text(label ?? slider.label)
                Spacer()
                Text(slider.valueText).font(.caption).monospacedDigit().foregroundStyle(.secondary)
            }
            HStack(spacing: 4) {
                Slider(value: snapped, in: lo...hi)   // no `step:`: it would draw a tick per step
                    .accessibilityLabel(accessibilityName ?? label ?? slider.label)
                Button { slider.resetToDefault() } label: {
                    Image(systemName: "arrow.counterclockwise").frame(width: 18, height: 18)
                }
                .buttonStyle(.borderless)
                .help("Reset to default")
                .accessibilityLabel(label.map { "Reset \($0.lowercased()) to default" } ?? slider.resetName)
            }
        }
        .disabled(!slider.isEnabled)
    }
}
