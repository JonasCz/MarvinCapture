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

/// The gear next to "Retry bad sections": opens the retry settings in a popover.
struct RetryOptionsButton: View {
    let options: RetryOptionsModel
    let accessibilityName: String
    @State private var shown = false

    var body: some View {
        Button { shown = true } label: {
            Image(systemName: "gearshape").frame(width: 18)
        }
        .help("Retry settings")
        .accessibilityLabel(accessibilityName)
        .accessibilityHint("Attempts, memory window, preroll and what damage asks for a re-read")
        .popover(isPresented: $shown, arrowEdge: .bottom) {
            RetryOptionsView(options: options)
        }
    }
}

/// How the retry of bad sections works, in the gear's popover. Shared by the DV and HDV tabs; ranges and
/// help texts from the core. Locked while capturing. Port of RetryOptionsView.xaml.
struct RetryOptionsView: View {
    @Bindable var options: RetryOptionsModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Picker("Retry", selection: $options.modeIndex) {
                    Text("Automatic").tag(0)
                    Text("Each bad section (quickest)").tag(1)
                    Text("Whole tape, then re-read").tag(2)
                }
                .pickerStyle(.radioGroup)
                .help(options.modeHelp)
                if options.modeIndex == 2 {
                    // the whole tape: fewer rewinds, less deck wear
                    Text(options.wholeTapeNote).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            NumberField(title: "Attempts per section", value: $options.attempts, range: options.attemptsRange,
                        help: options.attemptsHelp, width: 52)
            NumberField(title: "Longest section (seconds of tape)", value: $options.windowSeconds,
                        range: options.windowRange, help: options.windowHelp, width: 64)
            NumberField(title: "Preroll (seconds)", value: $options.prerollSeconds, range: options.prerollRange,
                        help: options.prerollHelp, width: 52)
            Picker("Keep held tape in", selection: $options.storageIndex) {
                Text("Memory").tag(0)
                Text("Temporary file next to the output").tag(1)
            }
            .pickerStyle(.radioGroup)
            .help(options.storageHelp)
            VStack(alignment: .leading, spacing: 4) {
                Picker("Re-read on", selection: $options.triggerIndex) {
                    Text("Any damage").tag(0)
                    Text("Uncorrected errors only").tag(1)
                }
                .pickerStyle(.radioGroup)
                .help(options.triggerHelp)
                Text(options.triggerHelp).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .disabled(!options.isEditable)
        .padding(16)
        .frame(width: 360)
    }
}
