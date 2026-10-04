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

/// Device picker, the "…" menu and the input selector.
struct SourceCard: View {
    let app: AppModel
    private var model: WindowModel { app.window }

    var body: some View {
        @Bindable var m = app.window
        Card {
            HStack(spacing: 8) {
                DevicePopUp(
                    devices: model.devices,
                    selection: model.selectedDeviceID,
                    enabled: model.deviceSelectEnabled,
                    placeholder: model.noDevices ? "No devices found" : "Select a device",
                    onSelect: { model.selectedDeviceID = $0 })
                    .frame(maxWidth: .infinity)
                MoreMenu(app: app)
            }
            if let d = model.selectedDevice { DeviceCaption(device: d) }
            if model.noDevices {
                Text(model.noDevicesHint)
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Picker("Input", selection: $m.inputIndex) {
                Text("DV / HDV").tag(0)
                Text("S-Video").tag(1)
                Text("Composite").tag(2)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .disabled(!model.isIdle)
            .accessibilityLabel("Input")
            .help("DV / HDV: FireWire camera or deck, DV and HDV are detected automatically")
        }
    }
}

/// The id, status and hub warning of the selected device (the closed popup only has room for the name).
private struct DeviceCaption: View {
    let device: DeviceItem

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text("\(device.shortId) · \(device.statusText)")
                .foregroundStyle(.secondary)
            if device.isBehindHub {
                Text(device.hubLine).foregroundStyle(.orange)
                    .help(device.hubHint ?? "")
            }
        }
        .font(.caption)
        .lineLimit(1)
        .truncationMode(.middle)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(device.accessibilityName)
    }
}

/// "…": New Window, Command-line Help, About.
private struct MoreMenu: View {
    let app: AppModel

    var body: some View {
        Menu {
            Button {
                NewWindow.open { error in
                    if let error { app.window.showInfo("Could not open a new window", error, .error) }
                }
            } label: { Label("New Window", systemImage: "macwindow.badge.plus") }
                .help("Opens another capture window, for example for a second device")
            Button { app.openHelpFromMenu() } label: { Label("Command-line Help", systemImage: "terminal") }
            Divider()
            Button { About.show() } label: { Label("About MarvinCapture", systemImage: "info.circle") }
        } label: {
            Image(systemName: "ellipsis.circle").font(.system(size: 16))
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help("More options")
        .accessibilityLabel("More options")
    }
}
