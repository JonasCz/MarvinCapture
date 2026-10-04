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

/// Device picker and the input selector.
struct SourceCard: View {
    let app: AppModel
    private var model: WindowModel { app.window }

    var body: some View {
        @Bindable var m = app.window
        Card {
            DevicePopUp(
                devices: model.devices,
                selection: model.selectedDeviceID,
                enabled: model.deviceSelectEnabled,
                placeholder: model.noDevices ? "No devices found" : "Select a device",
                onSelect: { model.selectedDeviceID = $0 })
                .frame(maxWidth: .infinity)
            // The device itself is in the pop-up; only the USB-hub warning is worth a line of its own.
            if let d = model.selectedDevice, d.isBehindHub { HubWarning(device: d) }
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

/// The hub warning of the selected device (the closed popup only has room for the name).
private struct HubWarning: View {
    let device: DeviceItem

    var body: some View {
        Text(device.hubLine)
            .font(.caption).foregroundStyle(.orange)
            .lineLimit(1)
            .truncationMode(.middle)
            .help(device.hubHint ?? "")
            .accessibilityLabel(device.accessibilityName)
    }
}
