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

/// One kind's (DV or HDV) file options: format (its settings behind the gear), aspect, scene split, the two
/// minute limits, the retry of bad sections (its settings behind the gear). Port of KindSettingsView.xaml.
/// The caller disables the whole view while capturing.
struct KindSettingsView: View {
    @Bindable var settings: KindSettingsModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 8) {
                Field("Format") {
                    HStack(spacing: 4) {
                        Picker("Format", selection: $settings.selectedFormat) {
                            ForEach(settings.formats) { f in
                                Text("\(f.label)  (.\(f.fileExtension.trimmingCharacters(in: CharacterSet(charactersIn: "."))))")
                                    .tag(Optional(f))
                            }
                        }
                        .labelsHidden()
                        .accessibilityLabel("\(settings.tabHeader) format")
                        .help(settings.formatHelp)
                        // the selected format's settings (deinterlace, quality, FFV1 compression), if it has any
                        FormatOptionsButton(options: settings.formatOptions,
                                            accessibilityName: "\(settings.tabHeader) format settings")
                    }
                }
                Field("Aspect") {
                    // disabled for a format that stores no aspect (raw .dv / .m2t): the choice is kept, Auto applies
                    AspectPicker(selection: $settings.aspectIndex, label: "\(settings.tabHeader) aspect ratio",
                                 help: settings.aspectHelp)
                        .disabled(!settings.aspectEnabled)
                }
                .frame(width: 104)
            }
            // the core's warning for the selected format (H.264 / HEVC: lossy)
            if !settings.formatOptions.warning.isEmpty {
                InfoBannerContent(severity: .warning, title: "", message: settings.formatOptions.warning)
            }
            Toggle("Split into scenes", isOn: $settings.splitChecked)
                .disabled(!settings.splitEnabled)
                .help(settings.splitHelp)
            HStack(alignment: .top, spacing: 8) {
                NumberField(title: "No signal (min)", value: $settings.idleStopMinutes,
                            help: "Ends the capture after this many minutes without signal or data (the end of the tape). The time a retry spends winding back does not count. 0 disables it (never stops automatically).",
                            width: 64, accessibilityName: "Stop no signal (min)")
                    .frame(maxWidth: .infinity, alignment: .leading)
                NumberField(title: "Stop after (min)", value: $settings.maxDurationMinutes,
                            help: "Ends the capture and stops the tape after this many minutes of capture, whether or not signal is present. The time a retry spends winding back and re-reading does not count. 0 disables it.", width: 64)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            // Retry bad sections, its settings behind the gear (shared by DV and HDV)
            HStack(spacing: 4) {
                Toggle("Retry bad sections", isOn: $settings.retry)
                    .help(settings.retryHelp)
                    .accessibilityLabel("\(settings.tabHeader): retry bad sections")
                RetryOptionsButton(options: settings.retryOptions,
                                   accessibilityName: "\(settings.tabHeader) retry settings")
            }
            .disabled(!settings.retryEnabled)
        }
    }
}
