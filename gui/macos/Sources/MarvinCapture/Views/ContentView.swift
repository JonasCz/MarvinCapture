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

/// TEMPORARY debug view wired to WindowModel so the model layer can be exercised (device, input,
/// status texts, capture / stop, deck). Replaced by the real UI in the next step.
struct ContentView: View {
    @Bindable var app: AppModel

    private var model: WindowModel { app.window }

    var body: some View {
        @Bindable var m = app.window
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text("MarvinCapture (debug view)").font(.title2.bold())

                HStack {
                    Picker("Device", selection: $m.selectedDeviceID) {
                        Text("None").tag(String?.none)
                        ForEach(m.devices) { d in
                            Text("\(d.displayName) · \(d.statusText)").tag(String?.some(d.id)).disabled(!d.isUsable)
                        }
                    }
                    .disabled(!m.deviceSelectEnabled)
                    Button("Open") { m.openSelectedDevice() }.disabled(m.isCapturing || !m.hasSelectedDevice)
                    Button("Close") { m.closeSession() }.disabled(m.isCapturing || !m.hasSession)
                }
                if m.noDevices { Text(m.noDevicesHint).foregroundStyle(.secondary) }

                Picker("Input", selection: $m.inputIndex) {
                    Text("DV / HDV").tag(0)
                    Text("S-Video").tag(1)
                    Text("Composite").tag(2)
                }
                .pickerStyle(.segmented).disabled(m.isCapturing)

                if !m.isDvInput {
                    HStack {
                        Picker("Standard", selection: $m.selectedStandard) {
                            ForEach(m.standards) { s in Text(s.name).tag(StdItem?.some(s)) }
                        }
                        Picker("Format", selection: $m.analogFormat) {
                            ForEach(m.analogFormats) { f in Text(f.label).tag(FormatItem?.some(f)) }
                        }
                    }.disabled(m.isCapturing)
                    HStack {
                        TextField("Folder", text: $m.analogOutputDir)
                        TextField("Name", text: $m.analogName).frame(width: 160)
                    }.disabled(m.isCapturing)
                } else {
                    HStack {
                        TextField("Folder", text: $m.dvOutputDir)
                        TextField("Name", text: $m.dvName).frame(width: 160)
                    }.disabled(m.isCapturing)
                }

                Divider()
                Group {
                    line("State", m.sessionStateText)
                    line("Status", m.statusShortText)
                    line("No-video text", m.noVideoText)
                    line("Signal", "\(m.signalLockText) · \(m.signalTypeText)")
                    line("Frames", m.framesTotalText)
                    line("Sizes", "\(m.sizeText) \(m.storageFreeText)")
                    line("Deck", m.deckStateText)
                    line(m.statusTimeTip, m.statusTimeText)
                    line("Peaks", m.audioPeakText)
                    line("Title", m.windowTitle)
                    line("Log", m.lastLogLine)
                }

                HStack {
                    if !m.isDvInput {
                        Button(m.captureButtonText) { analogCapture() }.disabled(!m.captureEnabled)
                    } else {
                        Button(m.manualCaptureTitle) { dvManual() }.disabled(!m.playAndCaptureEnabled)
                        Button(m.primaryDvTitle) { dvAuto() }.disabled(!m.dvAutoCaptureEnabled)
                        Button("Rew") { m.userRequestedDeck(.rew) }.disabled(!m.deckRewEnabled)
                        Button("Play") { m.userRequestedDeck(.play) }.disabled(!m.deckPlayEnabled)
                        Button("Stop") { m.userRequestedDeck(.stop) }.disabled(!m.deckStopEnabled)
                        Button("FF") { m.userRequestedDeck(.ff) }.disabled(!m.deckFfEnabled)
                    }
                    Toggle("Muted", isOn: $m.isMuted)
                }

                if m.infoOpen {
                    HStack {
                        Text("\(m.infoTitle): \(m.infoMessage)").foregroundStyle(m.infoSeverity == .error ? .red : .orange)
                        Button("Dismiss") { m.dismissInfo() }
                    }
                }
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .alert(item: $app.alert) { a in Alert(title: Text(a.title), message: Text(a.message)) }
        .sheet(isPresented: $app.showHelp) { HelpSheet(app: app) }
    }

    private func line(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(.secondary).frame(width: 110, alignment: .trailing)
            Text(value).textSelection(.enabled)
        }
    }

    // MARK: capture flows (the real UI step replaces these dialogs)

    private func analogCapture() { model.isCapturing ? model.stopCapture(stopDeck: false) : startCapture(playFirst: false) }
    private func dvManual() { model.isCapturing ? model.stopCapture(stopDeck: false) : startCapture(playFirst: false) }
    private func dvAuto() { model.isCapturing ? model.stopCapture(stopDeck: true) : startCapture(playFirst: true) }

    private func startCapture(playFirst: Bool) {
        guard let plan = model.planCapture(playFirst: playFirst) else { return }
        for c in plan.confirmations {
            let a = NSAlert()
            a.messageText = c.title
            a.informativeText = c.message
            a.addButton(withTitle: c.primaryButton)
            a.addButton(withTitle: "Cancel")
            if a.runModal() != .alertFirstButtonReturn { return }
        }
        model.startCapture(plan.opts, overwrite: plan.overwrite)
    }
}

/// Command-line help (and a parse error) in a sheet.
struct HelpSheet: View {
    let app: AppModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Command-line options").font(.title3.bold())
            if let e = app.launchError {
                Text("Invalid command line: \(e)").foregroundStyle(.red)
            }
            ScrollView {
                Text(app.helpText).font(.system(size: 12, design: .monospaced)).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Button("Close") { dismiss() }.keyboardShortcut(.defaultAction)
        }
        .padding(20)
        .frame(width: 760, height: 520)
    }
}
