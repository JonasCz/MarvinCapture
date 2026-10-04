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

/// DV / HDV: Output card (shared name and folder, the selected kind's file options) and the Tape card
/// (deck controls, Manual capture, Automatic rewind & capture).
struct DvPanel: View {
    let model: WindowModel

    var body: some View {
        @Bindable var m = model
        Card {
            HStack {
                CardHeader("Output")
                Spacer()
                // Which kind's file options are shown below (the path is shared).
                Picker("File options for", selection: $m.selectedKindTabIndex) {
                    Text("DV").tag(0)
                    Text("HDV").tag(1)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 110)
                .help("Which file options are shown below: DV or HDV (the name and folder are shared)")
                .accessibilityLabel("File options for")
            }
            Group {
                OutputNameRow(
                    name: $m.dvName, directory: model.dvOutputDir,
                    nameLabel: "DV name and title",
                    nameHelp: "File name for DV and HDV (scenes and passes get numbered suffixes), also stored as the title metadata of the file",
                    folderLabel: "Choose DV output folder",
                    onChooseFolder: { model.chooseOutputFolder() })
                KindSettingsView(settings: model.kindTabs[Swift.min(Swift.max(model.selectedKindTabIndex, 0), 1)])
            }
            .disabled(!model.isIdle)
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("DV and HDV capture options")

        Divider()

        Card {
            HStack(spacing: 12) {
                CardHeader("Tape")
                // Greyed out while a deck command is pending.
                Text(model.deckStateText)
                    .font(.caption)
                    .foregroundStyle(model.deckBusy ? Color(nsColor: .tertiaryLabelColor) : Color(nsColor: .labelColor))
                    .lineLimit(1).truncationMode(.tail)
                    .help(model.deckTip)
            }
            DeckRow(model: model)
            // Secondary: record whatever arrives, without deck control.
            CaptureButton(
                title: model.manualCaptureTitle,
                symbol: model.isCapturing ? "stop.circle.fill" : "arrow.down.circle",
                destructive: model.isCapturing, help: model.manualCaptureHelp,
                action: { CaptureFlow.captureClicked(model) })
                .disabled(!model.playAndCaptureEnabled)
            // Primary, last: rewind, play, record. Turns into "Stop capture & stop tape" in place.
            CaptureButton(
                title: model.primaryDvTitle,
                symbol: model.isCapturing ? "stop.circle.fill" : "arrow.down.circle.fill",
                prominent: true, destructive: model.isCapturing, help: model.primaryDvHelp,
                action: { CaptureFlow.playAndCaptureClicked(model) })
                .disabled(!model.dvAutoCaptureEnabled)
        }
    }
}

/// Rewind, Play, Stop, Fast forward: lit while the deck reports that state, enabled by the core's rules.
struct DeckRow: View {
    let model: WindowModel

    var body: some View {
        HStack(spacing: 8) {
            button("backward.fill", "Rewind", checked: model.isRewChecked, enabled: model.deckRewEnabled, .rew)
            button("play.fill", "Play", checked: model.isPlayChecked, enabled: model.deckPlayEnabled, .play)
            button("stop.fill", "Stop", checked: model.isStopChecked, enabled: model.deckStopEnabled, .stop)
            button("forward.fill", "Fast forward", checked: model.isFfChecked, enabled: model.deckFfEnabled, .ff)
        }
    }

    private func button(_ symbol: String, _ name: String, checked: Bool, enabled: Bool, _ cmd: DeckCommand) -> some View {
        Toggle(isOn: Binding(get: { checked }, set: { _ in model.userRequestedDeck(cmd) })) {
            Image(systemName: symbol).frame(maxWidth: .infinity)
        }
        .toggleStyle(.button)
        .controlSize(.large)
        .disabled(!enabled)
        .help(name)
        .accessibilityLabel(name)
    }
}
