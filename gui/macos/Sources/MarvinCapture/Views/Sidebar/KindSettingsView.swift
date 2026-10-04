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

/// One kind's (DV or HDV) file options: format, aspect, scene split, the two minute limits, passes.
/// Port of KindSettingsView.xaml. The caller disables the whole view while capturing.
struct KindSettingsView: View {
    @Bindable var settings: KindSettingsModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 8) {
                Field("Format") {
                    Picker("Format", selection: $settings.selectedFormat) {
                        ForEach(settings.formats) { f in
                            Text("\(f.label)  (.\(f.fileExtension.trimmingCharacters(in: CharacterSet(charactersIn: "."))))")
                                .tag(Optional(f))
                        }
                    }
                    .labelsHidden()
                    .accessibilityLabel("\(settings.tabHeader) format")
                }
                Field("Aspect") {
                    AspectPicker(selection: $settings.aspectIndex, label: "\(settings.tabHeader) aspect ratio",
                                 help: "Overrides the aspect ratio for the preview and the file. Auto uses what the tape reports.")
                }
                .frame(width: 104)
            }
            Toggle("Split into scenes", isOn: $settings.splitIntoScenes)
                .disabled(!settings.splitEnabled)
                .help("Starts a new numbered file at every recording start, timecode jump or date change on the tape.")
            HStack(alignment: .top, spacing: 12) {
                NumberField(title: "Stop no signal (min)", value: $settings.idleStopMinutes,
                            help: "Ends the pass after this many minutes without signal or data. With passes left, rewinds and starts the next one. 0 disables it (never stops automatically).")
                    .frame(maxWidth: .infinity, alignment: .leading)
                NumberField(title: "Stop after (min)", value: $settings.maxDurationMinutes,
                            help: "Ends the pass after this many minutes of capture, per pass, excluding rewind time (the count restarts with each pass), whether or not signal is present. With passes left, rewinds and starts the next one; in the last pass it ends the capture and stops the tape. 0 disables it.")
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            NumberField(title: "Capture passes", value: $settings.displayPasses, range: 1...20,
                        help: settings.passesToolTip, width: 56)
                .disabled(!settings.passesUsable)
        }
    }
}
